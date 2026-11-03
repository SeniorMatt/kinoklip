// SPDX-License-Identifier: GPL-3.0-or-later
// ColorPage layout and wiring of the color.* commands.
#include "app/pages/ColorPage.h"

#include "app/pages/CompositingPage.h"
#include "core/GradeGallery.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorWidgets.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelSplitter.h"
#include "ui/scopes/ScopeFeeder.h"
#include "ui/scopes/ScopeView.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QShowEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {

/// Layout version of the saved splitter states (bump when the structure changes).
constexpr int kLayoutVersion = 1;
/// Gallery thumbnail size.
constexpr QSize kStillIcon(96, 54);

QAction* registryAction(const AppContext& ctx, const QString& id)
{
    return ctx.actions && ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
}

} // namespace

ColorPage::ColorPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("ColorPage"));
    m_session = new ColorSession(ctx, this);

    // ---- Top: viewer | scopes ----
    auto* viewerArea = new QWidget;
    auto* va = new QVBoxLayout(viewerArea);
    va->setContentsMargins(0, 0, 0, 0);
    va->setSpacing(0);
    m_header = new QWidget;
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(6, 2, 6, 2);
    auto* title = new QLabel;
    title->setObjectName(QStringLiteral("ColorViewerTitle"));
    m_targetButton = new QToolButton;
    m_targetButton->setObjectName(QStringLiteral("ColorTargetButton"));
    m_targetButton->setCheckable(true);
    m_targetButton->setText(tr("Timeline Grade"));
    m_targetButton->setToolTip(tr("Grade the whole timeline (applied after the clip grades)"));
    m_bypassButton = new QToolButton;
    m_bypassButton->setObjectName(QStringLiteral("ColorBypassButton"));
    m_bypassButton->setCheckable(true);
    m_bypassButton->setText(tr("Bypass"));
    hl->addWidget(title, 1);
    hl->addWidget(m_targetButton);
    hl->addWidget(m_bypassButton);
    va->addWidget(m_header);
    m_surface = ViewerSurface::create(viewerArea);
    va->addWidget(m_surface->widget(), 1);
    const TimelineData& data = ctx.project->timeline()->data();
    m_surface->setAspect(QSize(data.width, data.height));

    m_scopes = new ScopesPanel(QStringLiteral("colorPage/scopeType"));
    m_feeder = new ScopeFeeder(ctx.playback, this);
    connect(m_feeder, &ScopeFeeder::imageReady, m_scopes, &ScopesPanel::setImage);

    m_top = new PanelSplitter(Qt::Horizontal);
    m_top->setObjectName(QStringLiteral("ColorTopSplitter"));
    m_top->addWidget(viewerArea);
    m_top->addWidget(m_scopes);
    m_top->setStretchFactor(0, 3);
    m_top->setStretchFactor(1, 1);

    // ---- Bottom: nodes | primaries | curves | gallery ----
    m_nodes = new NodeList(m_session);
    m_primaries = new PrimariesPanel(m_session);
    m_curves = new GradeCurveEditor(m_session);
    m_gallery = new QListWidget;
    m_gallery->setObjectName(QStringLiteral("ColorGallery"));
    m_gallery->setViewMode(QListView::IconMode);
    m_gallery->setIconSize(kStillIcon);
    m_gallery->setResizeMode(QListView::Adjust);
    m_gallery->setToolTip(tr("Gallery: double-click a still to apply its grade to the clip"));
    m_bottom = new PanelSplitter(Qt::Horizontal);
    m_bottom->setObjectName(QStringLiteral("ColorBottomSplitter"));
    m_bottom->addWidget(m_nodes);
    m_bottom->addWidget(m_primaries);
    m_bottom->addWidget(m_curves);
    m_bottom->addWidget(m_gallery);
    m_bottom->setStretchFactor(1, 3);
    m_bottom->setStretchFactor(2, 2);

    m_rows = new PanelSplitter(Qt::Vertical);
    m_rows->setObjectName(QStringLiteral("ColorRowsSplitter"));
    m_rows->addWidget(m_top);
    m_rows->addWidget(m_bottom);
    m_rows->setStretchFactor(0, 3);
    m_rows->setStretchFactor(1, 2);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_rows);

    // ---- Wiring ----
    if (ctx.playback) {
        connect(ctx.playback, &PlaybackEngine::frameReady, this, [this](const RenderedFramePtr& f) {
            if (isVisible() && f)
                m_surface->setRenderedFrame(f);
        });
    }
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] {
        const TimelineData& d = m_ctx.project->timeline()->data();
        m_surface->setAspect(QSize(d.width, d.height));
    });
    connect(m_session, &ColorSession::clipChanged, this, &ColorPage::updateHeader);
    connect(m_session, &ColorSession::gradeChanged, this, &ColorPage::updateHeader);
    connect(m_session, &ColorSession::bypassAllChanged, this, &ColorPage::updateHeader);
    connect(m_targetButton, &QToolButton::toggled, this, [this](bool timeline) {
        m_session->setTarget(
            timeline ? ColorSession::Target::Timeline : ColorSession::Target::Clip);
    });
    connect(m_bypassButton, &QToolButton::toggled, m_session, &ColorSession::setBypassAll);
    connect(ctx.project->gallery(), &GradeGallery::changed, this, &ColorPage::refreshGallery);
    connect(m_gallery, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        m_session->applyStill(item->data(Qt::UserRole).toULongLong());
    });
    connectActions();
    refreshGallery();
    updateHeader();
}

