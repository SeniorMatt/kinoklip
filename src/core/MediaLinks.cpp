// SPDX-License-Identifier: GPL-3.0-or-later
// MediaLinks — proxy / optimized media links, stale detection, cache settings (JSON forms).
#include "core/MediaLinks.h"

#include "core/MediaItem.h"
#include "media/MediaSource.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Proxy height that "Choose automatically" aims at (lines). 540 lines decode in ~1.5 ms per
/// frame in software (H.264 fastdecode, measured; see export/readme.md).
constexpr int kAutoProxyMaxHeight = 540;
constexpr int kMinProxyEdge = 16;
constexpr int kMaxBackgroundDelaySec = 600;

template<typename E>
struct EnumName
{
    E value;
    const char* id;
};

constexpr EnumName<ProxyResolution> kResolutionNames[] = {
    {ProxyResolution::Original, "original"},
    {ProxyResolution::Half, "half"},
    {ProxyResolution::Quarter, "quarter"},
    {ProxyResolution::Auto, "auto"},
};
constexpr EnumName<ProxyHandling> kHandlingNames[] = {
    {ProxyHandling::PreferProxies, "preferProxies"},
    {ProxyHandling::PreferOriginals, "preferOriginals"},
    {ProxyHandling::Disabled, "disabled"},
};
constexpr EnumName<RenderCacheMode> kCacheModeNames[] = {
    {RenderCacheMode::None, "none"},
    {RenderCacheMode::Smart, "smart"},
    {RenderCacheMode::User, "user"},
};

template<typename E, size_t N>
QString idOf(const EnumName<E> (&names)[N], E value)
{
    for (const auto& n : names) {
        if (n.value == value)
            return QLatin1String(n.id);
    }
    return QLatin1String(names[0].id);
}

template<typename E, size_t N>
E fromIdOf(const EnumName<E> (&names)[N], const QString& id, E fallback)
{
    for (const auto& n : names) {
        if (id == QLatin1String(n.id))
            return n.value;
    }
    return fallback;
}

QJsonObject fingerprintToJson(const SourceFingerprint& f)
{
    return {{QLatin1String("size"), f.size}, {QLatin1String("mtimeMs"), f.mtimeMs}};
}

SourceFingerprint fingerprintFromJson(const QJsonObject& o)
{
    SourceFingerprint f;
    f.size = o.value(QLatin1String("size")).toInteger(-1);
    f.mtimeMs = o.value(QLatin1String("mtimeMs")).toInteger(0);
    return f;
}

QJsonObject linkToJson(const MediaLink& l)
{
    QJsonObject o {{QLatin1String("path"), l.path},
        {QLatin1String("source"), fingerprintToJson(l.source)},
        {QLatin1String("width"), l.size.width()}, {QLatin1String("height"), l.size.height()}};
    if (!l.format.isEmpty())
        o[QLatin1String("format")] = l.format;
    return o;
}

MediaLink linkFromJson(const QJsonObject& o)
{
    MediaLink l;
    l.path = o.value(QLatin1String("path")).toString();
    l.source = fingerprintFromJson(o.value(QLatin1String("source")).toObject());
    l.size
        = QSize(o.value(QLatin1String("width")).toInt(), o.value(QLatin1String("height")).toInt());
    l.format = o.value(QLatin1String("format")).toString();
    return l;
}

} // namespace

// ---- SourceFingerprint ----

SourceFingerprint SourceFingerprint::of(const QString& path)
{
    // MediaSource: media inside a single-file project have a size and a time too.
    const media::MediaSource source = media::MediaSource::resolve(path);
    if (!source.exists())
        return {};
    if (!source.isEmbedded() && !QFileInfo(path).isFile())
        return {};
    return {source.size(), source.modifiedMs()};
}

// ---- Settings ----

QString proxyResolutionId(ProxyResolution r)
{
    return idOf(kResolutionNames, r);
}
ProxyResolution proxyResolutionFromId(const QString& id)
{
    return fromIdOf(kResolutionNames, id, ProxyResolution::Auto);
}
QString proxyHandlingId(ProxyHandling h)
{
    return idOf(kHandlingNames, h);
}
ProxyHandling proxyHandlingFromId(const QString& id)
{
    return fromIdOf(kHandlingNames, id, ProxyHandling::PreferProxies);
}
QString renderCacheModeId(RenderCacheMode m)
{
    return idOf(kCacheModeNames, m);
}
RenderCacheMode renderCacheModeFromId(const QString& id)
{
    return fromIdOf(kCacheModeNames, id, RenderCacheMode::None);
}

