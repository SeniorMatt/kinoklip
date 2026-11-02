// SPDX-License-Identifier: GPL-3.0-or-later
// ExportFormats — the tables of plan.md §5.8 (export side). To add a container or codec, add a
// row here, teach EncoderCatalog how to configure it, and add a preset if users should see it.
#include "export/ExportFormats.h"

#include <QCoreApplication>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ExportFormats", text);
}

QStringList s(std::initializer_list<const char*> list)
{
    QStringList out;
    for (const char* item : list) {
        out << QString::fromLatin1(item);
    }
    return out;
}

QList<ContainerInfo> makeContainers()
{
    // Video codecs offered per container follow what the muxer can carry and what players
    // expect: MP4 = delivery codecs only, MOV = mastering codecs too, MKV = anything.
    const QStringList movieAudio = s({"aac", "pcm_s16le", "pcm_s24le"});
    return {
        {QStringLiteral("mp4"), tr("MP4"), QStringLiteral("mp4"), QStringLiteral("mp4"),
            OutputKind::Movie, s({"h264", "hevc", "av1"}), s({"aac", "mp3", "opus"}), true},
        {QStringLiteral("mov"), tr("QuickTime"), QStringLiteral("mov"), QStringLiteral("mov"),
            OutputKind::Movie, s({"prores", "dnxhr", "h264", "hevc"}),
            s({"pcm_s24le", "pcm_s16le", "aac"}), true},
        {QStringLiteral("mkv"), tr("Matroska"), QStringLiteral("mkv"), QStringLiteral("matroska"),
            OutputKind::Movie, s({"h264", "hevc", "av1", "vp9", "prores", "dnxhr"}),
            s({"flac", "opus", "aac", "pcm_s24le"}), false},
        {QStringLiteral("webm"), tr("WebM"), QStringLiteral("webm"), QStringLiteral("webm"),
            OutputKind::Movie, s({"vp9", "av1"}), s({"opus"}), false},
        {QStringLiteral("wav"), tr("Wave (audio only)"), QStringLiteral("wav"),
            QStringLiteral("wav"), OutputKind::Audio, {},
            s({"pcm_s24le", "pcm_s16le", "pcm_f32le"}), false},
        {QStringLiteral("flac"), tr("FLAC (audio only)"), QStringLiteral("flac"),
            QStringLiteral("flac"), OutputKind::Audio, {}, s({"flac"}), false},
        {QStringLiteral("mp3"), tr("MP3 (audio only)"), QStringLiteral("mp3"),
            QStringLiteral("mp3"), OutputKind::Audio, {}, s({"mp3"}), false},
        {QStringLiteral("m4a"), tr("AAC / M4A (audio only)"), QStringLiteral("m4a"),
            QStringLiteral("ipod"), OutputKind::Audio, {}, s({"aac"}), true},
        {QStringLiteral("opus"), tr("Opus (audio only)"), QStringLiteral("opus"),
            QStringLiteral("opus"), OutputKind::Audio, {}, s({"opus"}), false},
        {QStringLiteral("png_seq"), tr("PNG sequence"), QStringLiteral("png"),
            QStringLiteral("image2"), OutputKind::ImageSequence, s({"png"}), {}, false},
        {QStringLiteral("jpeg_seq"), tr("JPEG sequence"), QStringLiteral("jpg"),
            QStringLiteral("image2"), OutputKind::ImageSequence, s({"mjpeg"}), {}, false},
        {QStringLiteral("tiff_seq"), tr("TIFF sequence"), QStringLiteral("tif"),
            QStringLiteral("image2"), OutputKind::ImageSequence, s({"tiff"}), {}, false},
        {QStringLiteral("exr_seq"), tr("OpenEXR sequence"), QStringLiteral("exr"),
            QStringLiteral("image2"), OutputKind::ImageSequence, s({"exr"}), {}, false},
        {QStringLiteral("gif"), tr("GIF"), QStringLiteral("gif"), QStringLiteral("gif"),
            OutputKind::Gif, s({"gif"}), {}, false},
        {QStringLiteral("png_still"), tr("Single frame (PNG)"), QStringLiteral("png"), {},
            OutputKind::Still, s({"png"}), {}, false},
        {QStringLiteral("jpeg_still"), tr("Single frame (JPEG)"), QStringLiteral("jpg"), {},
            OutputKind::Still, s({"mjpeg"}), {}, false},
    };
}

