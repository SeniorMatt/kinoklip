// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel: assembles tool bar, controller and view; wires the zoom actions.
#include "ui/timeline/TimelinePanel.h"

#include "core/ClipColors.h"
#include "core/EditBehaviour.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/ChangeSpeedDialog.h"
#include "ui/timeline/ClipColorMenu.h"
#include "ui/timeline/StackedTimelinePane.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineDialogs.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineTabs.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDrag>
#include <QEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kZoomSliderSteps = 1000; ///< slider resolution over the whole zoom range
constexpr int kZoomSliderWidth = 140; ///< px, like Resolve's compact zoom slider
} // namespace

TimelinePanel::TimelinePanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Timeline"), parent)
    , m_ctx(ctx)
{
    setPanelInfo(this, tr("Timeline"),
        tr("The timeline with its tool bar: tracks, clips, editing tools, snapping and zoom."));
    setPanelKey(QStringLiteral("timeline")); // in-app help: panel:timeline
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_controller = new TimelineController(ctx, this);
    m_controller->connectActions();
    // The commands' questions are modal dialogs owned by this panel (tests set their own).
    m_controller->setHandlePrompt([this](int64_t missing, bool canShorten) {
        return timelinedialogs::askHandles(this, missing, canShorten);
    });
    m_controller->setMarkerEditor([this](Marker& marker, bool clipMarker) {
        return timelinedialogs::editMarker(
            this, marker, clipMarker, m_ctx.project->timeline()->data().fps);
    });
    m_controller->setPasteAttributesPrompt(
        [this] { return timelinedialogs::askPasteAttributes(this); });
    m_controller->setNamePrompt([this](const QString& title, const QString& label,
                                    const QString& suggestion, const QString& okText) {
        return timelinedialogs::askName(this, title, label, suggestion, okText);
    });
    m_controller->setNewTimelinePrompt(
        [this](const TimelineController::NewTimelineOptions& suggestion) {
            return timelinedialogs::askNewTimeline(this, suggestion, m_ctx.project->settings().fps);
        });
    m_controller->setConfirmPrompt([this](const QString& title, const QString& text) {
        return QMessageBox::question(this, title, text) == QMessageBox::Yes;
    });
    m_controller->setMessageSink([this](const QString& title, const QString& text) {
        QMessageBox::information(this, title, text);
    });
    m_controller->setTimelineSettingsPrompt(
        [this](quint64 uid) { timelinedialogs::editTimelineSettings(this, *m_ctx.project, uid); });

    m_toolBar = new QToolBar(body);
    m_toolBar->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Resolve's timeline tool bar order: edit tools | source edit modes | snapping and
    // linking | flags and markers | (right) zoom. Data, not code: the groups are action ids.
    const char* const groups[][4] = {
        {"tool.select", "tool.trim", "trim.dynamic", "tool.blade"},
        {"edit.insert", "edit.overwrite", "edit.place_on_top", "edit.replace"},
        {"timeline.snapping", "timeline.linked_selection", nullptr, nullptr},
        {"edit.split", "clip.flag", "mark.add_marker", nullptr},
    };
    m_toolBar->addWidget(makeViewOptionsButton());
    m_toolBar->addSeparator();
    for (const auto& group : groups) {
        if (!m_toolBar->actions().isEmpty())
            m_toolBar->addSeparator();
        for (const char* id : group) {
            if (id && ctx.actions && ctx.actions->contains(QLatin1String(id)))
                m_toolBar->addAction(ctx.actions->action(QLatin1String(id)));
        }
    }
    addMarkerColorMenu();
    auto* spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);
    if (ctx.actions) {
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_fit")));
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_out")));
    }
    m_zoomSlider = new QSlider(Qt::Horizontal, m_toolBar);
    m_zoomSlider->setObjectName(QStringLiteral("TimelineZoomSlider"));
    m_zoomSlider->setRange(0, kZoomSliderSteps);
    m_zoomSlider->setFixedWidth(kZoomSliderWidth);
    m_zoomSlider->setFocusPolicy(Qt::NoFocus);
    m_zoomSlider->setToolTip(tr("Timeline zoom"));
    m_toolBar->addWidget(m_zoomSlider);
    if (ctx.actions)
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_in")));
    layout->addWidget(m_toolBar);

    // Stacked timelines: two slots in a vertical splitter; the editor group (tabs + view)
    // sits in one, the inactive pane (created on first use) in the other.
    m_stack = new QSplitter(Qt::Vertical, body);
    m_stack->setObjectName(QStringLiteral("StackedTimelines"));
    m_stack->setChildrenCollapsible(false);
    for (QWidget*& slot : m_slots) {
        slot = new QWidget(m_stack);
        auto* l = new QVBoxLayout(slot);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        m_stack->addWidget(slot);
    }
    m_slots[0]->hide();
    layout->addWidget(m_stack, 1);
    m_editorGroup = new QWidget(m_slots[1]);
    auto* editorLayout = new QVBoxLayout(m_editorGroup);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    m_slots[1]->layout()->addWidget(m_editorGroup);

    // Open timelines (v2: tabs by default, Preferences > Editing > Timelines).
    m_tabs = new TimelineTabs(ctx, m_editorGroup);
    editorLayout->addWidget(m_tabs);
    connect(m_tabs, &TimelineTabs::renameRequested, this, [this](quint64 uid) {
        const auto data = m_ctx.project->timelineData(uid);
        if (!data)
            return;
        const std::optional<QString> name = timelinedialogs::askName(
            this, tr("Rename Timeline"), tr("Timeline name"), data->name, tr("Rename"));
        if (name && !name->trimmed().isEmpty())
            m_ctx.project->renameTimeline(uid, name->trimmed());
    });
    connect(m_tabs, &TimelineTabs::settingsRequested, this,
        [this](quint64 uid) { timelinedialogs::editTimelineSettings(this, *m_ctx.project, uid); });
    applyPreferences();
    // Switching timelines stops playback (the transport belongs to the timeline shown).
    connect(ctx.project, &Project::activeTimelineAboutToChange, this, [this] {
        if (m_ctx.playback)
            m_ctx.playback->pause();
    });

    m_view = new TimelineWidget(ctx, m_controller, m_editorGroup);
    m_view->setClipThumbnails(m_viewThumbnails);
    m_view->setShowWaveforms(m_viewWaveforms);
    m_view->setShowSourceTimecode(m_viewSourceTimecode);
    editorLayout->addWidget(m_view, 1);
    setContentWidget(body);
    // Resolve has no title above its timeline: the name is in the timeline viewer's header
    // and the tool bar starts right under the viewers.
    setTitleBarVisible(false);

    // Zoom slider <-> view. The slider is logarithmic over the view's zoom limits. The view
    // has no zoom signal (its zoom logic belongs to the timeline module): every repaint of
    // the viewport re-syncs the slider, which is cheap (one division and a log).
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_syncingSlider)
            return;
        const double t = double(value) / kZoomSliderSteps;
        const double ppf = timelinemetrics::kMinPixelsPerFrame
            * std::pow(
                timelinemetrics::kMaxPixelsPerFrame / timelinemetrics::kMinPixelsPerFrame, t);
        // Resolve zooms around the playhead.
        const double anchor
            = m_view->layout().frameToX(double(m_ctx.project->timeline()->playhead()));
        m_view->setPixelsPerFrame(ppf, anchor);
    });
    m_view->viewport()->installEventFilter(this);
    syncZoomSlider();

    if (ctx.actions) {
        auto on = [this](const char* id, void (TimelineWidget::*slot)()) {
            // Another page's timeline (the Cut page) zooms itself while this one is hidden.
            connect(m_ctx.actions->action(QLatin1String(id)), &QAction::triggered, m_view,
                [this, slot] {
                    if (m_view->isVisible() || !window()->isVisible())
                        (m_view->*slot)();
                });
        };
        on("timeline.zoom_in", &TimelineWidget::zoomIn);
        on("timeline.zoom_out", &TimelineWidget::zoomOut);
        on("timeline.zoom_fit", &TimelineWidget::toggleZoomToFit);
        connect(m_ctx.actions->action(QStringLiteral("clip.change_duration")), &QAction::triggered,
            this, &TimelinePanel::askClipDuration);
        connect(m_ctx.actions->action(QStringLiteral("clip.change_speed")), &QAction::triggered,
            this, &TimelinePanel::askClipSpeed);
        if (m_ctx.actions->contains(QStringLiteral("view.stacked_timelines"))) {
            QAction* stacked = m_ctx.actions->action(QStringLiteral("view.stacked_timelines"));
            stacked->setChecked(m_stacked);
            connect(stacked, &QAction::toggled, this, &TimelinePanel::setStackedTimelines);
        }
    }
    // Stacked: the pane never shows the active timeline (switching tabs in the editor to the
    // timeline the pane shows puts the previous one into the pane).
    connect(ctx.project, &Project::activeTimelineAboutToChange, this,
        [this](quint64 uid) { m_previousActive = uid; });
    connect(ctx.project, &Project::activeTimelineChanged, this, [this](quint64 uid) {
        if (m_stacked && m_pane && !m_swapping && m_pane->currentTimeline() == uid
            && m_ctx.project->hasTimeline(m_previousActive))
            m_pane->showTimeline(m_previousActive);
    });

    // Title = timeline name (the program viewer is already called "Timeline").
    Timeline* timeline = ctx.project->timeline();
    auto updateTitle = [this, timeline] {
        const QString name = timeline->data().name;
        setTitle(name.isEmpty() ? tr("Timeline") : name);
    };
    connect(timeline, &Timeline::changed, this, updateTitle);
    updateTitle();
}