QJsonObject MediaCacheSettings::toJson() const
{
    return {{QLatin1String("proxyResolution"), proxyResolutionId(proxyResolution)},
        {QLatin1String("proxyFormat"), proxyFormat},
        {QLatin1String("optimizedResolution"), proxyResolutionId(optimizedResolution)},
        {QLatin1String("optimizedFormat"), optimizedFormat},
        {QLatin1String("renderCacheFormat"), renderCacheFormat},
        {QLatin1String("backgroundCache"), backgroundCache},
        {QLatin1String("backgroundCacheDelaySec"), backgroundCacheDelaySec},
        {QLatin1String("userCacheTransitions"), userCacheTransitions},
        {QLatin1String("userCacheComposites"), userCacheComposites},
        {QLatin1String("proxyHandling"), proxyHandlingId(proxyHandling)},
        {QLatin1String("useOptimizedMedia"), useOptimizedMedia},
        {QLatin1String("renderCache"), renderCacheModeId(renderCacheMode)}};
}

MediaCacheSettings MediaCacheSettings::fromJson(const QJsonObject& json)
{
    MediaCacheSettings s;
    auto str = [&json](const char* key, const QString& fallback) {
        const QString v = json.value(QLatin1String(key)).toString();
        return v.isEmpty() ? fallback : v;
    };
    s.proxyResolution = fromIdOf(kResolutionNames, str("proxyResolution", {}), s.proxyResolution);
    s.proxyFormat = str("proxyFormat", s.proxyFormat);
    s.optimizedResolution
        = fromIdOf(kResolutionNames, str("optimizedResolution", {}), s.optimizedResolution);
    s.optimizedFormat = str("optimizedFormat", s.optimizedFormat);
    s.renderCacheFormat = str("renderCacheFormat", s.renderCacheFormat);
    s.backgroundCache = json.value(QLatin1String("backgroundCache")).toBool(s.backgroundCache);
    s.backgroundCacheDelaySec = std::clamp(
        json.value(QLatin1String("backgroundCacheDelaySec")).toInt(s.backgroundCacheDelaySec), 0,
        kMaxBackgroundDelaySec);
    s.userCacheTransitions
        = json.value(QLatin1String("userCacheTransitions")).toBool(s.userCacheTransitions);
    s.userCacheComposites
        = json.value(QLatin1String("userCacheComposites")).toBool(s.userCacheComposites);
    s.proxyHandling = fromIdOf(kHandlingNames, str("proxyHandling", {}), s.proxyHandling);
    s.useOptimizedMedia
        = json.value(QLatin1String("useOptimizedMedia")).toBool(s.useOptimizedMedia);
    s.renderCacheMode = fromIdOf(kCacheModeNames, str("renderCache", {}), s.renderCacheMode);
    return s;
}

double proxyScale(ProxyResolution r, int sourceHeight)
{
    switch (r) {
    case ProxyResolution::Original:
        return 1.0;
    case ProxyResolution::Half:
        return 0.5;
    case ProxyResolution::Quarter:
        return 0.25;
    case ProxyResolution::Auto:
        break;
    }
    for (const double s : {1.0, 0.5}) {
        if (sourceHeight * s <= kAutoProxyMaxHeight)
            return s;
    }
    return 0.25;
}

QSize proxySize(ProxyResolution r, QSize display)
{
    const double s = proxyScale(r, display.height());
    auto even = [](double v) { return std::max(kMinProxyEdge, int(std::lround(v / 2.0)) * 2); };
    return {even(display.width() * s), even(display.height() * s)};
}

QString stableLinkFileName(const QString& originalPath)
{
    const QFileInfo fi(originalPath);
    const QByteArray hash
        = QCryptographicHash::hash(fi.absoluteFilePath().toUtf8(), QCryptographicHash::Sha1)
              .toHex()
              .left(8);
    QString base = fi.completeBaseName();
    // Image sequence paths ("shot_%04d.exr#fps=24") and odd names: keep a portable file name.
    for (QChar& c : base) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')
            && c != QLatin1Char('.'))
            c = QLatin1Char('_');
    }
    if (base.isEmpty())
        base = QStringLiteral("media");
    return base + QLatin1Char('_') + QString::fromLatin1(hash) + QStringLiteral(".mov");
}

// ---- MediaLinks ----

MediaLinks::MediaLinks(QObject* parent)
    : QObject(parent)
{ }

MediaLink MediaLinks::link(const QString& mediaId, LinkKind kind) const
{
    const auto it = m_links.constFind(mediaId);
    if (it == m_links.constEnd())
        return {};
    return kind == LinkKind::Proxy ? it->proxy : it->optimized;
}