/// A codec row; quality range only for codecs with a constant-quality mode.
VideoCodecInfo codec(const char* id, const QString& name, int qMin = 0, int qMax = 0,
    int qDefault = 0, bool bitrate = false)
{
    VideoCodecInfo c;
    c.id = QString::fromLatin1(id);
    c.name = name;
    c.hasQuality = qMax > 0;
    c.hasBitrate = bitrate;
    c.qualityMin = qMin;
    c.qualityMax = qMax;
    c.qualityDefault = qDefault;
    return c;
}

QList<VideoCodecInfo> makeVideoCodecs()
{
    // Quality scales: x264/x265/NVENC CQ/VAAPI QP share 0..51 (lower = better); SVT-AV1 and
    // VP9 use 0..63; MJPEG qscale 2..31. ProRes/DNxHR quality is chosen by profile.
    VideoCodecInfo h264 = codec("h264", tr("H.264"), 0, 51, 20, true);
    VideoCodecInfo hevc = codec("hevc", tr("H.265 / HEVC"), 0, 51, 24, true);
    VideoCodecInfo av1 = codec("av1", tr("AV1"), 0, 63, 32, true);
    VideoCodecInfo vp9 = codec("vp9", tr("VP9"), 0, 63, 32, true);
    VideoCodecInfo prores = codec("prores", tr("Apple ProRes"));
    prores.profiles = s({"hq", "standard", "lt", "proxy", "4444", "4444xq"});
    prores.profileNames = QStringList {
        tr("422 HQ"), tr("422"), tr("422 LT"), tr("422 Proxy"), tr("4444"), tr("4444 XQ")};
    prores.alphaCapable = true;
    VideoCodecInfo dnxhr = codec("dnxhr", tr("DNxHR"));
    dnxhr.profiles = s({"hq", "sq", "lb", "hqx"});
    dnxhr.profileNames = QStringList {tr("HQ"), tr("SQ"), tr("LB"), tr("HQX (10-bit)")};
    VideoCodecInfo png = codec("png", tr("PNG"));
    png.alphaCapable = true;
    png.needsEvenSize = false;
    VideoCodecInfo jpeg = codec("mjpeg", tr("JPEG"), 2, 31, 2);
    VideoCodecInfo tiff = codec("tiff", tr("TIFF"));
    tiff.alphaCapable = true;
    tiff.needsEvenSize = false;
    VideoCodecInfo exr = codec("exr", tr("OpenEXR (half-float, linear)"));
    exr.alphaCapable = true;
    exr.needsEvenSize = false;
    VideoCodecInfo gif = codec("gif", tr("GIF"));
    gif.needsEvenSize = false;
    return {h264, hevc, av1, vp9, prores, dnxhr, png, jpeg, tiff, exr, gif};
}

QList<AudioCodecInfo> makeAudioCodecs()
{
    const QList<int> common {48000, 44100, 96000};
    return {
        {QStringLiteral("aac"), tr("AAC"), QStringLiteral("aac"), false, {48000, 44100}},
        {QStringLiteral("opus"), tr("Opus"), QStringLiteral("libopus"), false, {48000}},
        {QStringLiteral("mp3"), tr("MP3"), QStringLiteral("libmp3lame"), false, {48000, 44100}},
        {QStringLiteral("flac"), tr("FLAC"), QStringLiteral("flac"), true, common},
        {QStringLiteral("pcm_s16le"), tr("PCM 16-bit"), QStringLiteral("pcm_s16le"), true, common},
        {QStringLiteral("pcm_s24le"), tr("PCM 24-bit"), QStringLiteral("pcm_s24le"), true, common},
        {QStringLiteral("pcm_f32le"), tr("PCM 32-bit float"), QStringLiteral("pcm_f32le"), true,
            common},
    };
}

template<typename T>
const T* findById(const QList<T>& list, const QString& id)
{
    for (const T& item : list) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

} // namespace

const QList<ContainerInfo>& exportContainers()
{
    static const QList<ContainerInfo> list = makeContainers();
    return list;
}

const QList<VideoCodecInfo>& exportVideoCodecs()
{
    static const QList<VideoCodecInfo> list = makeVideoCodecs();
    return list;
}

const QList<AudioCodecInfo>& exportAudioCodecs()
{
    static const QList<AudioCodecInfo> list = makeAudioCodecs();
    return list;
}

const ContainerInfo* findContainer(const QString& id)
{
    return findById(exportContainers(), id);
}

const VideoCodecInfo* findVideoCodec(const QString& id)
{
    return findById(exportVideoCodecs(), id);
}

const AudioCodecInfo* findAudioCodec(const QString& id)
{
    return findById(exportAudioCodecs(), id);
}

} // namespace kk