void TimelinePanel::applyPreferences()
{
    m_tabs->setTabsMode(EditBehaviour::fromSettings().timelineTabs);
}

void TimelinePanel::setStackedTimelines(bool on)
{
    if (on == m_stacked)
        return;
    m_stacked = on;
    if (on && !m_pane) {
        m_pane = new StackedTimelinePane(m_ctx, m_slots[0]);
        connect(m_pane, &StackedTimelinePane::activateRequested, this,
            &TimelinePanel::activateStackedPane);
        connect(m_pane, &StackedTimelinePane::clipsDropped, this,
            [this](quint64 source, const QVector<quint64>& clips, int64_t frame, TrackRef track,
                bool insert) {
                // Dropped on the inactive timeline: it becomes the active one, then the copy
                // is an ordinary edit there (one undo step).
                if (activateStackedPane())
                    m_controller->copyClipsFrom(source, clips, frame, track, insert);
            });
    }
    if (on) {
        // The pane starts with another regular timeline (the first one that is not active).
        const quint64 active = m_ctx.project->activeTimelineUid();
        if (!m_ctx.project->hasTimeline(m_pane->currentTimeline())
            || m_pane->currentTimeline() == active) {
            quint64 other = 0;
            for (const quint64 uid : m_ctx.project->timelineUids()) {
                const auto data = m_ctx.project->timelineData(uid);
                if (uid != active && data && !data->compound) {
                    other = uid;
                    break;
                }
            }
            m_pane->setTabs(other ? QVector<quint64> {other} : QVector<quint64> {}, other);
        }
        m_view->setClipDragOut(
            [this](const QVector<quint64>& clips, int64_t grab) { dragClipsOut(clips, grab); });
    } else {
        m_view->setClipDragOut({});
        if (m_pane)
            m_pane->view()->storeViewState();
    }
    placeStackedPanes();
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.stacked_timelines"))) {
        QAction* a = m_ctx.actions->action(QStringLiteral("view.stacked_timelines"));
        if (a->isChecked() != on)
            a->setChecked(on);
    }
}

