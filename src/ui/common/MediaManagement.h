// SPDX-License-Identifier: GPL-3.0-or-later
// MediaManagement — application glue for proxy media, optimized media and the render cache:
// the media pool / Playback / Clip commands, the background queues, the proxy map handed to
// every viewer, the cache view handed to program playback, and per-item status for the media
// pool (badges, columns).
#pragma once

#include "core/MediaLinks.h"
#include "render/RenderTypes.h"

#include <QObject>
#include <QString>

#include <memory>

class QWidget;

namespace kk {

struct AppContext;
class Compositor;
class ProxyQueue;
class RenderCacheManager;

/// Status of one media pool item's proxy and optimized media, formatted for the media pool.
struct MediaLinkStatus
{
    /// Tile badge: "" (none), "P" (proxy in use), "O" (optimized media), "P 42%" (generating),
    /// "P!" (stale or missing proxy).
    QString badge;
    QString proxyText; ///< list column: "None", "960x540", "Generating 42%", "Stale", "Missing"
    QString optimizedText; ///< same for optimized media
    bool problem = false; ///< stale / missing: the badge is drawn as a warning
};

/// Owns the ProxyQueue, the RenderCacheManager and a background Compositor shared by both
/// (created on first use). Created in main() after the AppContext, before the main window;
/// registers itself as AppContext::media.
///
/// Media pool commands (`mediapool.*`) act on the ids stored in the action's data by the
/// media pool panel (MediaPoolPanel keeps them up to date with its selection).
///
/// Thread-safety: GUI thread only.
class MediaManagement : public QObject
{
    Q_OBJECT
public:
    /// Connects the commands of @p ctx.actions (if any) and follows @p ctx.project.
    explicit MediaManagement(AppContext& ctx, QObject* parent = nullptr);
    ~MediaManagement() override;

    [[nodiscard]] ProxyQueue* proxyQueue() const { return m_queue.get(); }
    [[nodiscard]] RenderCacheManager* renderCache() const { return m_cache.get(); }
    /// Proxy map currently used by the viewers (Playback > Proxy Handling applied).
    [[nodiscard]] const std::shared_ptr<const ProxyMap>& proxies() const { return m_proxies; }
    /// Media table for playback (offline originals play their proxy's sound).
    [[nodiscard]] const std::shared_ptr<const MediaPathMap>& playbackMedia() const
    {
        return m_playbackMedia;
    }
    /// Status of item @p mediaId (stats a few files; callers cache it).
    [[nodiscard]] MediaLinkStatus status(const QString& mediaId) const;

    /// Queues proxy (or optimized) media generation for @p ids. Items that cannot have one
    /// (audio, stills, timelines) are skipped and reported in @p skipped. @return queued count.
    int generate(const QStringList& ids, LinkKind kind, QStringList* skipped = nullptr);
    /// Links @p file as the proxy of item @p mediaId ("Link Proxy Media"). @return false with
    /// @p error when the file is not usable video.
    bool linkProxy(const QString& mediaId, const QString& file, QString* error = nullptr);
    /// Removes the link only (the file stays).
    void unlink(const QStringList& ids, LinkKind kind);
    /// Deletes generated files (only inside the managed folders) and the links.
    void deleteLinked(const QStringList& ids, LinkKind kind);

    /// Rebuilds the proxy map now (normally automatic on every relevant change).
    void refresh();
    /// Links proxies / optimized media found in the folders by their stable names (project
    /// load, import; automatic).
    void autoLink();
    /// Preferences were saved: follows a moved cache / proxy folder.
    void preferencesChanged();

Q_SIGNALS:
    /// The proxy map changed (viewers: SourcePlayer::engine()->setSubstitutes()).
    void proxiesChanged(std::shared_ptr<const ProxyMap> proxies);
    /// status(@p mediaId) may have changed.
    void statusChanged(const QString& mediaId);
    /// Every status may have changed (project loaded, folders re-checked).
    void statusReset();

private:
    void connectActions();
    void syncActions();
    void setSettings(const MediaCacheSettings& settings);
    void ensureCompositor();
    [[nodiscard]] QStringList selection(const char* actionId) const;
    [[nodiscard]] QWidget* dialogParent() const;

    AppContext& m_ctx;
    std::unique_ptr<Compositor> m_compositor;
    std::unique_ptr<ProxyQueue> m_queue;
    std::unique_ptr<RenderCacheManager> m_cache;
    std::shared_ptr<const ProxyMap> m_proxies;
    std::shared_ptr<const MediaPathMap> m_playbackMedia;
    bool m_syncing = false;
};

} // namespace kk
