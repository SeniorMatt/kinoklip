// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEncoder — encoder setup per codec/backend (pixel format, rate control, speed preset,
// colour tags) and frame conversion.
#include "export/VideoEncoder.h"

#include "export/ExportLog.h"
#include "export/RgbToYuv.h"

#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>

namespace kk::exporter {

namespace {

/// GOP length in seconds for long-GOP codecs (seekable deliverables, typical platform advice).
constexpr double kGopSeconds = 2.0;
/// Bitrate mode: peak rate and VBV buffer relative to the average (constrained VBR).
constexpr double kMaxRateFactor = 1.5;
constexpr double kBufferFactor = 2.0;
/// VAAPI surface pool size (encoder references + lookahead).
constexpr int kVaapiPoolSize = 16;

/// QImage::Format_ARGB32(_Premultiplied) in memory: 0xAARRGGBB native-endian words, which is
/// exactly FFmpeg's AV_PIX_FMT_RGB32 (BGRA bytes on little endian).
constexpr AVPixelFormat kImageFormat = AV_PIX_FMT_RGB32;

/// swscale slice threads: half the cores (the encoder and compositor use the rest), 1..8.
int swsThreads()
{
    return std::clamp(QThread::idealThreadCount() / 2, 1, 8);
}

bool isYuvCodec(const QString& codec)
{
    return codec != QLatin1String("png") && codec != QLatin1String("tiff")
        && codec != QLatin1String("exr") && codec != QLatin1String("gif");
}

/// Preferred software-side pixel formats for a codec, best first.
std::vector<AVPixelFormat> preferredFormats(const VideoEncoderConfig& c)
{
    const QString& codec = c.settings.codec;
    if (c.backend == EncoderBackend::Vaapi || c.backend == EncoderBackend::Qsv) {
        return {AV_PIX_FMT_NV12};
    }
    if (codec == QLatin1String("prores")) {
        if (c.settings.profile.startsWith(QLatin1String("4444"))) {
            return c.alpha ? std::vector {AV_PIX_FMT_YUVA444P10LE}
                           : std::vector {AV_PIX_FMT_YUV444P10LE};
        }
        return {AV_PIX_FMT_YUV422P10LE};
    }
    if (codec == QLatin1String("dnxhr")) {
        return c.settings.profile == QLatin1String("hqx") ? std::vector {AV_PIX_FMT_YUV422P10LE}
                                                          : std::vector {AV_PIX_FMT_YUV422P};
    }
    if (codec == QLatin1String("png") || codec == QLatin1String("tiff")) {
        return c.alpha ? std::vector {AV_PIX_FMT_RGBA} : std::vector {AV_PIX_FMT_RGB24};
    }
    if (codec == QLatin1String("exr")) {
        return c.alpha ? std::vector {AV_PIX_FMT_GBRAPF32LE} : std::vector {AV_PIX_FMT_GBRPF32LE};
    }
    if (codec == QLatin1String("gif")) {
        return {AV_PIX_FMT_PAL8};
    }
    if (codec == QLatin1String("mjpeg")) {
        // Full-range 4:2:0; old FFmpeg versions only take the deprecated "J" formats.
        return {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P};
    }
    return {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12};
}

AVPixelFormat choosePixelFormat(const AVCodec* codec, const VideoEncoderConfig& c)
{
    const std::vector<AVPixelFormat> wanted = preferredFormats(c);
    const std::vector<AVPixelFormat> supported = supportedPixelFormats(codec);
    if (supported.empty()) {
        return wanted.front();
    }
    for (AVPixelFormat f : wanted) {
        if (std::find(supported.begin(), supported.end(), f) != supported.end()) {
            return f;
        }
    }
    return AV_PIX_FMT_NONE;
}

/// Speed presets per encoder family ("fast" / "medium" / "slow").
int speedIndex(const QString& speed)
{
    if (speed == QLatin1String("fast")) {
        return 0;
    }
    if (speed == QLatin1String("slow")) {
        return 2;
    }
    return 1;
}

void setOpt(AVDictionary** dict, const char* key, const QByteArray& value)
{
    av_dict_set(dict, key, value.constData(), 0);
}

void setOpt(AVDictionary** dict, const char* key, long long value)
{
    av_dict_set_int(dict, key, value, 0);
}

/// Encoder-specific options: speed preset, rate control, profile. The generic fields
/// (bit_rate, gop, colour) are set on the context directly.
void configureEncoder(AVCodecContext* ctx, AVDictionary** opts, const VideoEncoderConfig& c)
{
    const QString& name = c.encoderName;
    const VideoExportSettings& s = c.settings;
    const int speed = speedIndex(s.speed);
    const bool quality = s.rateControl == RateControl::Quality;
    const int64_t bitrate = int64_t(s.bitrateKbps) * 1000;
    if (quality) {
        ctx->bit_rate = 0; // the AVCodecContext default (200 kb/s) would cap CQ modes
    } else {
        ctx->bit_rate = bitrate;
        ctx->rc_max_rate = int64_t(double(bitrate) * kMaxRateFactor);
        ctx->rc_buffer_size = int(std::min<double>(double(bitrate) * kBufferFactor, INT32_MAX));
    }

    if (name == QLatin1String("libx264") || name == QLatin1String("libx265")) {
        static const char* x264[] = {"veryfast", "medium", "slow"};
        static const char* x265[] = {"veryfast", "fast", "medium"}; // x265 is ~4x slower
        setOpt(opts, "preset", name == QLatin1String("libx264") ? x264[speed] : x265[speed]);
        if (quality) {
            setOpt(opts, "crf", QByteArray::number(s.quality));
        }
        if (name == QLatin1String("libx265")) {
            setOpt(opts, "x265-params", QByteArray("log-level=error"));
            ctx->codec_tag = MKTAG('h', 'v', 'c', '1'); // what Apple players require in MP4/MOV
        }
    } else if (name.endsWith(QLatin1String("_nvenc"))) {
        static const char* presets[] = {"p2", "p5", "p7"};
        setOpt(opts, "preset", presets[speed]);
        setOpt(opts, "tune", QByteArray("hq"));
        setOpt(opts, "rc", QByteArray("vbr"));
        if (quality) {
            setOpt(opts, "cq", QByteArray::number(std::max(1, s.quality)));
        }
        if (name.startsWith(QLatin1String("hevc"))) {
            ctx->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
    } else if (name.endsWith(QLatin1String("_vaapi"))) {
        // rc_mode "auto": with a bitrate VBR; without, ICQ (Intel) or CQP using
        // global_quality as the quality/QP value (see vaapi_encode_init_rate_control()).
        if (quality) {
            ctx->global_quality = std::max(1, s.quality);
        }
        if (c.lowPower) {
            setOpt(opts, "low_power", 1);
        }
        if (name.startsWith(QLatin1String("hevc"))) {
            ctx->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
    } else if (name.endsWith(QLatin1String("_qsv"))) {
        static const char* presets[] = {"veryfast", "medium", "veryslow"};
        setOpt(opts, "preset", presets[speed]);
        if (quality) {
            ctx->global_quality = std::max(1, s.quality);
        }
        if (name.startsWith(QLatin1String("hevc"))) {
            ctx->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
    } else if (name == QLatin1String("libsvtav1")) {
        static const int presets[] = {10, 8, 6};
        setOpt(opts, "preset", presets[speed]);
        if (quality) {
            setOpt(opts, "crf", s.quality);
        }
        // SVT-AV1 prints its banner straight to stderr unless SVT_LOG says otherwise.
        if (qEnvironmentVariableIsEmpty("SVT_LOG")) {
            qputenv("SVT_LOG", "1");
        }
    } else if (name == QLatin1String("libaom-av1")) {
        static const int cpu[] = {8, 6, 4};
        setOpt(opts, "cpu-used", cpu[speed]);
        setOpt(opts, "row-mt", 1);
        if (quality) {
            setOpt(opts, "crf", s.quality);
        }
    } else if (name == QLatin1String("libvpx-vp9")) {
        static const int cpu[] = {6, 4, 2};
        setOpt(opts, "deadline", QByteArray("good"));
        setOpt(opts, "cpu-used", cpu[speed]);
        setOpt(opts, "row-mt", 1);
        if (quality) {
            setOpt(opts, "crf", s.quality);
        }
    } else if (name == QLatin1String("prores_ks")) {
        setOpt(opts, "profile", s.profile.isEmpty() ? QByteArray("hq") : s.profile.toLatin1());
        setOpt(opts, "vendor", QByteArray("apl0")); // what QuickTime/FCP expect
        ctx->bit_rate = 0;
    } else if (name == QLatin1String("dnxhd")) {
        setOpt(opts, "profile",
            QByteArray("dnxhr_") + (s.profile.isEmpty() ? QByteArray("hq") : s.profile.toLatin1()));
        ctx->bit_rate = 0;
    } else if (name == QLatin1String("mjpeg")) {
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = std::clamp(s.quality, 2, 31) * FF_QP2LAMBDA;
        ctx->bit_rate = 0;
    } else if (name == QLatin1String("tiff")) {
        setOpt(opts, "compression_algo", QByteArray("deflate"));
    } else if (name == QLatin1String("exr")) {
        setOpt(opts, "compression", QByteArray("zip16"));
        setOpt(opts, "format", QByteArray("half"));
    } else if (name == QLatin1String("png")) {
        // Default zlib level 9 is ~3x slower than 3 for a few % size; renders are big.
        ctx->compression_level = 3;
    }
}

} // namespace

VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::open(const VideoEncoderConfig& config, QString* error)
{
    initExportFfmpeg();
    m_config = config;
    auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        m_ctx.reset();
        return false;
    };
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoderName.toLatin1().constData());
    if (!codec) {
        return fail(QStringLiteral("encoder %1 is not available in this FFmpeg build")
                        .arg(config.encoderName));
    }
    m_ctx.reset(avcodec_alloc_context3(codec));
    AVCodecContext* ctx = m_ctx.get();
    ctx->width = config.size.width();
    ctx->height = config.size.height();
    ctx->time_base = AVRational {config.fps.den, config.fps.num};
    ctx->framerate = AVRational {config.fps.num, config.fps.den};
    ctx->sample_aspect_ratio = AVRational {1, 1};
    ctx->gop_size = config.settings.keyframeInterval > 0
        ? config.settings.keyframeInterval
        : std::max(1, int(std::lround(config.fps.toDouble() * kGopSeconds)));
    if (!config.settings.frameReordering)
        ctx->max_b_frames = 0; // honoured by x264/x265/SVT-AV1/libvpx and the hw encoders
    ctx->thread_count = 0; // automatic (software encoders)

    m_swFormat = choosePixelFormat(codec, config);
    if (m_swFormat == AV_PIX_FMT_NONE) {
        return fail(QStringLiteral("encoder %1 supports none of the pixel formats Kinoklip uses")
                        .arg(config.encoderName));
    }
    ctx->pix_fmt = m_swFormat;

    // Colour tags: HD and UHD deliverables are BT.709 (the compositor's output is sRGB-ish
    // gamma-encoded RGB, which is what BT.709 players show); JPEG is full-range BT.601 by
    // convention; RGB codecs carry no matrix.
    const QString& codecId = config.settings.codec;
    const bool jpeg = codecId == QLatin1String("mjpeg");
    if (isYuvCodec(codecId)) {
        ctx->color_range = jpeg ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        ctx->colorspace = jpeg ? AVCOL_SPC_BT470BG : AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = jpeg ? AVCOL_TRC_IEC61966_2_1 : AVCOL_TRC_BT709;
    } else {
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->colorspace = AVCOL_SPC_RGB;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc
            = codecId == QLatin1String("exr") ? AVCOL_TRC_LINEAR : AVCOL_TRC_IEC61966_2_1;
    }

    if (config.backend == EncoderBackend::Vaapi) {
        if (!config.hwDevice) {
            return fail(QStringLiteral("no VAAPI device"));
        }
        m_hwFrames.reset(av_hwframe_ctx_alloc(config.hwDevice));
        if (!m_hwFrames) {
            return fail(QStringLiteral("cannot create VAAPI frames context"));
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(m_hwFrames->data);
        frames->format = AV_PIX_FMT_VAAPI;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = ctx->width;
        frames->height = ctx->height;
        frames->initial_pool_size = kVaapiPoolSize;
        if (int err = av_hwframe_ctx_init(m_hwFrames.get()); err < 0) {
            return fail(QStringLiteral("VAAPI surfaces %1x%2: %3")
                            .arg(ctx->width)
                            .arg(ctx->height)
                            .arg(avError(err)));
        }
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        ctx->hw_frames_ctx = av_buffer_ref(m_hwFrames.get());
    }

    AVDictionary* opts = nullptr;
    configureEncoder(ctx, &opts, config);
    if (config.globalHeader) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    const int err = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        return fail(QStringLiteral("cannot open %1 at %2x%3: %4")
                        .arg(config.encoderName)
                        .arg(ctx->width)
                        .arg(ctx->height)
                        .arg(avError(err)));
    }

    m_packet.reset(av_packet_alloc());
    if (codecId == QLatin1String("gif")) {
        m_filtered.reset(av_frame_alloc());
        return openGifGraph(error) || fail(error ? *error : QString());
    }

    m_frame.reset(av_frame_alloc());
    m_frame->format = m_swFormat;
    m_frame->width = ctx->width;
    m_frame->height = ctx->height;
    if (av_frame_get_buffer(m_frame.get(), 0) < 0) {
        return fail(QStringLiteral("out of memory"));
    }
    if (config.backend == EncoderBackend::Vaapi) {
        m_hwFrame.reset(av_frame_alloc());
    }

    if (codecId == QLatin1String("exr")) {
        // sRGB decoding curve (IEC 61966-2-1): EXR stores linear light.
        m_srgbToLinear.resize(256);
        for (int i = 0; i < 256; ++i) {
            const double v = i / 255.0;
            m_srgbToLinear[size_t(i)]
                = float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
        return true;
    }

    m_fastYuv = (m_swFormat == AV_PIX_FMT_YUV420P || m_swFormat == AV_PIX_FMT_NV12) && !jpeg
        && !config.alpha && ctx->width % 2 == 0 && ctx->height % 2 == 0;
    if (m_fastYuv) {
        m_pool = std::make_unique<QThreadPool>();
        m_pool->setMaxThreadCount(swsThreads());
        return true;
    }

    SwsContext* sws = sws_alloc_context();
    av_opt_set_int(sws, "srcw", ctx->width, 0);
    av_opt_set_int(sws, "srch", ctx->height, 0);
    av_opt_set_int(sws, "src_format", kImageFormat, 0);
    av_opt_set_int(sws, "dstw", ctx->width, 0);
    av_opt_set_int(sws, "dsth", ctx->height, 0);
    av_opt_set_int(sws, "dst_format", m_swFormat, 0);
    // Same size: the filter only matters for chroma subsampling; bilinear averages the 2x2
    // chroma block (point sampling would alias). ACCURATE_RND avoids a +-1 bias in YUV.
    av_opt_set_int(sws, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND, 0);
    av_opt_set_int(sws, "threads", swsThreads(), 0);
    if (sws_init_context(sws, nullptr, nullptr) < 0) {
        sws_freeContext(sws);
        return fail(QStringLiteral("swscale cannot convert to %1")
                        .arg(QString::fromLatin1(av_get_pix_fmt_name(m_swFormat))));
    }
    m_sws.reset(sws);
    if (isYuvCodec(codecId)) {
        const int* srcMatrix = sws_getCoefficients(SWS_CS_DEFAULT);
        const int* dstMatrix = sws_getCoefficients(jpeg ? SWS_CS_ITU601 : SWS_CS_ITU709);
        // brightness 0, contrast 1.0, saturation 1.0 in 16.16 fixed point
        sws_setColorspaceDetails(sws, srcMatrix, 1, dstMatrix, jpeg ? 1 : 0, 0, 1 << 16, 1 << 16);
    }
    return true;
}

bool VideoEncoder::openGifGraph(QString* error)
{
    // buffer (RGB32) → split → palettegen (one palette for the whole clip, from every frame)
    //                       └→ paletteuse (waits for the palette, then dithers each frame)
    // palettegen only emits at EOF, so split buffers all frames: keep GIFs short and small.
    AVCodecContext* ctx = m_ctx.get();
    m_graph.reset(avfilter_graph_alloc());
    const QByteArray args
        = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=1/1")
              .arg(ctx->width)
              .arg(ctx->height)
              .arg(QString::fromLatin1(av_get_pix_fmt_name(kImageFormat)))
              .arg(ctx->time_base.num)
              .arg(ctx->time_base.den)
              .toLatin1();
    int err = avfilter_graph_create_filter(&m_bufferSrc, avfilter_get_by_name("buffer"), "in",
        args.constData(), nullptr, m_graph.get());
    if (err >= 0) {
        err = avfilter_graph_create_filter(&m_bufferSink, avfilter_get_by_name("buffersink"), "out",
            nullptr, nullptr, m_graph.get());
    }
    if (err < 0) {
        if (error) {
            *error = QStringLiteral("GIF filter graph: %1").arg(avError(err));
        }
        return false;
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_bufferSrc;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_bufferSink;
    const char* chain = "split[a][b];[a]palettegen=stats_mode=full[p];"
                        "[b][p]paletteuse=dither=sierra2_4a";
    err = avfilter_graph_parse_ptr(m_graph.get(), chain, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (err >= 0) {
        err = avfilter_graph_config(m_graph.get(), nullptr);
    }
    if (err < 0) {
        if (error) {
            *error = QStringLiteral("GIF filter graph: %1").arg(avError(err));
        }
        return false;
    }
    return true;
}

bool VideoEncoder::convert(const QImage& input)
{
    // Alpha outputs need straight alpha (the compositor delivers premultiplied); opaque
    // outputs drop alpha, and premultiplied-over-black is exactly the RGB channels.
    const QImage* source = &input;
    if (m_config.alpha) {
        m_straight = input.convertToFormat(QImage::Format_ARGB32);
        source = &m_straight;
    }
    if (av_frame_make_writable(m_frame.get()) < 0) {
        return false;
    }
    if (!m_srgbToLinear.empty()) {
        convertExr(*source);
        return true;
    }
    if (m_fastYuv) {
        // Bands of an even number of rows; band 0 runs here, the rest on the pool.
        AVFrame* f = m_frame.get();
        Yuv420Planes planes {f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
            f->linesize[2], m_swFormat == AV_PIX_FMT_NV12};
        const uint8_t* bits = source->constBits();
        const int stride = int(source->bytesPerLine());
        const int width = f->width;
        const int height = f->height;
        const int bands = m_pool->maxThreadCount() + 1;
        const int bandRows = ((height / bands) + 1) & ~1;
        for (int b = 1; b < bands; ++b) {
            const int begin = std::min(height, b * bandRows);
            const int end = b + 1 == bands ? height : std::min(height, (b + 1) * bandRows);
            if (begin < end) {
                m_pool->start([=] { rgb32ToYuv420(bits, stride, width, begin, end, planes); });
            }
        }
        rgb32ToYuv420(bits, stride, width, 0, std::min(height, bandRows), planes);
        m_pool->waitForDone();
        return true;
    }
    // Wrap the QImage memory in an AVFrame without copying.
    AVFrame src {};
    src.format = kImageFormat;
    src.width = source->width();
    src.height = source->height();
    src.data[0] = const_cast<uint8_t*>(source->constBits());
    src.linesize[0] = int(source->bytesPerLine());
    return sws_scale_frame(m_sws.get(), m_frame.get(), &src) >= 0;
}

void VideoEncoder::convertExr(const QImage& image)
{
    // GBR(A) planar float, linear light; EXR convention is premultiplied alpha, so linearise
    // the straight colour and multiply by alpha again. Opaque pixels skip the division.
    const float* lut = m_srgbToLinear.data();
    const bool alpha = m_config.alpha;
    AVFrame* f = m_frame.get();
    for (int y = 0; y < image.height(); ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        auto* g = reinterpret_cast<float*>(f->data[0] + y * f->linesize[0]);
        auto* b = reinterpret_cast<float*>(f->data[1] + y * f->linesize[1]);
        auto* r = reinterpret_cast<float*>(f->data[2] + y * f->linesize[2]);
        float* a = alpha ? reinterpret_cast<float*>(f->data[3] + y * f->linesize[3]) : nullptr;
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = in[x];
            if (alpha) {
                const float av = float(qAlpha(px)) * (1.0f / 255.0f);
                r[x] = lut[qRed(px)] * av;
                g[x] = lut[qGreen(px)] * av;
                b[x] = lut[qBlue(px)] * av;
                a[x] = av;
            } else {
                r[x] = lut[qRed(px)];
                g[x] = lut[qGreen(px)];
                b[x] = lut[qBlue(px)];
            }
        }
    }
}

bool VideoEncoder::encode(const QImage& image, int64_t pts, const PacketSink& sink, QString* error)
{
    if (!m_ctx) {
        return false;
    }
    if (image.size() != QSize(m_ctx->width, m_ctx->height)) {
        if (error) {
            *error = QStringLiteral("frame size %1x%2 does not match the encoder")
                         .arg(image.width())
                         .arg(image.height());
        }
        return false;
    }
    if (m_graph) {
        const QImage straight = image.format() == QImage::Format_ARGB32_Premultiplied
            ? image.convertToFormat(QImage::Format_ARGB32)
            : image;
        AVFrame src {};
        src.format = kImageFormat;
        src.width = straight.width();
        src.height = straight.height();
        src.data[0] = const_cast<uint8_t*>(straight.constBits());
        src.linesize[0] = int(straight.bytesPerLine());
        src.pts = pts;
        // Not refcounted → buffersrc copies the pixels, so `straight` may go away.
        if (int err = av_buffersrc_write_frame(m_bufferSrc, &src); err < 0) {
            if (error) {
                *error = QStringLiteral("GIF filter: %1").arg(avError(err));
            }
            return false;
        }
        return drainGif(sink, error);
    }

    QElapsedTimer timer;
    timer.start();
    if (!convert(image)) {
        if (error) {
            *error = QStringLiteral("pixel conversion failed");
        }
        return false;
    }
    m_convertMs += double(timer.nsecsElapsed()) / 1e6;
    m_frame->pts = pts;
    AVFrame* toSend = m_frame.get();
    if (m_hwFrame) {
        av_frame_unref(m_hwFrame.get());
        int err = av_hwframe_get_buffer(m_hwFrames.get(), m_hwFrame.get(), 0);
        if (err >= 0) {
            err = av_hwframe_transfer_data(m_hwFrame.get(), m_frame.get(), 0);
        }
        if (err < 0) {
            if (error) {
                *error = QStringLiteral("upload to VAAPI surface: %1").arg(avError(err));
            }
            return false;
        }
        m_hwFrame->pts = pts;
        toSend = m_hwFrame.get();
    }
    return sendFrame(toSend, sink, error);
}

bool VideoEncoder::sendFrame(AVFrame* frame, const PacketSink& sink, QString* error)
{
    QElapsedTimer timer;
    timer.start();
    int err = avcodec_send_frame(m_ctx.get(), frame);
    if (err < 0 && err != AVERROR_EOF) {
        if (error) {
            *error = QStringLiteral("%1: %2").arg(m_config.encoderName, avError(err));
        }
        return false;
    }
    while (true) {
        err = avcodec_receive_packet(m_ctx.get(), m_packet.get());
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            break;
        }
        if (err < 0) {
            if (error) {
                *error = QStringLiteral("%1: %2").arg(m_config.encoderName, avError(err));
            }
            return false;
        }
        m_encodeMs += double(timer.nsecsElapsed()) / 1e6;
        const bool ok = sink(m_packet.get());
        av_packet_unref(m_packet.get());
        if (!ok) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("writing a video packet failed");
            }
            return false;
        }
        timer.restart();
    }
    m_encodeMs += double(timer.nsecsElapsed()) / 1e6;
    return true;
}

bool VideoEncoder::drainGif(const PacketSink& sink, QString* error)
{
    while (true) {
        const int err = av_buffersink_get_frame(m_bufferSink, m_filtered.get());
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            return true;
        }
        if (err < 0) {
            if (error) {
                *error = QStringLiteral("GIF filter: %1").arg(avError(err));
            }
            return false;
        }
        const bool ok = sendFrame(m_filtered.get(), sink, error);
        av_frame_unref(m_filtered.get());
        if (!ok) {
            return false;
        }
    }
}

bool VideoEncoder::flush(const PacketSink& sink, QString* error)
{
    if (!m_ctx) {
        return false;
    }
    if (m_graph) {
        if (int err = av_buffersrc_write_frame(m_bufferSrc, nullptr); err < 0) { // EOF
            if (error) {
                *error = QStringLiteral("GIF filter: %1").arg(avError(err));
            }
            return false;
        }
        if (!drainGif(sink, error)) {
            return false;
        }
    }
    return sendFrame(nullptr, sink, error);
}

} // namespace kk::exporter
