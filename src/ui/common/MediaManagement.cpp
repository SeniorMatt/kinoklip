// SPDX-License-Identifier: GPL-3.0-or-later
// MediaManagement — proxy / optimized media and render cache commands and wiring.
#include "ui/common/MediaManagement.h"

#include "core/EditOps.h"
#include "core/Plural.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "export/ProxyQueue.h"
#include "export/RenderCacheManager.h"
#include "media/MediaInfo.h"
#include "platform/FileDialogs.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QWidget>

#include <cmath>

namespace kk {

namespace {

/// A linked file may differ in length from its original by this much before we warn.
constexpr int64_t kLinkDurationToleranceUs = 1'000'000;

QString sizeText(const MediaLink& link)
{
    return link.size.isEmpty()
        ? QObject::tr("Linked")
        : QStringLiteral("%1x%2").arg(link.size.width()).arg(link.size.height());
}

} // namespace

MediaManagement::MediaManagement(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_queue(std::make_unique<ProxyQueue>(ctx.project->mediaLinks()))
    , m_cache(std::make_unique<RenderCacheManager>(ctx.project))
{
    ctx.media = this;
    Project* project = ctx.project;
    MediaLinks* links = project->mediaLinks();

    connect(m_queue.get(), &ProxyQueue::changed, this, &MediaManagement::statusChanged);
    connect(links, &MediaLinks::changed, this, [this](const QString& id) {
        refresh();
        emit statusChanged(id);
    });
    connect(links, &MediaLinks::reset, this, [this] {
        refresh();
        emit statusReset();
    });
    connect(project->mediaPool(), &MediaPool::changed, this, &MediaManagement::refresh);
    connect(project->mediaPool(), &MediaPool::itemAdded, this, [this] { autoLink(); });
    connect(project, &Project::projectReset, this, [this] {
        autoLink();
        refresh();
        emit statusReset();
    });
    connect(project, &Project::cacheSettingsChanged, this, [this] {
        if (m_ctx.project->cacheSettings().renderCacheMode != RenderCacheMode::None)
            ensureCompositor();
        refresh();
        syncActions();
    });
    // Files appear / disappear while the application is in the background (a drive is
    // plugged in, proxies are copied): re-check when it becomes active again.
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(
            app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
                if (s == Qt::ApplicationActive) {
                    refresh();
                    emit statusReset();
                }
            });
    }

    if (PlaybackEngine* engine = ctx.playback) {
        connect(m_cache.get(), &RenderCacheManager::viewChanged, engine,
            &PlaybackEngine::setRenderCache);
        engine->setRenderCache(m_cache->view());
        // Background caching pauses while anything plays (Resolve does the same).
        connect(engine, &PlaybackEngine::stateChanged, this, [this](PlaybackEngine::State s) {
            m_cache->setPaused(s != PlaybackEngine::State::Stopped);
        });
    }
    if (project->cacheSettings().renderCacheMode != RenderCacheMode::None)
        ensureCompositor();
    autoLink();
    refresh();
    connectActions();
    syncActions();
}

MediaManagement::~MediaManagement()
{
    if (m_ctx.media == this)
        m_ctx.media = nullptr;
    // Queues first (they wait for their workers), then the compositor they render with.
    m_cache.reset();
    m_queue.reset();
    m_compositor.reset();
}

void MediaManagement::ensureCompositor()
{
    if (m_compositor)
        return;
    m_compositor = std::make_unique<Compositor>(); // GUI thread (QOffscreenSurface)
    m_queue->setCompositor(m_compositor.get());
    m_cache->setCompositor(m_compositor.get());
}

void MediaManagement::autoLink()
{
    const MediaPoolState& pool = m_ctx.project->mediaPool()->state();
    MediaLinks* links = m_ctx.project->mediaLinks();
    links->autoLink(pool, LinkKind::Proxy, prefs::proxyDirectory());
    links->autoLink(pool, LinkKind::Optimized, prefs::optimizedMediaDirectory());
}

