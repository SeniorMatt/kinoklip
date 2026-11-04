// SPDX-License-Identifier: GPL-3.0-or-later
// MediaProbe — media::probe(): describes a media file using FFmpeg, or ImageLoader /
// ImageFrameList for stills, animated WebP and image sequences.
#include "media/FFmpegUtil.h"
#include "media/ImageFrameList.h"
#include "media/MediaInfo.h"
#include "media/MediaSource.h"

#include <QFileInfo>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace kk::media {

namespace {

/// Frame rates that a measured rate is snapped to when it is within kFrameRateSnapTolerance.
/// Phone recordings often report averages like 29.9701 fps; editors want 30000/1001.
constexpr std::array<Rational, 12> kStandardFrameRates = {{
    {24000, 1001},
    {24, 1},
    {25, 1},
    {30000, 1001},
    {30, 1},
    {48, 1},
    {50, 1},
    {60000, 1001},
    {60, 1},
    {100, 1},
    {120000, 1001},
    {120, 1},
}};

/// Relative tolerance for snapping to kStandardFrameRates (0.1 %).
constexpr double kFrameRateSnapTolerance = 0.001;

Rational snapFrameRate(AVRational rate)
{
    if (rate.num <= 0 || rate.den <= 0) {
        return {0, 1};
    }
    const double value = av_q2d(rate);
    for (const Rational& standard : kStandardFrameRates) {
        if (std::abs(value - standard.toDouble())
            <= standard.toDouble() * kFrameRateSnapTolerance) {
            return standard;
        }
    }
    AVRational reduced {};
    av_reduce(&reduced.num, &reduced.den, rate.num, rate.den, INT32_MAX);
    return {reduced.num, reduced.den};
}

/// libavformat reports e.g. "mov,mp4,m4a,3gp,3g2,mj2" — pick the entry matching the file
/// extension (or the first one) so the UI can show "mp4".
QString friendlyContainerName(const AVFormatContext* fmt, const QString& path)
{
    const QStringList names = QString::fromUtf8(fmt->iformat->name).split(QLatin1Char(','));
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (names.contains(suffix)) {
        return suffix;
    }
    return names.value(0);
}

/// Clockwise display rotation (0/90/180/270) from the stream's display matrix side data.
int displayRotation(const AVStream* stream)
{
    const AVCodecParameters* par = stream->codecpar;
    const AVPacketSideData* sideData = av_packet_side_data_get(
        par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sideData || sideData->size < 9 * sizeof(int32_t)) {
        return 0;
    }
    // av_display_rotation_get() returns the counter-clockwise angle; we store clockwise.
    const double counterClockwise
        = av_display_rotation_get(reinterpret_cast<const int32_t*>(sideData->data));
    if (std::isnan(counterClockwise)) {
        return 0;
    }
    const int clockwise = int(std::lround(-counterClockwise / 90.0)) * 90;
    return ((clockwise % 360) + 360) % 360;
}

/// True for demuxers that read single images (FFmpeg's "image2" and "*_pipe" demuxers).
bool isImageDemuxer(const AVFormatContext* fmt)
{
    const QString name = QString::fromUtf8(fmt->iformat->name);
    return name == QLatin1String("image2") || name.endsWith(QLatin1String("_pipe"));
}

/// Microsecond end of `stream` relative to media time 0, or -1 if unknown.
int64_t streamEndUs(const AVFormatContext* fmt, const AVStream* stream)
{
    if (stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) {
        return -1;
    }
    const int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    return av_rescale_q(start + stream->duration, stream->time_base, detail::kMicrosecondsTb)
        - detail::originUs(fmt);
}

/// MP4 / MOV files whose media data was cut off (a copy or download interrupted, a camera
/// battery dying while the index was already written): the end (µs from media time 0) of the
/// samples of @p stream that are completely in the file, or -1 when every sample is there.
/// The demuxer's index lists every sample with its byte position; samples past the end of the
/// file can never be decoded (the decoder would freeze on the last frame).
int64_t truncatedEndUs(const AVFormatContext* fmt, const AVStream* stream)
{
    if (!fmt->pb || !fmt->iformat || !QByteArray(fmt->iformat->name).startsWith("mov"))
        return -1;
    const int64_t fileSize = avio_size(fmt->pb);
    if (fileSize <= 0)
        return -1;
    const int count = avformat_index_get_entries_count(stream);
    int64_t firstMissingTs = AV_NOPTS_VALUE;
    for (int i = 0; i < count; ++i) {
        const AVIndexEntry* e = avformat_index_get_entry(const_cast<AVStream*>(stream), i);
        if (e && e->pos + e->size > fileSize
            && (firstMissingTs == AV_NOPTS_VALUE || e->timestamp < firstMissingTs))
            firstMissingTs = e->timestamp;
    }
    if (firstMissingTs == AV_NOPTS_VALUE)
        return -1;
    return std::max<int64_t>(0,
        av_rescale_q(firstMissingTs, stream->time_base, detail::kMicrosecondsTb)
            - detail::originUs(fmt));
}

/// Embedded start timecode ("HH:MM:SS:FF", ';' before the frames = drop frame): the
/// container's "timecode" tag, else the video stream's, else any stream's (QuickTime keeps it
/// on a "tmcd" data track, MXF on the container). Empty when the file has none.
QString startTimecode(const AVFormatContext* fmt, const AVStream* video)
{
    auto tag = [](const AVDictionary* dict) {
        const AVDictionaryEntry* e = av_dict_get(dict, "timecode", nullptr, 0);
        return e && e->value ? QString::fromUtf8(e->value).trimmed() : QString();
    };
    QString tc = tag(fmt->metadata);
    if (tc.isEmpty() && video)
        tc = tag(video->metadata);
    for (unsigned i = 0; tc.isEmpty() && i < fmt->nb_streams; ++i)
        tc = tag(fmt->streams[i]->metadata);
    return tc;
}

} // namespace