ColorPage::~ColorPage()
{
    if (m_filterInstalled)
        qApp->removeEventFilter(this);
    if (m_ctx.playback)
        m_ctx.playback->removeOutputClient(this);
}

QStringList ColorPage::commandIds(const ActionRegistry& registry)
{
    QStringList ids;
    for (const QString& id : registry.menuEntries(QStringLiteral("color"))) {
        if (registry.contains(id))
            ids.append(id);
    }
    return ids;
}

void ColorPage::connectActions()
{
    auto on = [this](const char* id, auto&& fn) {
        if (QAction* a = registryAction(m_ctx, QLatin1String(id)))
            connect(a, &QAction::triggered, this, fn);
    };
    on("color.add_serial", [this] { m_session->addSerialNode(); });
    on("color.add_parallel", [this] { m_session->addParallelNode(); });
    on("color.add_layer", [this] { m_session->addLayerNode(); });
    on("color.delete_node", [this] { m_session->removeNode(); });
    on("color.bypass_node", [this] { m_session->toggleNodeBypass(); });
    on("color.reset_node", [this] { m_session->resetNode(); });
    on("color.reset_grade", [this] { m_session->resetGrade(); });
    on("color.grab_still", [this] { m_session->grabStill(); });
    on("color.apply_still", [this] {
        if (QListWidgetItem* item = m_gallery->currentItem())
            m_session->applyStill(item->data(Qt::UserRole).toULongLong());
    });
    if (QAction* a = registryAction(m_ctx, QStringLiteral("color.bypass_all"))) {
        connect(a, &QAction::toggled, m_session, &ColorSession::setBypassAll);
        connect(m_session, &ColorSession::bypassAllChanged, a, [a](bool on) {
            const QSignalBlocker block(a);
            a->setChecked(on);
        });
    }
    if (QAction* a = registryAction(m_ctx, QStringLiteral("color.timeline_grade"))) {
        connect(a, &QAction::toggled, m_targetButton, &QToolButton::setChecked);
        connect(m_targetButton, &QToolButton::toggled, a, [a](bool on) {
            const QSignalBlocker block(a);
            a->setChecked(on);
        });
    }
    // Disabled until the page is shown (the Color menu is in the Edit menu of every page).
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(false);
    }
}

