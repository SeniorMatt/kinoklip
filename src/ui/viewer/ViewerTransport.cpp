// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerTransport — global transport actions -> active viewer.
#include "ui/viewer/ViewerTransport.h"

#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QShortcut>

namespace kk {

namespace {
/// Shuttle action ids by ShuttleControl::Key order (J, K, L).
const char* const kShuttleIds[3]
    = {"transport.shuttle_reverse", "transport.shuttle_stop", "transport.shuttle_forward"};

/// The ViewerPanel containing @p w, or nullptr.
ViewerPanel* viewerOf(QWidget* w)
{
    for (; w; w = w->parentWidget()) {
        if (auto* v = qobject_cast<ViewerPanel*>(w)) {
            return v;
        }
    }
    return nullptr;
}

bool tagged(QWidget* w)
{
    for (; w; w = w->parentWidget()) {
        if (w->property("kkProgramFocus").toBool()) {
            return true;
        }
    }
    return false;
}
} // namespace

ViewerTransport::ViewerTransport(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    connect(qApp, &QApplication::focusChanged, this, &ViewerTransport::onFocusChanged);
    qApp->installEventFilter(this);
}

ViewerTransport::~ViewerTransport()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    delete m_fullScreen.data();
}

ViewerPanel* ViewerTransport::targetViewer() const
{
    ViewerPanel* active = ViewerPanel::activeViewer();
    if (active && active->isVisible()) {
        return active;
    }
    ViewerPanel* firstVisible = nullptr;
    for (ViewerPanel* v : ViewerPanel::allViewers()) {
        if (!v->isVisible()) {
            continue;
        }
        if (v->mode() == ViewerPanel::Mode::Timeline) {
            return v;
        }
        if (!firstVisible) {
            firstVisible = v;
        }
    }
    return firstVisible;
}

PlaybackEngine* ViewerTransport::targetEngine() const
{
    if (ViewerPanel* v = targetViewer()) {
        return v->engine(); // may be null: an empty source viewer ignores transport keys
    }
    return m_ctx.playback;
}

void ViewerTransport::connectActions()
{
    if (m_connected || !m_ctx.actions) {
        return;
    }
    m_connected = true;
    ActionRegistry* actions = m_ctx.actions;
    auto on = [this, actions](const char* id, auto fn) {
        const QString key = QLatin1String(id);
        if (!actions->contains(key)) {
            return static_cast<QAction*>(nullptr);
        }
        QAction* a = actions->action(key);
        connect(a, &QAction::triggered, this, [this, fn] {
            if (PlaybackEngine* e = targetEngine()) {
                fn(e);
            }
        });
        return a;
    };
    on("transport.play_pause", [](PlaybackEngine* e) { e->togglePlay(); });
    on("transport.shuttle_reverse", [](PlaybackEngine* e) { e->shuttleReverse(); });
    on("transport.shuttle_stop", [](PlaybackEngine* e) { e->shuttleStop(); });
    on("transport.shuttle_forward", [](PlaybackEngine* e) { e->shuttleForward(); });
    on("transport.prev_frame", [](PlaybackEngine* e) { e->step(-1); });
    on("transport.next_frame", [](PlaybackEngine* e) { e->step(1); });
    on("transport.back_1s", [](PlaybackEngine* e) { e->stepSeconds(-1); });
    on("transport.forward_1s", [](PlaybackEngine* e) { e->stepSeconds(1); });
    on("transport.play_in_out", [](PlaybackEngine* e) { e->playInToOut(); });
    on("transport.play_around", [](PlaybackEngine* e) { e->playAroundCurrent(); });
    // A held key must not re-trigger: J/L would race to 8x, Space would toggle repeatedly.
    for (const char* id :
        {"transport.play_pause", "transport.shuttle_reverse", "transport.shuttle_stop",
            "transport.shuttle_forward", "transport.play_in_out", "transport.play_around"}) {
        if (actions->contains(QLatin1String(id))) {
            actions->action(QLatin1String(id))->setAutoRepeat(false);
        }
    }
    if (actions->contains(QStringLiteral("view.fullscreen_viewer"))) {
        connect(actions->action(QStringLiteral("view.fullscreen_viewer")), &QAction::triggered,
            this, [this] { setFullScreenViewer(m_fullScreen.isNull()); });
    }
}

void ViewerTransport::setFullScreenViewer(bool on)
{
    if (!on) {
        if (m_fullScreen) {
            m_fullScreen->close(); // WA_DeleteOnClose
        }
        return;
    }
    if (m_fullScreen || !m_ctx.playback) {
        return;
    }
    QWidget* window = QApplication::activeWindow();
    auto* viewer = new ViewerPanel(ViewerPanel::Mode::Timeline, m_ctx);
    viewer->setObjectName(QStringLiteral("FullScreenViewer"));
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setWindowTitle(tr("Viewer"));
    viewer->setChromeVisible(false);
    if (m_ctx.actions) {
        // Shortcuts are window-scoped: make every command available in this window too.
        viewer->addActions(m_ctx.actions->actions());
    }
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), viewer);
    connect(escape, &QShortcut::activated, viewer, &QWidget::close);
    if (window && window->screen()) {
        viewer->setGeometry(window->screen()->geometry());
    }
    m_fullScreen = viewer;
    viewer->showFullScreen();
    viewer->activate();
    viewer->setFocus();
}

void ViewerTransport::onFocusChanged(QWidget*, QWidget* now)
{
    if (!now) {
        return;
    }
    if (ViewerPanel* v = viewerOf(now)) {
        v->activate();
        return;
    }
    if (tagged(now)) {
        for (ViewerPanel* v : ViewerPanel::allViewers()) {
            if (v->mode() == ViewerPanel::Mode::Timeline && v->isVisible()) {
                v->activate();
                return;
            }
        }
    }
}

int ViewerTransport::shuttleKey(const QKeyEvent* key) const
{
    if (!m_ctx.actions) {
        return -1;
    }
    const QKeySequence pressed(
        QKeyCombination(key->modifiers() & ~Qt::KeypadModifier, Qt::Key(key->key())));
    for (int i = 0; i < 3; ++i) {
        const QString id = QLatin1String(kShuttleIds[i]);
        if (!m_ctx.actions->contains(id)) {
            continue;
        }
        for (const QKeySequence& seq : m_ctx.actions->shortcuts(id)) {
            if (seq == pressed) {
                return i;
            }
        }
    }
    return -1;
}

bool ViewerTransport::eventFilter(QObject* watched, QEvent* event)
{
    // Cheap type test first: this filter sees every event of the application.
    const QEvent::Type type = event->type();
    if (type != QEvent::ShortcutOverride && type != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->isAutoRepeat()) {
        return QObject::eventFilter(watched, event);
    }
    const int which = shuttleKey(key);
    if (which < 0) {
        return QObject::eventFilter(watched, event);
    }
    const auto shuttle = ShuttleControl::Key(which);
    if (type == QEvent::ShortcutOverride) {
        // Pressed (the shortcut fires right after; the filter only observes).
        if (PlaybackEngine* e = targetEngine()) {
            if (m_heldEngine[which] != e) {
                e->setShuttleKeyHeld(shuttle, true);
                m_heldEngine[which] = e;
            }
        }
    } else if (PlaybackEngine* e = m_heldEngine[which]) {
        m_heldEngine[which] = nullptr;
        e->setShuttleKeyHeld(shuttle, false);
    }
    return QObject::eventFilter(watched, event);
}

} // namespace kk
