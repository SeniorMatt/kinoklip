// SPDX-License-Identifier: GPL-3.0-or-later
// ExportSettings — derived values, validation, normalisation and the JSON form.
#include "export/ExportSettings.h"

#include "core/TimelineData.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ExportSettings", text);
}

constexpr int kMaxDimension = 16384; ///< sanity limit for custom output sizes (pixels)

QString rateControlId(RateControl rc)
{
    return rc == RateControl::Bitrate ? QStringLiteral("bitrate") : QStringLiteral("quality");
}

QString rangeId(RangeMode mode)
{
    switch (mode) {
    case RangeMode::InOut:
        return QStringLiteral("inout");
    case RangeMode::Custom:
        return QStringLiteral("custom");
    case RangeMode::Entire:
        break;
    }
    return QStringLiteral("entire");
}

RangeMode rangeFromId(const QString& id, RangeMode fallback)
{
    if (id == QLatin1String("entire")) {
        return RangeMode::Entire;
    }
    if (id == QLatin1String("inout")) {
        return RangeMode::InOut;
    }
    if (id == QLatin1String("custom")) {
        return RangeMode::Custom;
    }
    return fallback;
}

} // namespace

QString encoderBackendId(EncoderBackend backend)
{
    switch (backend) {
    case EncoderBackend::Software:
        return QStringLiteral("software");
    case EncoderBackend::Nvenc:
        return QStringLiteral("nvenc");
    case EncoderBackend::Vaapi:
        return QStringLiteral("vaapi");
    case EncoderBackend::Qsv:
        return QStringLiteral("qsv");
    case EncoderBackend::Auto:
        break;
    }
    return QStringLiteral("auto");
}

EncoderBackend encoderBackendFromId(const QString& id)
{
    for (EncoderBackend b : {EncoderBackend::Software, EncoderBackend::Nvenc, EncoderBackend::Vaapi,
             EncoderBackend::Qsv}) {
        if (encoderBackendId(b) == id) {
            return b;
        }
    }
    return EncoderBackend::Auto;
}

QString encoderBackendName(EncoderBackend backend)
{
    switch (backend) {
    case EncoderBackend::Software:
        return tr("Software");
    case EncoderBackend::Nvenc:
        return tr("NVIDIA NVENC");
    case EncoderBackend::Vaapi:
        return tr("VAAPI (Intel/AMD)");
    case EncoderBackend::Qsv:
        return tr("Intel Quick Sync");
    case EncoderBackend::Auto:
        break;
    }
    return tr("Auto (hardware if available)");
}

OutputKind ExportSettings::kind() const
{
    const ContainerInfo* info = containerInfo();
    return info ? info->kind : OutputKind::Movie;
}

bool ExportSettings::hasVideo() const
{
    const OutputKind k = kind();
    return k != OutputKind::Audio && (k != OutputKind::Movie || videoEnabled);
}

bool ExportSettings::hasAudio() const
{
    const OutputKind k = kind();
    return k == OutputKind::Audio || (k == OutputKind::Movie && audio.enabled);
}

bool ExportSettings::burnsInSubtitles() const
{
    return subtitles.enabled && subtitles.mode == SubtitleExportMode::BurnIn && hasVideo();
}

QString ExportSettings::embeddedSubtitleCodec() const
{
    if (!subtitles.enabled || subtitles.mode != SubtitleExportMode::Embedded)
        return {};
    const QStringList codecs = subtitleCodecs(container);
    if (codecs.isEmpty())
        return {};
    return codecs.contains(subtitles.embeddedCodec) ? subtitles.embeddedCodec : codecs.front();
}

bool ExportSettings::keepsAlpha() const
{
    if (!video.alpha || !hasVideo()) {
        return false;
    }
    const VideoCodecInfo* codec = findVideoCodec(video.codec);
    if (!codec || !codec->alphaCapable) {
        return false;
    }
    if (codec->id == QLatin1String("prores")) {
        return video.profile.startsWith(QLatin1String("4444"));
    }
    return true;
}