void TimelinePanel::placeStackedPanes()
{
    const int editor = m_stacked ? m_activeSlot : 1;
    // Reparenting hides a widget until the layout's queued show: show them right away so
    // the swap is complete when this returns.
    m_slots[editor]->layout()->addWidget(m_editorGroup);
    m_editorGroup->show();
    if (m_pane)
        m_slots[1 - editor]->layout()->addWidget(m_pane);
    m_slots[0]->setVisible(m_stacked || editor == 0);
    m_slots[1]->setVisible(m_stacked || editor == 1);
    if (m_pane)
        m_pane->setVisible(m_stacked);
    if (m_stacked)
        m_stack->setSizes({1, 1}); // two equal halves
}

bool TimelinePanel::activateStackedPane()
{
    if (!m_stacked || !m_pane)
        return false;
    Project* project = m_ctx.project;
    const quint64 uid = m_pane->currentTimeline();
    const quint64 old = project->activeTimelineUid();
    if (!uid || uid == old || project->isEditing())
        return false;
    // Each pane keeps its own tabs, zoom and scroll: the pane's become the editor's (the
    // project's) and the other way round.
    m_pane->view()->storeViewState();
    const QVector<quint64> paneTabs = m_pane->tabs();
    const QVector<quint64> editorTabs = project->tabs();
    m_swapping = true;
    const bool ok = project->setActiveTimeline(uid);
    if (ok) {
        project->setTabs(paneTabs);
        m_pane->setTabs(editorTabs, old);
        m_activeSlot = 1 - m_activeSlot; // the editor moves to where the user clicked
        placeStackedPanes();
        m_view->setFocus(Qt::MouseFocusReason);
    }
    m_swapping = false;
    return ok;
}

