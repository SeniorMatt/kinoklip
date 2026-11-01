// SPDX-License-Identifier: GPL-3.0-or-later
// AudioEncoder — audio codec setup and the resample → FIFO → encode chain.
#include "export/AudioEncoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>

namespace kk::exporter {

namespace {

constexpr int kMixerRate = 48000; ///< AudioMixer output rate
constexpr int kPcmFrameSize = 1024; ///< chunk size for codecs without a fixed frame size

/// Best sample format of @p codec for our float source (float first, then wider ints).
AVSampleFormat chooseSampleFormat(const AVCodec* codec, const AudioExportSettings& s)
{
    std::vector<AVSampleFormat> wanted {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P};
    if (s.codec == QLatin1String("flac")) {
        wanted
            = s.bitDepth >= 24 ? std::vector {AV_SAMPLE_FMT_S32} : std::vector {AV_SAMPLE_FMT_S16};
    }
    const std::vector<AVSampleFormat> supported = supportedSampleFormats(codec);
    if (supported.empty()) {
        return wanted.front();
    }
    for (AVSampleFormat f : wanted) {
        if (std::find(supported.begin(), supported.end(), f) != supported.end()) {
            return f;
        }
    }
    return supported.front();
}

} // namespace

AudioEncoder::AudioEncoder() = default;

AudioEncoder::~AudioEncoder()
{
    if (m_convBuffer) {
        av_freep(&m_convBuffer[0]);
        av_freep(&m_convBuffer);
    }
}

bool AudioEncoder::open(const AudioExportSettings& settings, bool globalHeader, QString* error)
{
    initExportFfmpeg();
    auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        m_ctx.reset();
        return false;
    };
    const AudioCodecInfo* info = findAudioCodec(settings.codec);
    if (!info) {
        return fail(QStringLiteral("unknown audio codec %1").arg(settings.codec));
    }
    const AVCodec* codec = avcodec_find_encoder_by_name(info->encoder.toLatin1().constData());
    if (!codec) {
        return fail(QStringLiteral("audio encoder %1 is not available in this FFmpeg build")
                        .arg(info->encoder));
    }
    m_ctx.reset(avcodec_alloc_context3(codec));
    AVCodecContext* ctx = m_ctx.get();
    ctx->sample_rate = settings.sampleRate;
    const std::vector<int> rates = supportedSampleRates(codec);
    if (!rates.empty() && std::find(rates.begin(), rates.end(), ctx->sample_rate) == rates.end()) {
        return fail(
            QStringLiteral("%1 does not support %2 Hz").arg(info->name).arg(settings.sampleRate));
    }
    ctx->sample_fmt = chooseSampleFormat(codec, settings);
    av_channel_layout_default(&ctx->ch_layout, 2);
    ctx->time_base = AVRational {1, ctx->sample_rate};
    if (!info->lossless) {
        ctx->bit_rate = int64_t(settings.bitrateKbps) * 1000;
    }
    if (settings.codec == QLatin1String("flac") && settings.bitDepth >= 24) {
        ctx->bits_per_raw_sample = 24;
    }
    if (globalHeader) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (int err = avcodec_open2(ctx, codec, nullptr); err < 0) {
        return fail(QStringLiteral("cannot open %1: %2").arg(info->encoder, avError(err)));
    }
    m_frameSize = (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) || ctx->frame_size <= 0
        ? kPcmFrameSize
        : ctx->frame_size;