void MediaManagement::refresh()
{
    const MediaPoolState& pool = m_ctx.project->mediaPool()->state();
    const MediaLinks& links = *m_ctx.project->mediaLinks();
    const MediaCacheSettings& s = m_ctx.project->cacheSettings();
    m_proxies = makeProxyMap(pool, links, s.proxyHandling, s.useOptimizedMedia);
    m_playbackMedia = makeAudioPathMap(pool, links, s.proxyHandling);
    if (m_ctx.playback)
        m_ctx.playback->setSubstitutes(m_proxies, m_playbackMedia);
    emit proxiesChanged(m_proxies);
}

void MediaManagement::preferencesChanged()
{
    m_cache->setFolder(prefs::renderCacheDirectory());
    autoLink();
    refresh();
    emit statusReset();
}

MediaLinkStatus MediaManagement::status(const QString& mediaId) const
{
    MediaLinkStatus st;
    const MediaItem* item = m_ctx.project->mediaPool()->item(mediaId);
    if (!item || item->isTimeline())
        return st;
    const MediaLinks& links = *m_ctx.project->mediaLinks();
    auto describe = [&](LinkKind kind, QString* text, const char* letter) {
        const double progress = m_queue->progress(mediaId, kind);
        if (progress >= 0) {
            const int percent = int(std::floor(progress * 100));
            *text = tr("Generating %1%").arg(percent);
            if (st.badge.isEmpty())
                st.badge = QStringLiteral("%1 %2%").arg(QLatin1String(letter)).arg(percent);
            return;
        }
        switch (links.state(mediaId, kind, item->path)) {
        case LinkState::None:
            *text = tr("None");
            break;
        case LinkState::Ready:
            *text = sizeText(links.link(mediaId, kind));
            if (st.badge.isEmpty())
                st.badge = QLatin1String(letter);
            break;
        case LinkState::Stale:
            *text = tr("Stale (source changed)");
            st.badge = QLatin1String(letter) + QLatin1Char('!');
            st.problem = true;
            break;
        case LinkState::Missing:
            *text = tr("Missing");
            st.badge = QLatin1String(letter) + QLatin1Char('!');
            st.problem = true;
            break;
        }
    };
    describe(LinkKind::Proxy, &st.proxyText, "P");
    describe(LinkKind::Optimized, &st.optimizedText, "O");
    return st;
}

int MediaManagement::generate(const QStringList& ids, LinkKind kind, QStringList* skipped)
{
    const QString folder
        = kind == LinkKind::Proxy ? prefs::proxyDirectory() : prefs::optimizedMediaDirectory();
    int queued = 0;
    for (const QString& id : ids) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (!item)
            continue;
        ProxyJobSpec spec;
        QString why;
        if (!makeProxyJob(*item, kind, m_ctx.project->cacheSettings(), folder, &spec, &why)) {
            if (skipped)
                skipped->append(why);
            continue;
        }
        ensureCompositor();
        if (m_queue->enqueue(spec))
            ++queued;
    }
    return queued;
}

bool MediaManagement::linkProxy(const QString& mediaId, const QString& file, QString* error)
{
    const MediaItem* item = m_ctx.project->mediaPool()->item(mediaId);
    const media::MediaInfo info = media::probe(file);
    if (!item || !info.valid || !info.hasVideo || info.isStillImage) {
        if (error)
            *error = tr("%1 is not a video file.").arg(QFileInfo(file).fileName());
        return false;
    }
    if (std::abs(info.durationUs - item->info.durationUs) > kLinkDurationToleranceUs) {
        if (error)
            *error = tr("%1 is %2 s long, %3 is %4 s: not a proxy of it.")
                         .arg(QFileInfo(file).fileName())
                         .arg(info.durationUs / 1e6, 0, 'f', 1)
                         .arg(item->name)
                         .arg(item->info.durationUs / 1e6, 0, 'f', 1);
        return false;
    }
    MediaLink link;
    link.path = QFileInfo(file).absoluteFilePath();
    link.source = SourceFingerprint::of(item->path);
    link.size = QSize(info.displayWidth, info.displayHeight);
    m_ctx.project->mediaLinks()->setLink(mediaId, LinkKind::Proxy, link);
    return true;
}

void MediaManagement::unlink(const QStringList& ids, LinkKind kind)
{
    for (const QString& id : ids)
        m_ctx.project->mediaLinks()->clearLink(id, kind);
}