QSize ExportSettings::outputSize(const TimelineData& timeline) const
{
    int w = video.width > 0 ? video.width : timeline.width;
    int h = video.height > 0 ? video.height : timeline.height;
    // One dimension given: the other follows the timeline's aspect ratio (GIF "480 wide").
    if (video.width > 0 && video.height <= 0 && timeline.width > 0) {
        h = int(std::lround(double(video.width) * timeline.height / timeline.width));
    } else if (video.height > 0 && video.width <= 0 && timeline.height > 0) {
        w = int(std::lround(double(video.height) * timeline.width / timeline.height));
    }
    const VideoCodecInfo* codec = findVideoCodec(video.codec);
    if (!codec || codec->needsEvenSize) {
        w &= ~1;
        h &= ~1;
    }
    return {std::clamp(w, 2, kMaxDimension), std::clamp(h, 2, kMaxDimension)};
}

std::pair<int64_t, int64_t> ExportSettings::frameRange(const TimelineData& timeline) const
{
    const int64_t end = timeline.duration();
    int64_t first = 0;
    int64_t last = end;
    switch (range) {
    case RangeMode::InOut:
        // Same convention as the timeline: markOut is exclusive; an unset mark keeps the edge.
        if (timeline.markIn >= 0) {
            first = timeline.markIn;
        }
        if (timeline.markOut >= 0) {
            last = timeline.markOut;
        }
        break;
    case RangeMode::Custom:
        first = rangeIn;
        last = rangeOut;
        break;
    case RangeMode::Entire:
        break;
    }
    first = std::max<int64_t>(0, first);
    if (kind() == OutputKind::Still) {
        last = first + 1; // a still is the first frame of the range (the playhead for "grab")
    }
    return {first, std::max(first, last)};
}

bool ExportSettings::validate(QString* error) const
{
    auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const ContainerInfo* info = containerInfo();
    if (!info) {
        return fail(tr("Unknown output format \"%1\".").arg(container));
    }
    if (outputPath.isEmpty()) {
        return fail(tr("No output file name was given."));
    }
    if (hasVideo() && !info->videoCodecs.contains(video.codec)) {
        return fail(tr("%1 cannot contain %2 video.").arg(info->name, video.codec));
    }
    if (hasAudio() && !info->audioCodecs.contains(audio.codec)) {
        return fail(tr("%1 cannot contain %2 audio.").arg(info->name, audio.codec));
    }
    if (!hasVideo() && !hasAudio()) {
        return fail(tr("Neither video nor audio is enabled."));
    }
    if (hasVideo()) {
        const VideoCodecInfo* codec = findVideoCodec(video.codec);
        if (!codec) {
            return fail(tr("Unknown video codec \"%1\".").arg(video.codec));
        }
        if (!video.profile.isEmpty() && !codec->profiles.isEmpty()
            && !codec->profiles.contains(video.profile)) {
            return fail(tr("Unknown %1 profile \"%2\".").arg(codec->name, video.profile));
        }
        if (video.width < 0 || video.height < 0 || video.width > kMaxDimension
            || video.height > kMaxDimension) {
            return fail(tr("Invalid output resolution %1x%2.").arg(video.width).arg(video.height));
        }
        if (video.rateControl == RateControl::Bitrate && codec->hasBitrate
            && video.bitrateKbps <= 0) {
            return fail(tr("The video bitrate must be positive."));
        }
    }
    if (hasAudio()) {
        const AudioCodecInfo* codec = findAudioCodec(audio.codec);
        if (!codec) {
            return fail(tr("Unknown audio codec \"%1\".").arg(audio.codec));
        }
        if (!codec->sampleRates.contains(audio.sampleRate)) {
            return fail(tr("%1 does not support %2 Hz.").arg(codec->name).arg(audio.sampleRate));
        }
        if (!codec->lossless && audio.bitrateKbps <= 0) {
            return fail(tr("The audio bitrate must be positive."));
        }
    }
    if (range == RangeMode::Custom && rangeOut <= rangeIn && kind() != OutputKind::Still) {
        return fail(tr("The render range is empty."));
    }
    return true;
}