    AVChannelLayout stereo {};
    av_channel_layout_default(&stereo, 2);
    SwrContext* swr = nullptr;
    int err = swr_alloc_set_opts2(&swr, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, &stereo,
        AV_SAMPLE_FMT_FLT, kMixerRate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    if (err < 0 || swr_init(swr) < 0) {
        swr_free(&swr);
        return fail(QStringLiteral("cannot set up audio resampling"));
    }
    m_swr.reset(swr);
    m_fifo.reset(av_audio_fifo_alloc(ctx->sample_fmt, 2, m_frameSize * 4));
    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    m_nextPts = 0;
    return true;
}

bool AudioEncoder::pushConverted(int samples)
{
    return samples <= 0
        || av_audio_fifo_write(m_fifo.get(), reinterpret_cast<void**>(m_convBuffer), samples)
        == samples;
}

bool AudioEncoder::encode(
    const float* interleaved48k, int frames, const PacketSink& sink, QString* error)
{
    if (!m_ctx) {
        return false;
    }
    // Upper bound of output samples for this input (resampler delay included).
    const int capacity = int(av_rescale_rnd(swr_get_delay(m_swr.get(), kMixerRate) + frames,
                             m_ctx->sample_rate, kMixerRate, AV_ROUND_UP))
        + 16;
    if (capacity > m_convCapacity) {
        if (m_convBuffer) {
            av_freep(&m_convBuffer[0]);
            av_freep(&m_convBuffer);
        }
        if (av_samples_alloc_array_and_samples(
                &m_convBuffer, nullptr, 2, capacity, m_ctx->sample_fmt, 0)
            < 0) {
            m_convCapacity = 0;
            return false;
        }
        m_convCapacity = capacity;
    }
    const auto* in = reinterpret_cast<const uint8_t*>(interleaved48k);
    const int out = swr_convert(m_swr.get(), m_convBuffer, m_convCapacity, &in, frames);
    if (out < 0 || !pushConverted(out)) {
        if (error) {
            *error = QStringLiteral("audio conversion failed");
        }
        return false;
    }
    return encodeFromFifo(false, sink, error);
}

bool AudioEncoder::encodeFromFifo(bool final, const PacketSink& sink, QString* error)
{
    AVCodecContext* ctx = m_ctx.get();
    while (av_audio_fifo_size(m_fifo.get()) >= m_frameSize
        || (final && av_audio_fifo_size(m_fifo.get()) > 0)) {
        const int available = av_audio_fifo_size(m_fifo.get());
        const int n = std::min(available, m_frameSize);
        av_frame_unref(m_frame.get());
        m_frame->format = ctx->sample_fmt;
        m_frame->sample_rate = ctx->sample_rate;
        av_channel_layout_copy(&m_frame->ch_layout, &ctx->ch_layout);
        // Fixed-frame-size codecs (AAC, MP3, Opus) accept a shorter last frame only with this
        // capability; otherwise the tail is padded with silence (the muxer's duration still
        // ends at the real last sample through the packet duration / edit list).
        const bool shortOk = (ctx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)
            || (ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
        m_frame->nb_samples = (n < m_frameSize && !shortOk) ? m_frameSize : n;
        if (av_frame_get_buffer(m_frame.get(), 0) < 0) {
            return false;
        }
        if (m_frame->nb_samples > n) {
            av_samples_set_silence(m_frame->data, 0, m_frame->nb_samples, 2, ctx->sample_fmt);
        }
        if (av_audio_fifo_read(m_fifo.get(), reinterpret_cast<void**>(m_frame->data), n) != n) {
            return false;
        }
        m_frame->pts = m_nextPts;
        m_nextPts += n;
        if (!send(m_frame.get(), sink, error)) {
            return false;
        }
    }
    return true;
}

bool AudioEncoder::send(AVFrame* frame, const PacketSink& sink, QString* error)
{
    int err = avcodec_send_frame(m_ctx.get(), frame);
    if (err < 0 && err != AVERROR_EOF) {
        if (error) {
            *error = QStringLiteral("audio encoder: %1").arg(avError(err));
        }
        return false;
    }
    while (true) {
        err = avcodec_receive_packet(m_ctx.get(), m_packet.get());
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            return true;
        }
        if (err < 0) {
            if (error) {
                *error = QStringLiteral("audio encoder: %1").arg(avError(err));
            }
            return false;
        }
        const bool ok = sink(m_packet.get());
        av_packet_unref(m_packet.get());
        if (!ok) {
            return false;
        }
    }
}

bool AudioEncoder::flush(const PacketSink& sink, QString* error)
{
    if (!m_ctx) {
        return false;
    }
    // Drain the resampler's delay line.
    if (m_convCapacity > 0) {
        const int out = swr_convert(m_swr.get(), m_convBuffer, m_convCapacity, nullptr, 0);
        if (out > 0 && !pushConverted(out)) {
            return false;
        }
    }
    return encodeFromFifo(true, sink, error) && send(nullptr, sink, error);
}

} // namespace kk::exporter