void MediaManagement::deleteLinked(const QStringList& ids, LinkKind kind)
{
    const QString folder
        = kind == LinkKind::Proxy ? prefs::proxyDirectory() : prefs::optimizedMediaDirectory();
    for (const QString& id : ids) {
        m_queue->cancel(id, kind);
        ProxyQueue::deleteLinked(m_ctx.project->mediaLinks(), id, kind, folder);
    }
}

QStringList MediaManagement::selection(const char* actionId) const
{
    if (!m_ctx.actions || !m_ctx.actions->contains(QLatin1String(actionId)))
        return {};
    return m_ctx.actions->action(QLatin1String(actionId))->data().toStringList();
}

QWidget* MediaManagement::dialogParent() const
{
    return QApplication::activeWindow();
}

void MediaManagement::setSettings(const MediaCacheSettings& settings)
{
    if (!m_syncing)
        m_ctx.project->setCacheSettings(settings);
}

void MediaManagement::connectActions()
{
    ActionRegistry* actions = m_ctx.actions;
    if (!actions)
        return;
    auto on = [this, actions](const char* id, auto&& fn) {
        if (actions->contains(QLatin1String(id)))
            connect(actions->action(QLatin1String(id)), &QAction::triggered, this, fn);
    };

    // ---- Media pool ----
    auto generateFor = [this](LinkKind kind, const char* actionId) {
        QStringList skipped;
        const QStringList ids = selection(actionId);
        if (generate(ids, kind, &skipped) == 0 && !skipped.isEmpty()) {
            QMessageBox::information(dialogParent(),
                kind == LinkKind::Proxy ? tr("Generate Proxy Media")
                                        : tr("Generate Optimized Media"),
                skipped.join(QLatin1Char('\n')));
        }
    };
    on("mediapool.generate_proxy",
        [generateFor] { generateFor(LinkKind::Proxy, "mediapool.generate_proxy"); });
    on("mediapool.generate_optimized",
        [generateFor] { generateFor(LinkKind::Optimized, "mediapool.generate_optimized"); });
    on("mediapool.link_proxy", [this] {
        const QStringList ids = selection("mediapool.link_proxy");
        if (ids.size() != 1)
            return;
        const QString file = platform::getOpenFileName(dialogParent(), tr("Link Proxy Media"),
            tr("Video files (*.mov *.mp4 *.mkv *.mxf *.avi);;All files (*)"),
            QStringLiteral("dialogs/linkProxy"), prefs::proxyDirectory());
        if (file.isEmpty())
            return;
        QString error;
        if (!linkProxy(ids.front(), file, &error))
            QMessageBox::warning(dialogParent(), tr("Link Proxy Media"), error);
    });
    on("mediapool.unlink_proxy",
        [this] { unlink(selection("mediapool.unlink_proxy"), LinkKind::Proxy); });
    on("mediapool.delete_proxy", [this] {
        const QStringList ids = selection("mediapool.delete_proxy");
        if (!ids.isEmpty()
            && QMessageBox::question(dialogParent(), tr("Delete Proxy Media"),
                   plural(ids.size(),
                       tr("Delete the proxy media of %n clip? A proxy you linked by hand is "
                          "only unlinked."),
                       tr("Delete the proxy media of %n clips? Proxies you linked by hand are "
                          "only unlinked.")))
                == QMessageBox::Yes)
            deleteLinked(ids, LinkKind::Proxy);
    });
    on("mediapool.delete_optimized", [this] {
        const QStringList ids = selection("mediapool.delete_optimized");
        if (!ids.isEmpty()
            && QMessageBox::question(dialogParent(), tr("Delete Optimized Media"),
                   plural(ids.size(), tr("Delete the optimized media of %n clip?"),
                       tr("Delete the optimized media of %n clips?")))
                == QMessageBox::Yes)
            deleteLinked(ids, LinkKind::Optimized);
    });
    on("mediapool.stop_generation", [this] {
        for (const QString& id : selection("mediapool.stop_generation")) {
            m_queue->cancel(id, LinkKind::Proxy);
            m_queue->cancel(id, LinkKind::Optimized);
        }
    });

    // ---- Playback ----
    auto handling = [this](ProxyHandling h) {
        MediaCacheSettings s = m_ctx.project->cacheSettings();
        s.proxyHandling = h;
        setSettings(s);
    };
    on("playback.proxy_prefer", [handling] { handling(ProxyHandling::PreferProxies); });
    on("playback.proxy_originals", [handling] { handling(ProxyHandling::PreferOriginals); });
    on("playback.proxy_disable", [handling] { handling(ProxyHandling::Disabled); });
    on("playback.use_proxy", [handling](bool checked) {
        handling(checked ? ProxyHandling::PreferProxies : ProxyHandling::Disabled);
    });
    on("playback.use_optimized", [this](bool checked) {
        MediaCacheSettings s = m_ctx.project->cacheSettings();
        s.useOptimizedMedia = checked;
        setSettings(s);
    });
    auto cacheMode = [this](RenderCacheMode m) {
        MediaCacheSettings s = m_ctx.project->cacheSettings();
        s.renderCacheMode = m;
        setSettings(s);
    };
    on("playback.render_cache_none", [cacheMode] { cacheMode(RenderCacheMode::None); });
    on("playback.render_cache_smart", [cacheMode] { cacheMode(RenderCacheMode::Smart); });
    on("playback.render_cache_user", [cacheMode] { cacheMode(RenderCacheMode::User); });
    on("playback.delete_render_cache", [this] {
        if (QMessageBox::question(dialogParent(), tr("Delete Render Cache"),
                tr("Delete every cached frame in %1?").arg(m_cache->folder()))
            == QMessageBox::Yes)
            m_cache->deleteAll();
    });

    // ---- Clip > Render Cache Clip Output (undoable, selected clips of the active timeline)
    on("clip.render_cache_output", [this](bool checked) {
        if (!m_ctx.selection || m_ctx.selection->clips().isEmpty())
            return;
        Project* project = m_ctx.project;
        project->beginEdit(tr("Render Cache Clip Output"));
        TimelineData& data = project->timeline()->mutableData();
        for (const quint64 id : m_ctx.selection->clips()) {
            if (ClipData* c = editops::clipById(data, id))
                c->cacheOutput = checked;
        }
        project->endEdit();
    });
    if (m_ctx.selection && actions->contains(QStringLiteral("clip.render_cache_output"))) {
        auto syncClipFlag = [this] {
            QAction* a = m_ctx.actions->action(QStringLiteral("clip.render_cache_output"));
            const TimelineData& data = m_ctx.project->timeline()->data();
            bool any = false;
            for (const quint64 id : m_ctx.selection->clips()) {
                if (const ClipData* c = editops::clipById(data, id); c && c->cacheOutput)
                    any = true;
            }
            const QSignalBlocker block(a);
            a->setChecked(any);
        };
        connect(m_ctx.selection, &SelectionModel::changed, this, syncClipFlag);
        connect(m_ctx.project->timeline(), &Timeline::changed, this, syncClipFlag);
    }
}

void MediaManagement::syncActions()
{
    ActionRegistry* actions = m_ctx.actions;
    if (!actions)
        return;
    const MediaCacheSettings& s = m_ctx.project->cacheSettings();
    m_syncing = true;
    auto check = [actions](const char* id, bool on) {
        if (actions->contains(QLatin1String(id))) {
            QAction* a = actions->action(QLatin1String(id));
            const QSignalBlocker block(a);
            a->setChecked(on);
        }
    };
    check("playback.proxy_prefer", s.proxyHandling == ProxyHandling::PreferProxies);
    check("playback.proxy_originals", s.proxyHandling == ProxyHandling::PreferOriginals);
    check("playback.proxy_disable", s.proxyHandling == ProxyHandling::Disabled);
    check("playback.use_proxy", s.proxyHandling != ProxyHandling::Disabled);
    check("playback.use_optimized", s.useOptimizedMedia);
    check("playback.render_cache_none", s.renderCacheMode == RenderCacheMode::None);
    check("playback.render_cache_smart", s.renderCacheMode == RenderCacheMode::Smart);
    check("playback.render_cache_user", s.renderCacheMode == RenderCacheMode::User);
    m_syncing = false;
}

} // namespace kk