namespace detail {

MediaInfo stillInfo(const QString& path, const ImageHeader& header)
{
    MediaInfo info;
    info.path = path;
    info.valid = info.hasVideo = info.isStillImage = true;
    info.width = info.displayWidth = header.size.width();
    info.height = info.displayHeight = header.size.height();
    info.hasAlpha = header.hasAlpha;
    info.container = info.videoCodec = header.format;
    info.pixelFormat = header.pixelFormat;
    return info;
}

MediaInfo infoFromFormatContext(const AVFormatContext* fmt, const QString& path)
{
    MediaInfo info;
    info.path = path;
    info.container = friendlyContainerName(fmt, path);
    info.bitRate = fmt->bit_rate > 0 ? fmt->bit_rate : 0;
    info.startTimeUs = originUs(fmt);

    const int videoIndex = findVideoStream(fmt);
    const int audioIndex = findAudioStream(fmt);
    const AVStream* video = videoIndex >= 0 ? fmt->streams[videoIndex] : nullptr;
    const AVStream* audio = audioIndex >= 0 ? fmt->streams[audioIndex] : nullptr;

    if (video) {
        const AVCodecParameters* par = video->codecpar;
        info.hasVideo = true;
        info.videoCodec = QString::fromUtf8(avcodec_get_name(par->codec_id));
        info.width = par->width;
        info.height = par->height;
        info.rotation = displayRotation(video);
        AVRational sar = av_guess_sample_aspect_ratio(
            const_cast<AVFormatContext*>(fmt), const_cast<AVStream*>(video), nullptr);
        if (sar.num <= 0 || sar.den <= 0) {
            sar = {1, 1};
        }
        av_reduce(&sar.num, &sar.den, sar.num, sar.den, INT16_MAX);
        info.sampleAspectRatio = {sar.num, sar.den};
        info.displayWidth = int(std::lround(double(par->width) * sar.num / sar.den));
        info.displayHeight = par->height;
        if (info.rotation == 90 || info.rotation == 270) {
            std::swap(info.displayWidth, info.displayHeight);
        }
        const auto pixFmt = AVPixelFormat(par->format);
        if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt)) {
            info.pixelFormat = QString::fromUtf8(desc->name);
            info.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
        }
        info.isStillImage = isImageDemuxer(fmt) && video->nb_frames <= 1;
        if (!info.isStillImage) {
            info.frameRate = snapFrameRate(av_guess_frame_rate(
                const_cast<AVFormatContext*>(fmt), const_cast<AVStream*>(video), nullptr));
        }
    }
    if (audio) {
        const AVCodecParameters* par = audio->codecpar;
        info.hasAudio = true;
        info.audioCodec = QString::fromUtf8(avcodec_get_name(par->codec_id));
        info.audioChannels = par->ch_layout.nb_channels;
        info.audioSampleRate = par->sample_rate;
    }

    // Duration: the length of the primary stream (video, else audio) measured from media
    // time 0; fall back to the container duration when the stream does not know it.
    if (!info.isStillImage) {
        const AVStream* primary = video ? video : audio;
        int64_t endUs = primary ? streamEndUs(fmt, primary) : -1;
        if (endUs <= 0 && fmt->duration != AV_NOPTS_VALUE) {
            endUs = fmt->duration;
        }
        info.durationUs = std::max<int64_t>(0, endUs);
        const int64_t cutUs = primary ? truncatedEndUs(fmt, primary) : -1;
        if (cutUs >= 0 && cutUs < info.durationUs) {
            if (cutUs == 0) {
                info.hasVideo = info.hasAudio = false;
                info.error = QStringLiteral("The file is incomplete: it was cut off before its "
                                            "first frame (an interrupted copy or recording)");
                return info;
            }
            // Usable up to the cut: the clip is that long, and the user is told why.
            info.error = QStringLiteral("The file is incomplete: only the first %1 s of %2 s "
                                        "are in it (an interrupted copy or recording)")
                             .arg(double(cutUs) / 1e6, 0, 'f', 1)
                             .arg(double(info.durationUs) / 1e6, 0, 'f', 1);
            info.durationUs = cutUs;
        }
    }

    info.startTimecode = startTimecode(fmt, video);

    info.valid = info.hasVideo || info.hasAudio;
    if (!info.valid) {
        info.error = QStringLiteral("No decodable audio or video stream");
    }
    return info;
}

} // namespace detail

MediaInfo probe(const QString& path)
{
    // Images, animations and sequences first (decided by extension / path form, so ordinary
    // video and audio files go straight to FFmpeg without touching an image library).
    ImageHeader header;
    switch (detail::routeFor(path, &header)) {
    case detail::MediaRoute::Sequence:
        return detail::ImageFrameList::probeSequence(path);
    case detail::MediaRoute::Still:
        return detail::stillInfo(path, header);
    case detail::MediaRoute::Animation:
        return detail::ImageFrameList::probeAnimation(path, header);
    case detail::MediaRoute::FFmpeg:
        break;
    }
    MediaInfo info;
    info.path = path;
    if (!mediaExists(path)) {
        info.error = QStringLiteral("File not found: %1").arg(path);
        return info;
    }
    QString error;
    const detail::FormatContextPtr fmt = detail::openInput(path, &error);
    if (!fmt) {
        info.error = error;
        return info;
    }
    return detail::infoFromFormatContext(fmt.get(), path);
}

} // namespace kk::media