void TimelinePanel::dragClipsOut(const QVector<quint64>& clips, int64_t grabFrames)
{
    QJsonArray ids;
    for (const quint64 id : clips)
        ids.append(qint64(id));
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(mime::TimelineClips),
        QJsonDocument(
            QJsonObject {{QLatin1String("timeline"), qint64(m_ctx.project->activeTimelineUid())},
                {QLatin1String("clips"), ids}, {QLatin1String("grabFrames"), qint64(grabFrames)}})
            .toJson(QJsonDocument::Compact));
    auto* drag = new QDrag(m_view);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void TimelinePanel::askClipDuration()
{
    const QVector<quint64> ids = m_controller->effectiveSelection();
    if (ids.isEmpty() || !isVisible())
        return;
    const TimelineData& d = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(d, ids.front());
    if (!clip)
        return;
    bool ok = false;
    const QString text
        = QInputDialog::getText(this, tr("Change Clip Duration"), tr("Duration (hh:mm:ss:ff):"),
            QLineEdit::Normal, timecode::format(clip->duration, d.fps), &ok);
    if (!ok)
        return;
    const auto frames = timecode::parse(text, d.fps);
    if (frames && *frames > 0)
        m_controller->setSelectionDuration(*frames);
}

void TimelinePanel::askClipSpeed()
{
    const QVector<quint64> ids = m_controller->retimeTargets();
    if (ids.isEmpty() || !isVisible())
        return;
    const TimelineData& d = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(d, ids.front());
    if (!clip)
        return;
    Rational sourceFps;
    if (const MediaItem* item = m_ctx.project->mediaPool()->item(clip->mediaId))
        sourceFps = item->info.frameRate;
    ChangeSpeedDialog dialog(*clip, d.fps, sourceFps, this);
    if (dialog.exec() == QDialog::Accepted)
        m_controller->changeSelectionSpeed(dialog.result());
}

void TimelinePanel::syncZoomSlider()
{
    const double range
        = std::log(timelinemetrics::kMaxPixelsPerFrame / timelinemetrics::kMinPixelsPerFrame);
    const double t
        = std::log(m_view->pixelsPerFrame() / timelinemetrics::kMinPixelsPerFrame) / range;
    const int value = int(std::lround(std::clamp(t, 0.0, 1.0) * kZoomSliderSteps));
    if (m_zoomSlider->value() == value)
        return;
    m_syncingSlider = true;
    m_zoomSlider->setValue(value);
    m_syncingSlider = false;
}

bool TimelinePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Paint && watched == m_view->viewport())
        syncZoomSlider();
    return PanelFrame::eventFilter(watched, event);
}

void TimelinePanel::addMarkerColorMenu()
{
    // Resolve's flag and marker buttons have a drop-down of their colours: the one picked is
    // what the button (and M, Ctrl+M for markers) uses from then on (spec 18.17, 33.36).
    addColorMenu("mark.add_marker", markerColorPalette(), prefs::kNewMarkerColor,
        QStringLiteral("MarkerColorMenu"));
    addColorMenu("clip.flag", flagPalette(), prefs::kToolBarFlag, QStringLiteral("FlagColorMenu"));
}

void TimelinePanel::addColorMenu(const char* actionId, std::span<const PaletteColor> palette,
    const char* settingsKey, const QString& objectName)
{
    QAction* action = m_ctx.actions && m_ctx.actions->contains(QLatin1String(actionId))
        ? m_ctx.actions->action(QLatin1String(actionId))
        : nullptr;
    auto* button
        = action ? qobject_cast<QToolButton*>(m_toolBar->widgetForAction(action)) : nullptr;
    if (!button)
        return;
    auto* menu = new QMenu(button);
    menu->setObjectName(objectName);
    auto* group = new QActionGroup(menu);
    for (const PaletteColor& c : palette) {
        QPixmap swatch(12, 12);
        swatch.fill(QColor(c.rgb));
        QAction* a
            = menu->addAction(QIcon(swatch), QCoreApplication::translate("kk::ClipColors", c.name));
        a->setCheckable(true);
        a->setData(QString::fromLatin1(c.id));
        group->addAction(a);
        connect(a, &QAction::triggered, this,
            [key = QString::fromLatin1(settingsKey), id = QString::fromLatin1(c.id)] {
                QSettings().setValue(key, id);
            });
    }
    const QString fallback = QString::fromLatin1(palette.front().id);
    connect(
        menu, &QMenu::aboutToShow, menu, [menu, key = QString::fromLatin1(settingsKey), fallback] {
            const QString current = QSettings().value(key, fallback).toString();
            bool found = false;
            for (QAction* a : menu->actions())
                found |= a->data().toString() == current;
            for (QAction* a : menu->actions())
                a->setChecked(a->data().toString() == (found ? current : fallback));
        });
    button->setMenu(menu);
    button->setPopupMode(QToolButton::MenuButtonPopup);
}