void ExportSettings::normalize()
{
    const ContainerInfo* info = containerInfo();
    if (!info) {
        return;
    }
    if (!info->videoCodecs.isEmpty() && !info->videoCodecs.contains(video.codec)) {
        video.codec = info->videoCodecs.first();
        video.profile.clear();
    }
    if (!info->audioCodecs.isEmpty() && !info->audioCodecs.contains(audio.codec)) {
        audio.codec = info->audioCodecs.first();
    }
    if (const VideoCodecInfo* codec = findVideoCodec(video.codec)) {
        if (!codec->profiles.isEmpty() && !codec->profiles.contains(video.profile)) {
            video.profile = codec->profiles.first();
        }
        if (codec->profiles.isEmpty()) {
            video.profile.clear();
        }
        if (codec->hasQuality
            && (video.quality < codec->qualityMin || video.quality > codec->qualityMax)) {
            video.quality = codec->qualityDefault;
        }
    }
    if (const AudioCodecInfo* codec = findAudioCodec(audio.codec)) {
        if (!codec->sampleRates.contains(audio.sampleRate)) {
            audio.sampleRate = codec->sampleRates.first();
        }
    }
    if (!outputPath.isEmpty()) {
        outputPath = withContainerExtension(outputPath, container);
    }
}

QJsonObject ExportSettings::toJson() const
{
    QJsonObject v;
    v[QStringLiteral("codec")] = video.codec;
    v[QStringLiteral("encoder")] = encoderBackendId(video.encoder);
    if (!video.profile.isEmpty()) {
        v[QStringLiteral("profile")] = video.profile;
    }
    v[QStringLiteral("rateControl")] = rateControlId(video.rateControl);
    v[QStringLiteral("quality")] = video.quality;
    v[QStringLiteral("bitrateKbps")] = video.bitrateKbps;
    v[QStringLiteral("speed")] = video.speed;
    v[QStringLiteral("width")] = video.width;
    v[QStringLiteral("height")] = video.height;
    v[QStringLiteral("alpha")] = video.alpha;
    v[QStringLiteral("keyframeInterval")] = video.keyframeInterval;
    v[QStringLiteral("frameReordering")] = video.frameReordering;
    QJsonObject a;
    a[QStringLiteral("enabled")] = audio.enabled;
    a[QStringLiteral("codec")] = audio.codec;
    a[QStringLiteral("bitrateKbps")] = audio.bitrateKbps;
    a[QStringLiteral("sampleRate")] = audio.sampleRate;
    a[QStringLiteral("bitDepth")] = audio.bitDepth;
    QJsonObject o;
    o[QStringLiteral("container")] = container;
    o[QStringLiteral("videoEnabled")] = videoEnabled;
    o[QStringLiteral("video")] = v;
    o[QStringLiteral("audio")] = a;
    o[QStringLiteral("range")] = rangeId(range);
    o[QStringLiteral("rangeIn")] = double(rangeIn);
    o[QStringLiteral("rangeOut")] = double(rangeOut);
    if (!outputPath.isEmpty()) {
        o[QStringLiteral("outputPath")] = outputPath;
    }
    o[QStringLiteral("subtitles")] = subtitles.toJson();
    if (individualClips) {
        o[QStringLiteral("individualClips")] = true;
        o[QStringLiteral("clipNaming")] = clipNaming == ClipNaming::SourceName
            ? QStringLiteral("source")
            : clipNaming == ClipNaming::NumberedClipName ? QStringLiteral("numbered")
                                                         : QStringLiteral("clip");
        if (clipHandles > 0) {
            o[QStringLiteral("clipHandles")] = clipHandles;
        }
    }
    return o;
}

