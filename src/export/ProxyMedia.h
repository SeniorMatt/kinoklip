// SPDX-License-Identifier: GPL-3.0-or-later
// ProxyMedia — formats for proxy / optimized media and the render cache, and the export job
// description that turns one media pool item into its proxy (Media Pool > Generate Proxy
// Media / Generate Optimized Media).
#pragma once

#include "core/MediaLinks.h"
#include "export/ExportSettings.h"
#include "render/RenderTypes.h"

#include <QString>
#include <QVector>

#include <memory>

namespace kk {

struct MediaItem;
struct TimelineData;

/// One selectable intermediate format.
struct IntermediateFormat
{
    QString id; ///< stable id stored in projects ("h264", "prores_proxy", "dnxhr_lb", ...)
    QString name; ///< UI name
    QString codec; ///< VideoCodecInfo id
    QString profile; ///< ProRes / DNxHR profile, empty otherwise
    int quality = 20; ///< H.264 CRF / CQ
    int keyframeInterval = 0; ///< 0 = intra-only codecs (ignored) / automatic
    bool fastDecode = false;
};

/// Proxy / optimized media formats in UI order. The default proxy format is H.264 at a
/// 12-frame GOP without B-frames, CRF 20, "tune fastdecode" (hardware encoded when VAAPI /
/// NVENC / QSV work): at 960x540 it decodes in ~1.5 ms per frame in software on one core,
/// the fastest of the candidates measured (export/readme.md, Proxy media), with the smallest
/// files, and it is hardware-decodable on every laptop GPU.
[[nodiscard]] const QVector<IntermediateFormat>& proxyFormats();
/// Render cache formats (intra-only, timeline resolution).
[[nodiscard]] const QVector<IntermediateFormat>& renderCacheFormats();
/// Format by id in either list; nullptr if unknown.
[[nodiscard]] const IntermediateFormat* findIntermediateFormat(const QString& id);

/// Export settings of a QuickTime file in @p format at @p size (no audio unless
/// @p withAudio: PCM 16-bit 48 kHz). Range = the whole timeline.
[[nodiscard]] ExportSettings intermediateSettings(
    const IntermediateFormat& format, QSize size, bool withAudio);

/// What one proxy / optimized media job renders.
struct ProxyJobSpec
{
    QString mediaId;
    LinkKind kind = LinkKind::Proxy;
    QString originalPath;
    QString outputPath; ///< final file (stable name in the proxy / optimized folder)
    QString partialPath; ///< what is rendered first; renamed to outputPath when complete
    ExportSettings settings;
    std::shared_ptr<const TimelineData> timeline; ///< one clip (+ linked audio) of the item
    std::shared_ptr<const MediaPathMap> media; ///< {mediaId: originalPath}
    MediaLink link; ///< what to link when the job succeeds (path = outputPath)
};

/// Builds the job for @p item. The proxy is display-equivalent (see render ProxyRef):
/// rendered by the compositor from a one-clip timeline the size of the proxy, so it is
/// upright with square pixels whatever the source's rotation / pixel aspect, at the source's
/// frame rate, frame k at k / fps (the original's timing), with the source's sound as PCM.
/// Returns false (with @p error) for stills, timeline items and items without video.
bool makeProxyJob(const MediaItem& item, LinkKind kind, const MediaCacheSettings& settings,
    const QString& folder, ProxyJobSpec* spec, QString* error);

} // namespace kk