void ColorPage::refreshGallery()
{
    m_gallery->clear();
    for (const GalleryStill& s : m_ctx.project->gallery()->stills()) {
        auto* item = new QListWidgetItem(s.name, m_gallery);
        item->setData(Qt::UserRole, s.id);
        if (!s.image.isNull())
            item->setIcon(QIcon(QPixmap::fromImage(
                s.image.scaled(kStillIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    }
}

void ColorPage::updateHeader()
{
    auto* title = m_header->findChild<QLabel*>(QStringLiteral("ColorViewerTitle"));
    QString text;
    if (m_session->target() == ColorSession::Target::Timeline) {
        text = tr("Timeline grade");
    } else if (const quint64 id = m_session->clipId()) {
        const ClipData* c = nullptr;
        for (const TrackData& t : m_ctx.project->timeline()->data().videoTracks) {
            const int i = t.indexOfClip(id);
            if (i >= 0)
                c = &t.clips[size_t(i)];
        }
        text = c && !c->name.isEmpty() ? c->name : tr("Clip");
        if (m_session->grade().isEmpty())
            text += QStringLiteral(" — ") + tr("ungraded");
    } else {
        text = tr("No clip under the playhead");
    }
    if (m_session->bypassAll())
        text += QStringLiteral(" — ") + tr("grades bypassed");
    title->setText(text);
    const QSignalBlocker block(m_bypassButton);
    m_bypassButton->setChecked(m_session->bypassAll());
}

QAction* ColorPage::actionForKey(const QKeyEvent* event) const
{
    if (!m_ctx.actions)
        return nullptr;
    const QKeySequence seq(event->keyCombination());
    for (const QString& id : commandIds(*m_ctx.actions)) {
        QAction* a = m_ctx.actions->action(id);
        if (!a->isEnabled())
            continue;
        for (const QKeySequence& k : a->shortcuts()) {
            if (k == seq)
                return a;
        }
    }
    return nullptr;
}

bool ColorPage::eventFilter(QObject* watched, QEvent* event)
{
    // The page claims the keys of its commands while focus is on it (scope widget:color_page):
    // ShortcutOverride keeps the window shortcuts from taking them, the key press triggers.
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        if (w && isVisible() && (w == this || isAncestorOf(w))) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (QAction* a = actionForKey(key)) {
                if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
                    a->trigger();
                event->accept();
                return true;
            }
        }
    }
    return Page::eventFilter(watched, event);
}

void ColorPage::suspendTimelineCommands(bool suspend)
{
    if (!m_ctx.actions)
        return;
    if (!suspend) {
        for (const QPointer<QAction>& a : std::as_const(m_suspended)) {
            if (a)
                a->setEnabled(true);
        }
        m_suspended.clear();
        return;
    }
    for (const QString& id : CompositingPage::suspendedCommandIds(*m_ctx.actions)) {
        QAction* a = m_ctx.actions->action(id);
        if (a->isEnabled()) {
            a->setEnabled(false);
            m_suspended.append(a);
        }
    }
}

void ColorPage::updateOutputClient()
{
    if (!m_ctx.playback)
        return;
    if (!isVisible()) {
        m_ctx.playback->removeOutputClient(this);
        return;
    }
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    m_ctx.playback->setOutputClient(this,
        QSize(int(logical.width() * dpr), int(logical.height() * dpr)),
        m_surface->canDrawTextures());
}

void ColorPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (event->spontaneous())
        return;
    suspendTimelineCommands(true);
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(true);
    }
    if (!m_filterInstalled) {
        qApp->installEventFilter(this);
        m_filterInstalled = true;
    }
    m_session->openClip(0);
    updateOutputClient();
    if (m_ctx.playback && m_ctx.playback->currentFrame())
        m_surface->setRenderedFrame(m_ctx.playback->currentFrame());
    m_feeder->setActive(true);
    updateHeader();
}

void ColorPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    if (event->spontaneous())
        return; // minimised: keep the state
    suspendTimelineCommands(false);
    if (m_ctx.actions) {
        for (const QString& id : commandIds(*m_ctx.actions))
            m_ctx.actions->action(id)->setEnabled(false);
    }
    if (m_filterInstalled) {
        qApp->removeEventFilter(this);
        m_filterInstalled = false;
    }
    m_feeder->setActive(false);
    if (m_ctx.playback)
        m_ctx.playback->removeOutputClient(this);
}

void ColorPage::resizeEvent(QResizeEvent* event)
{
    Page::resizeEvent(event);
    updateOutputClient();
}

void ColorPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    settings.setValue(QStringLiteral("rows"), m_rows->saveState());
    settings.setValue(QStringLiteral("top"), m_top->saveState());
    settings.setValue(QStringLiteral("bottom"), m_bottom->saveState());
}

void ColorPage::restoreLayout(const QSettings& settings)
{
    if (settings.value(QStringLiteral("layoutVersion"), 0).toInt() != kLayoutVersion)
        return;
    m_rows->restoreState(settings.value(QStringLiteral("rows")).toByteArray());
    m_top->restoreState(settings.value(QStringLiteral("top")).toByteArray());
    m_bottom->restoreState(settings.value(QStringLiteral("bottom")).toByteArray());
}

} // namespace kk