QWidget* TimelinePanel::makeViewOptionsButton()
{
    // Resolve's Timeline View Options sit at the left end of the timeline tool bar.
    auto* button = new QToolButton(m_toolBar);
    button->setObjectName(QStringLiteral("TimelineViewOptions"));
    button->setToolTip(tr("Timeline View Options"));
    button->setFocusPolicy(Qt::NoFocus);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setIcon(paletteIcon(QStringLiteral("timeline-view-options"),
        [](QPainter& p, const QSizeF& size, const QColor& color) {
            // Three track rows, the middle one with two clips.
            const double w = size.width();
            const double h = size.height() / 7.0;
            p.fillRect(QRectF(w * 0.1, h * 1, w * 0.8, h), color);
            p.fillRect(QRectF(w * 0.1, h * 3, w * 0.35, h), color);
            p.fillRect(QRectF(w * 0.55, h * 3, w * 0.35, h), color);
            p.fillRect(QRectF(w * 0.1, h * 5, w * 0.8, h), color);
        }));
    auto* menu = new QMenu(button);
    menu->setObjectName(QStringLiteral("TimelineViewOptionsMenu"));
    button->setMenu(menu);
    // Rebuilt on every show so the checks reflect the view (and the current timeline).
    connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillViewOptionsMenu(*menu); });
    // Restore the stored choices.
    const QSettings settings;
    const QString mode = settings.value(QLatin1String(prefs::kTimelineThumbnails)).toString();
    m_viewThumbnails = mode == QLatin1String("headtail") ? ClipThumbnails::HeadTail
        : mode == QLatin1String("none")                  ? ClipThumbnails::None
                                                         : ClipThumbnails::Filmstrip;
    m_viewWaveforms = settings.value(QLatin1String(prefs::kTimelineWaveforms), true).toBool();
    m_viewSourceTimecode
        = settings.value(QLatin1String(prefs::kTimelineSourceTimecode), false).toBool();
    return button;
}

void TimelinePanel::fillViewOptionsMenu(QMenu& menu)
{
    menu.clear();
    menu.addSection(tr("Video Clips"));
    const std::pair<ClipThumbnails, QString> modes[] = {
        {ClipThumbnails::Filmstrip, tr("Filmstrip")},
        {ClipThumbnails::HeadTail, tr("Head and Tail Frames")},
        {ClipThumbnails::None, tr("Clip Name Only")},
    };
    auto* group = new QActionGroup(&menu);
    for (const auto& [mode, text] : modes) {
        QAction* a = menu.addAction(text, this, [this, m = mode] { setClipThumbnails(m); });
        a->setCheckable(true);
        a->setChecked(m_view->clipThumbnails() == mode);
        group->addAction(a);
    }
    menu.addSection(tr("Audio Clips"));
    QAction* wave
        = menu.addAction(tr("Show Waveforms"), this, [this](bool on) { setShowWaveforms(on); });
    wave->setCheckable(true);
    wave->setChecked(m_view->showWaveforms());
    menu.addSection(tr("Clips"));
    QAction* tc = menu.addAction(
        tr("Show Source Timecode"), this, [this](bool on) { setShowSourceTimecode(on); });
    tc->setCheckable(true);
    tc->setChecked(m_view->showSourceTimecode());
    menu.addSeparator();
    m_view->addTrackHeightMenu(menu, TrackRef {TrackType::Video, 0}, true);
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.stacked_timelines")))
        menu.addAction(m_ctx.actions->action(QStringLiteral("view.stacked_timelines")));
}

void TimelinePanel::setClipThumbnails(ClipThumbnails mode)
{
    m_viewThumbnails = mode;
    m_view->setClipThumbnails(mode);
    const char* id = mode == ClipThumbnails::HeadTail ? "headtail"
        : mode == ClipThumbnails::None                ? "none"
                                                      : "filmstrip";
    QSettings().setValue(QLatin1String(prefs::kTimelineThumbnails), QLatin1String(id));
}

void TimelinePanel::setShowSourceTimecode(bool on)
{
    m_viewSourceTimecode = on;
    m_view->setShowSourceTimecode(on);
    QSettings().setValue(QLatin1String(prefs::kTimelineSourceTimecode), on);
}

void TimelinePanel::setShowWaveforms(bool on)
{
    m_viewWaveforms = on;
    m_view->setShowWaveforms(on);
    QSettings().setValue(QLatin1String(prefs::kTimelineWaveforms), on);
}

} // namespace kk