ExportSettings ExportSettings::fromJson(const QJsonObject& o, const ExportSettings& base)
{
    ExportSettings s = base;
    auto str = [](const QJsonObject& obj, const char* key, const QString& fallback) {
        const QJsonValue value = obj.value(QLatin1String(key));
        return value.isString() ? value.toString() : fallback;
    };
    auto num = [](const QJsonObject& obj, const char* key, double fallback) {
        const QJsonValue value = obj.value(QLatin1String(key));
        return value.isDouble() ? value.toDouble() : fallback;
    };
    auto boolean = [](const QJsonObject& obj, const char* key, bool fallback) {
        const QJsonValue value = obj.value(QLatin1String(key));
        return value.isBool() ? value.toBool() : fallback;
    };
    s.container = str(o, "container", s.container);
    s.videoEnabled = boolean(o, "videoEnabled", s.videoEnabled);
    const QJsonObject v = o.value(QLatin1String("video")).toObject();
    s.video.codec = str(v, "codec", s.video.codec);
    if (v.contains(QLatin1String("encoder"))) {
        s.video.encoder = encoderBackendFromId(str(v, "encoder", {}));
    }
    s.video.profile = str(v, "profile", s.video.profile);
    if (v.contains(QLatin1String("rateControl"))) {
        s.video.rateControl = str(v, "rateControl", {}) == QLatin1String("bitrate")
            ? RateControl::Bitrate
            : RateControl::Quality;
    }
    s.video.quality = int(num(v, "quality", s.video.quality));
    s.video.bitrateKbps = int(num(v, "bitrateKbps", s.video.bitrateKbps));
    s.video.speed = str(v, "speed", s.video.speed);
    s.video.width = int(num(v, "width", s.video.width));
    s.video.height = int(num(v, "height", s.video.height));
    s.video.alpha = boolean(v, "alpha", s.video.alpha);
    s.video.keyframeInterval
        = std::max(0, int(num(v, "keyframeInterval", s.video.keyframeInterval)));
    s.video.frameReordering = boolean(v, "frameReordering", s.video.frameReordering);
    const QJsonObject a = o.value(QLatin1String("audio")).toObject();
    s.audio.enabled = boolean(a, "enabled", s.audio.enabled);
    s.audio.codec = str(a, "codec", s.audio.codec);
    s.audio.bitrateKbps = int(num(a, "bitrateKbps", s.audio.bitrateKbps));
    s.audio.sampleRate = int(num(a, "sampleRate", s.audio.sampleRate));
    s.audio.bitDepth = int(num(a, "bitDepth", s.audio.bitDepth));
    s.range = rangeFromId(str(o, "range", {}), s.range);
    s.rangeIn = int64_t(num(o, "rangeIn", double(s.rangeIn)));
    s.rangeOut = int64_t(num(o, "rangeOut", double(s.rangeOut)));
    s.outputPath = str(o, "outputPath", s.outputPath);
    s.subtitles = SubtitleExportSettings::fromJson(
        o.value(QStringLiteral("subtitles")).toObject(), base.subtitles);
    s.individualClips = boolean(o, "individualClips", s.individualClips);
    if (o.contains(QLatin1String("clipNaming"))) {
        const QString id = str(o, "clipNaming", {});
        s.clipNaming = id == QLatin1String("source") ? ClipNaming::SourceName
            : id == QLatin1String("numbered")        ? ClipNaming::NumberedClipName
                                                     : ClipNaming::ClipName;
    }
    s.clipHandles = std::clamp(int(num(o, "clipHandles", s.clipHandles)), 0, 1000);
    return s;
}

QString sequencePattern(const QString& path)
{
    if (path.contains(QLatin1Char('%'))) {
        return path;
    }
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    QString base = suffix.isEmpty() ? path : path.left(path.size() - suffix.size() - 1);
    return base + QStringLiteral("_%05d")
        + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
}

QString withContainerExtension(const QString& path, const QString& containerId)
{
    const ContainerInfo* info = findContainer(containerId);
    if (!info || path.isEmpty()) {
        return path;
    }
    const QFileInfo fi(path);
    const QString suffix = fi.suffix();
    if (suffix.compare(info->extension, Qt::CaseInsensitive) == 0) {
        return path;
    }
    // Only replace something that looks like an extension (no separators, short).
    const bool hasExtension
        = !suffix.isEmpty() && suffix.size() <= 5 && !fi.fileName().startsWith(QLatin1Char('.'));
    const QString base = hasExtension ? path.left(path.size() - suffix.size() - 1) : path;
    return base + QLatin1Char('.') + info->extension;
}

} // namespace kk
