// SPDX-License-Identifier: GPL-3.0-or-later
// ProxyMedia — intermediate formats and proxy / optimized media job descriptions.
#include "export/ProxyMedia.h"

#include "core/EditOps.h"
#include "core/MediaItem.h"
#include "core/TimelineData.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace kk {

namespace {

/// Proxy GOP: short enough that a seek decodes at most 11 extra frames (~16 ms at 540p),
/// long enough to keep files ~3x smaller than intra-only H.264.
constexpr int kProxyGop = 12;
constexpr int kProxyCrf = 20;

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ProxyMedia", text);
}

IntermediateFormat format(const char* id, const QString& name, const char* codec,
    const char* profile = "", int keyframeInterval = 0, bool fastDecode = false)
{
    IntermediateFormat f;
    f.id = QLatin1String(id);
    f.name = name;
    f.codec = QLatin1String(codec);
    f.profile = QLatin1String(profile);
    f.quality = kProxyCrf;
    f.keyframeInterval = keyframeInterval;
    f.fastDecode = fastDecode;
    return f;
}

/// Used automatically for sources with an alpha channel (H.264 / DNxHR LB would drop it).
const IntermediateFormat& alphaFormat()
{
    static const IntermediateFormat f = format("prores_4444", tr("ProRes 4444"), "prores", "4444");
    return f;
}

} // namespace

const QVector<IntermediateFormat>& proxyFormats()
{
    static const QVector<IntermediateFormat> formats = {
        format("h264", tr("H.264 (fast decode)"), "h264", "", kProxyGop, true),
        format("prores_proxy", tr("ProRes 422 Proxy"), "prores", "proxy"),
        format("prores_lt", tr("ProRes 422 LT"), "prores", "lt"),
        format("dnxhr_lb", tr("DNxHR LB"), "dnxhr", "lb"),
        format("dnxhr_sq", tr("DNxHR SQ"), "dnxhr", "sq"),
    };
    return formats;
}

const QVector<IntermediateFormat>& renderCacheFormats()
{
    static const QVector<IntermediateFormat> formats = {
        format("dnxhr_lb", tr("DNxHR LB"), "dnxhr", "lb"),
        format("dnxhr_sq", tr("DNxHR SQ"), "dnxhr", "sq"),
        format("prores_proxy", tr("ProRes 422 Proxy"), "prores", "proxy"),
        format("prores_standard", tr("ProRes 422"), "prores", "standard"),
    };
    return formats;
}

const IntermediateFormat* findIntermediateFormat(const QString& id)
{
    for (const auto* list : {&proxyFormats(), &renderCacheFormats()}) {
        for (const IntermediateFormat& f : *list) {
            if (f.id == id)
                return &f;
        }
    }
    if (id == alphaFormat().id)
        return &alphaFormat();
    return nullptr;
}

ExportSettings intermediateSettings(const IntermediateFormat& f, QSize size, bool withAudio)
{
    ExportSettings s;
    s.container = QStringLiteral("mov");
    s.videoEnabled = true;
    s.video.codec = f.codec;
    s.video.profile = f.profile;
    s.video.encoder = EncoderBackend::Auto; // hardware H.264 where it works
    s.video.rateControl = RateControl::Quality;
    s.video.quality = f.quality;
    s.video.speed = QStringLiteral("fast");
    s.video.width = size.width();
    s.video.height = size.height();
    s.video.alpha = f.profile.startsWith(QLatin1String("4444"));
    s.video.keyframeInterval = f.keyframeInterval;
    s.video.frameReordering = false; // no B-frames: cheaper decode, exact forward stepping
    s.video.fastDecode = f.fastDecode;
    s.audio.enabled = withAudio;
    s.audio.codec = QStringLiteral("pcm_s16le");
    s.audio.sampleRate = 48000;
    s.range = RangeMode::Entire;
    return s;
}

bool makeProxyJob(const MediaItem& item, LinkKind kind, const MediaCacheSettings& settings,
    const QString& folder, ProxyJobSpec* spec, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    const media::MediaInfo& info = item.info;
    if (item.isTimeline())
        return fail(tr("Timelines and compound clips have no proxy media."));
    if (!info.hasVideo || info.isStillImage || info.durationUs <= 0)
        return fail(tr("%1 has no moving pictures (proxies are made for video).").arg(item.name));
    if (!info.frameRate.isValid())
        return fail(tr("%1 has no known frame rate.").arg(item.name));
    QSize display(info.displayWidth, info.displayHeight);
    if (display.isEmpty())
        display = QSize(info.width, info.height);
    if (display.isEmpty())
        return fail(tr("%1 has no known picture size.").arg(item.name));

    const bool proxy = kind == LinkKind::Proxy;
    const ProxyResolution res = proxy ? settings.proxyResolution : settings.optimizedResolution;
    const IntermediateFormat* f = info.hasAlpha
        ? &alphaFormat()
        : findIntermediateFormat(proxy ? settings.proxyFormat : settings.optimizedFormat);
    if (!f)
        f = proxy ? &proxyFormats().front() : findIntermediateFormat(QStringLiteral("dnxhr_lb"));
    const QSize size = proxySize(res, display);

    // A one-clip timeline the size of the proxy at the source rate: the media clip is fitted
    // into it (aspect kept = fills it exactly), rotation and pixel aspect applied.
    TimelineData tl = makeDefaultTimeline(item.name, info.frameRate, size.width(), size.height());
    const int64_t frames = std::max<int64_t>(1,
        int64_t(
            std::ceil(double(info.durationUs) * info.frameRate.num / (1e6 * info.frameRate.den))));
    ClipData video;
    video.kind = ClipKind::Media;
    video.mediaId = item.id;
    video.name = item.name;
    video.start = 0;
    video.duration = frames;
    video.sourceInUs = 0;
    video.sourceDurationUs = info.durationUs;
    QVector<quint64> ids {editops::overwriteClip(tl, {TrackType::Video, 0}, video)};
    if (info.hasAudio && !tl.audioTracks.empty()) {
        ClipData audio = video;
        audio.id = 0;
        ids.append(editops::overwriteClip(tl, {TrackType::Audio, 0}, audio));
    }

    spec->mediaId = item.id;
    spec->kind = kind;
    spec->originalPath = item.path;
    spec->outputPath = QDir(folder).absoluteFilePath(stableLinkFileName(item.path));
    spec->partialPath = spec->outputPath + QStringLiteral(".partial.mov");
    spec->settings = intermediateSettings(*f, size, info.hasAudio);
    spec->settings.outputPath = spec->partialPath;
    spec->timeline = std::make_shared<const TimelineData>(std::move(tl));
    auto media = std::make_shared<MediaPathMap>();
    media->insert(item.id, item.path);
    spec->media = std::move(media);
    spec->link.path = spec->outputPath;
    spec->link.source = SourceFingerprint::of(item.path);
    spec->link.size = size;
    spec->link.format = f->id;
    return true;
}

} // namespace kk
