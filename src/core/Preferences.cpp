// SPDX-License-Identifier: GPL-3.0-or-later
// Typed accessors for preference keys stored in QSettings.
#include "core/Preferences.h"

#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace kk::prefs {

namespace {
constexpr int kDefaultAutosaveMinutes = 2;
constexpr int kDefaultStillCacheMiB = 256; ///< media::ImageLoader's own default
constexpr int kMinStillCacheMiB = 16;
constexpr int kDefaultRenderCacheGiB = 20;

struct HwAccelName
{
    HwAccel mode;
    const char* id;
};

constexpr HwAccelName kHwAccelNames[] = {
    {HwAccel::Auto, "auto"},
    {HwAccel::Off, "off"},
    {HwAccel::Vaapi, "vaapi"},
    {HwAccel::Cuda, "cuda"},
    {HwAccel::Qsv, "qsv"},
    {HwAccel::Vulkan, "vulkan"},
};
} // namespace

QString hwAccelToString(HwAccel mode)
{
    for (const HwAccelName& n : kHwAccelNames) {
        if (n.mode == mode)
            return QLatin1String(n.id);
    }
    return QStringLiteral("auto");
}

HwAccel hwAccelFromString(const QString& id)
{
    for (const HwAccelName& n : kHwAccelNames) {
        if (id == QLatin1String(n.id))
            return n.mode;
    }
    return HwAccel::Auto;
}

QStringList hwAccelIds()
{
    QStringList ids;
    for (const HwAccelName& n : kHwAccelNames)
        ids.append(QLatin1String(n.id));
    return ids;
}

HwAccel hwAccel()
{
    return hwAccelFromString(QSettings().value(QLatin1String(kHwAccel)).toString());
}

void setHwAccel(HwAccel mode)
{
    QSettings().setValue(QLatin1String(kHwAccel), hwAccelToString(mode));
}

int autosaveMinutes()
{
    return QSettings().value(QLatin1String(kAutosaveMinutes), kDefaultAutosaveMinutes).toInt();
}

void setAutosaveMinutes(int minutes)
{
    QSettings().setValue(QLatin1String(kAutosaveMinutes), qMax(0, minutes));
}

int stillCacheMiB()
{
    return qMax(kMinStillCacheMiB,
        QSettings().value(QLatin1String(kStillCacheMiB), kDefaultStillCacheMiB).toInt());
}

bool forceCpuRendering()
{
    return QSettings().value(QLatin1String(kRenderBackend)).toString() == QLatin1String("cpu");
}

QString cacheDirectory()
{
    return QSettings().value(QLatin1String(kCacheDirectory)).toString();
}

QString effectiveCacheDirectory()
{
    const QString dir = cacheDirectory();
    if (!dir.isEmpty())
        return dir;
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/media");
}

QString proxyDirectoryOverride()
{
    return QSettings().value(QLatin1String(kProxyDirectory)).toString();
}

QString proxyDirectory()
{
    const QString dir = proxyDirectoryOverride();
    return dir.isEmpty() ? effectiveCacheDirectory() + QStringLiteral("/ProxyMedia") : dir;
}

QString optimizedMediaDirectory()
{
    return effectiveCacheDirectory() + QStringLiteral("/OptimizedMedia");
}

QString renderCacheDirectory()
{
    return effectiveCacheDirectory() + QStringLiteral("/RenderCache");
}

int renderCacheLimitGiB()
{
    return std::max(
        1, QSettings().value(QLatin1String(kRenderCacheLimitGiB), kDefaultRenderCacheGiB).toInt());
}

QString audioOutputDevice()
{
    return QSettings().value(QLatin1String(kAudioOutputDevice)).toString();
}

QString previewQuality()
{
    QSettings settings;
    constexpr int kRevision = 2;
    if (settings.value(QLatin1String(kPreviewQualityRevision), 0).toInt() < kRevision) {
        // Alpha.1/alpha.2 defaulted to "auto", which dropped to a quarter of the viewer size
        // while playing: everyone starts at Full once; Auto stays available as an option.
        if (settings.value(QLatin1String(kPreviewQuality)).toString() == QLatin1String("auto"))
            settings.setValue(QLatin1String(kPreviewQuality), QStringLiteral("full"));
        settings.setValue(QLatin1String(kPreviewQualityRevision), kRevision);
    }
    return settings.value(QLatin1String(kPreviewQuality), QStringLiteral("full")).toString();
}

void setPreviewQuality(const QString& id)
{
    QSettings settings;
    settings.setValue(QLatin1String(kPreviewQuality), id);
    settings.setValue(QLatin1String(kPreviewQualityRevision), 2);
}

bool zoomAroundMouse()
{
    return QSettings().value(QLatin1String(kZoomAnchor)).toString() == QLatin1String("mouse");
}

bool showDefaultSmartBins()
{
    return QSettings().value(QLatin1String(kDefaultSmartBins), false).toBool();
}

void setShowDefaultSmartBins(bool show)
{
    QSettings().setValue(QLatin1String(kDefaultSmartBins), show);
}

} // namespace kk::prefs