void MediaLinks::setLink(const QString& mediaId, LinkKind kind, const MediaLink& link)
{
    if (link.isEmpty()) {
        clearLink(mediaId, kind);
        return;
    }
    Entry& e = m_links[mediaId];
    MediaLink& slot = kind == LinkKind::Proxy ? e.proxy : e.optimized;
    if (slot == link)
        return;
    slot = link;
    emit changed(mediaId);
}

void MediaLinks::clearLink(const QString& mediaId, LinkKind kind)
{
    const auto it = m_links.find(mediaId);
    if (it == m_links.end())
        return;
    MediaLink& slot = kind == LinkKind::Proxy ? it->proxy : it->optimized;
    if (slot.isEmpty())
        return;
    slot = {};
    if (it->proxy.isEmpty() && it->optimized.isEmpty())
        m_links.erase(it);
    emit changed(mediaId);
}

void MediaLinks::clear()
{
    if (m_links.isEmpty())
        return;
    m_links.clear();
    emit reset();
}

LinkState MediaLinks::state(
    const QString& mediaId, LinkKind kind, const QString& originalPath) const
{
    const MediaLink l = link(mediaId, kind);
    if (l.isEmpty())
        return LinkState::None;
    if (!QFileInfo::exists(l.path))
        return LinkState::Missing;
    const SourceFingerprint now = SourceFingerprint::of(originalPath);
    // An offline original cannot be checked; the proxy is exactly what stands in for it.
    if (!now.isValid() || !l.source.isValid())
        return LinkState::Ready;
    return now == l.source ? LinkState::Ready : LinkState::Stale;
}

QJsonArray MediaLinks::toJson(const MediaPoolState& pool) const
{
    QJsonArray out;
    for (const MediaItem& item : pool.items) {
        const auto it = m_links.constFind(item.id);
        if (it == m_links.constEnd())
            continue;
        QJsonObject o {{QLatin1String("mediaId"), item.id}};
        if (!it->proxy.isEmpty())
            o[QLatin1String("proxy")] = linkToJson(it->proxy);
        if (!it->optimized.isEmpty())
            o[QLatin1String("optimized")] = linkToJson(it->optimized);
        out.append(o);
    }
    return out;
}

void MediaLinks::fromJson(const QJsonArray& json)
{
    m_links.clear();
    for (const QJsonValue& v : json) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QLatin1String("mediaId")).toString();
        if (id.isEmpty())
            continue;
        Entry e;
        e.proxy = linkFromJson(o.value(QLatin1String("proxy")).toObject());
        e.optimized = linkFromJson(o.value(QLatin1String("optimized")).toObject());
        if (!e.proxy.isEmpty() || !e.optimized.isEmpty())
            m_links.insert(id, e);
    }
    emit reset();
}

int MediaLinks::autoLink(const MediaPoolState& pool, LinkKind kind, const QString& folder)
{
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return 0;
    const QDir dir(folder);
    int linked = 0;
    for (const MediaItem& item : pool.items) {
        if (item.isTimeline() || item.path.isEmpty() || !link(item.id, kind).isEmpty())
            continue;
        const QString file = dir.absoluteFilePath(stableLinkFileName(item.path));
        MediaLink l;
        if (!QFileInfo::exists(file) || !readSidecar(file, item.path, &l))
            continue;
        l.path = file;
        // Directly, not through setLink(): finding a proxy is not a change of the project
        // (no changed() -> the project does not become modified); one reset() at the end.
        Entry& e = m_links[item.id];
        (kind == LinkKind::Proxy ? e.proxy : e.optimized) = l;
        ++linked;
    }
    if (linked > 0)
        emit reset();
    return linked;
}

bool MediaLinks::writeSidecar(
    const QString& file, const QString& originalPath, const MediaLink& link)
{
    QSaveFile out(file + QStringLiteral(".json"));
    if (!out.open(QIODevice::WriteOnly))
        return false;
    QJsonObject o = linkToJson(link);
    o.remove(QLatin1String("path"));
    o[QLatin1String("original")] = QFileInfo(originalPath).absoluteFilePath();
    out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return out.commit();
}

bool MediaLinks::readSidecar(const QString& file, const QString& originalPath, MediaLink* link)
{
    QFile in(file + QStringLiteral(".json"));
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(in.readAll()).object();
    if (o.value(QLatin1String("original")).toString() != QFileInfo(originalPath).absoluteFilePath())
        return false;
    *link = linkFromJson(o);
    link->path = file;
    return true;
}

} // namespace kk
