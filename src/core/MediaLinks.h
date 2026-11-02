// SPDX-License-Identifier: GPL-3.0-or-later
// MediaLinks — proxy and optimized media linked to media pool items, their stale detection,
// and the project's "Optimized Media and Render Cache" settings (Resolve's Master Settings
// section of the same name plus the Playback menu's proxy / cache modes).
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk {

struct MediaPoolState;

/// Which kind of substitute media a link describes.
///
/// - **Proxy**: a small, fast-to-decode copy (default H.264 short GOP at half/quarter size)
///   used by the viewers when proxy handling allows it. Portable files in the proxy folder
///   (Preferences > Media Storage); they can be linked by hand ("Link Proxy Media").
/// - **Optimized**: an edit-friendly intermediate at full size by default (DNxHR LB, intra
///   only) managed by Kinoklip in the cache folder; used when "Use Optimized Media If
///   Available" is on and no proxy is used.
enum class LinkKind {
    Proxy,
    Optimized
};

/// Size + modification time of a file: enough to notice that a source was replaced or edited
/// since a proxy was made from it (Resolve does not check this; we do, cheaply).
struct SourceFingerprint
{
    qint64 size = -1; ///< bytes; -1 = unknown (file missing when the fingerprint was taken)
    qint64 mtimeMs = 0; ///< last modification, ms since the epoch (UTC)

    /// Fingerprint of @p path now (size -1 when it does not exist). One stat() call.
    [[nodiscard]] static SourceFingerprint of(const QString& path);
    [[nodiscard]] bool isValid() const { return size >= 0; }
    friend bool operator==(const SourceFingerprint&, const SourceFingerprint&) = default;
};

/// One proxy or optimized media file linked to a media pool item.
struct MediaLink
{
    QString path; ///< absolute path of the proxy / optimized file; empty = no link
    SourceFingerprint source; ///< the original's fingerprint when the file was made / linked
    QSize size; ///< picture size of the linked file (informational: media pool column)
    QString format; ///< format id it was made with ("h264", "dnxhr_lb", ...; "" = linked by hand)

    [[nodiscard]] bool isEmpty() const { return path.isEmpty(); }
    friend bool operator==(const MediaLink&, const MediaLink&) = default;
};

/// State of a link, as shown by the media pool badge / column.
enum class LinkState {
    None, ///< no link
    Ready, ///< the file exists and the original is unchanged (or offline)
    Stale, ///< the original changed since the file was made: not used until regenerated
    Missing ///< the linked file is gone
};

/// Proxy / optimized media resolution (Resolve: "Proxy media resolution", "Optimized media
/// resolution"). Auto = "Choose automatically".
enum class ProxyResolution {
    Original,
    Half,
    Quarter,
    Auto
};

/// Playback > Proxy Handling.
enum class ProxyHandling {
    PreferProxies, ///< use a ready proxy whenever one is linked
    PreferOriginals, ///< use the camera originals; proxies only stand in for offline media
    Disabled ///< never use proxies ("Disable All Proxies")
};

/// Playback > Render Cache.
enum class RenderCacheMode {
    None, ///< no render cache (default, like a new Resolve project)
    Smart, ///< cache heavy sections automatically (transitions, composites, effects, nested)
    User ///< cache only clips marked "Render Cache Clip Output" (+ the two options below)
};

/// Project settings of the "Optimized Media and Render Cache" section (Project Settings >
/// Master Settings in Resolve) and the per-project Playback menu modes. Saved with the
/// project; changing them is not undoable (like Resolve) but makes the project modified.
struct MediaCacheSettings
{
    ProxyResolution proxyResolution = ProxyResolution::Auto;
    QString proxyFormat = QStringLiteral("h264"); ///< see export/ProxyFormats.h
    ProxyResolution optimizedResolution = ProxyResolution::Original;
    QString optimizedFormat = QStringLiteral("dnxhr_lb");
    QString renderCacheFormat = QStringLiteral("dnxhr_lb");
    bool backgroundCache = true; ///< "Enable background caching after N seconds"
    int backgroundCacheDelaySec = 5; ///< idle time before background caching starts
    bool userCacheTransitions = false; ///< "Automatically cache transitions in User Mode"
    bool userCacheComposites = false; ///< "Automatically cache composites in User Mode"
    ProxyHandling proxyHandling = ProxyHandling::PreferProxies;
    bool useOptimizedMedia = true; ///< Playback > Use Optimized Media If Available
    RenderCacheMode renderCacheMode = RenderCacheMode::None;

    [[nodiscard]] QJsonObject toJson() const;
    /// Reads toJson() output; missing / unknown values keep the defaults.
    [[nodiscard]] static MediaCacheSettings fromJson(const QJsonObject& json);
    friend bool operator==(const MediaCacheSettings&, const MediaCacheSettings&) = default;
};

/// Stable ids for the enums (project files, settings combos).
[[nodiscard]] QString proxyResolutionId(ProxyResolution r);
[[nodiscard]] ProxyResolution proxyResolutionFromId(const QString& id);
[[nodiscard]] QString proxyHandlingId(ProxyHandling h);
[[nodiscard]] ProxyHandling proxyHandlingFromId(const QString& id);
[[nodiscard]] QString renderCacheModeId(RenderCacheMode m);
[[nodiscard]] RenderCacheMode renderCacheModeFromId(const QString& id);

/// Scale factor (1, 1/2, 1/4) for a source @p sourceHeight lines high. Auto = the largest of
/// Original / Half / Quarter whose height is at most 540 lines (Quarter if none is): 1080p
/// and 4K sources both give a 540-line proxy that any laptop decodes in a few ms.
[[nodiscard]] double proxyScale(ProxyResolution r, int sourceHeight);

/// Picture size of a proxy of a source whose upright display size is @p display: scaled by
/// proxyScale(), rounded to even numbers (4:2:0 codecs), at least 16x16.
[[nodiscard]] QSize proxySize(ProxyResolution r, QSize display);

/// Stable file name of the proxy / optimized file of @p originalPath:
/// "<base name>_<first 8 hex digits of sha1(absolute path)>.mov". Regenerating overwrites
/// the same file; two sources with the same name in different folders never collide.
[[nodiscard]] QString stableLinkFileName(const QString& originalPath);

/// The proxy / optimized media (one entry per media pool item id) of a project.
///
/// Not part of the undo history (generating a proxy is not an edit, and undoing an edit must
/// not unlink it), saved with the project ("mediaLinks"). The Project owns one.
/// Thread-safety: GUI thread only.
class MediaLinks : public QObject
{
    Q_OBJECT
public:
    explicit MediaLinks(QObject* parent = nullptr);

    /// Link of @p kind for item @p mediaId (empty when none).
    [[nodiscard]] MediaLink link(const QString& mediaId, LinkKind kind) const;
    /// Sets (or replaces) a link; emits changed(mediaId).
    void setLink(const QString& mediaId, LinkKind kind, const MediaLink& link);
    /// Removes a link; emits changed(mediaId) if there was one.
    void clearLink(const QString& mediaId, LinkKind kind);
    /// Removes every link (new project, load).
    void clear();
    /// Number of items with at least one link.
    [[nodiscard]] int size() const { return int(m_links.size()); }
    /// Item ids with at least one link.
    [[nodiscard]] QStringList ids() const { return m_links.keys(); }

    /// State of a link given the original file @p originalPath. Stats the files (GUI thread,
    /// two stat() calls); call it when the pool changes, not while painting.
    [[nodiscard]] LinkState state(
        const QString& mediaId, LinkKind kind, const QString& originalPath) const;

    /// JSON array for the project file: only links of items in @p pool are written.
    [[nodiscard]] QJsonArray toJson(const MediaPoolState& pool) const;
    /// Replaces the contents with toJson() output (emits reset()).
    void fromJson(const QJsonArray& json);

    /// "Link proxies on project load": for every file item without a link of @p kind, a file
    /// with the stable name (stableLinkFileName()) in @p folder is linked if its sidecar
    /// (written by the generator, see writeSidecar()) names that original. Returns the number
    /// of new links. Stats a few files per item (project load / import only).
    int autoLink(const MediaPoolState& pool, LinkKind kind, const QString& folder);

    /// Writes "<file>.json" next to a generated file: original path, its fingerprint, size,
    /// format. Lets autoLink() find and validate the file in other projects.
    static bool writeSidecar(
        const QString& file, const QString& originalPath, const MediaLink& link);
    /// Reads a sidecar written by writeSidecar(); false if absent or not for @p originalPath.
    static bool readSidecar(const QString& file, const QString& originalPath, MediaLink* link);

signals:
    /// A link of item @p mediaId was added, changed or removed.
    void changed(const QString& mediaId);
    /// Everything was replaced.
    void reset();

private:
    struct Entry
    {
        MediaLink proxy;
        MediaLink optimized;
    };
    QHash<QString, Entry> m_links;
};

} // namespace kk
