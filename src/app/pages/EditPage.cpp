// SPDX-License-Identifier: GPL-3.0-or-later
// EditPage layout and panel visibility handling.
#include "app/pages/EditPage.h"

#include "app/AppInfo.h"
#include "core/EditOps.h"
#include "core/MediaUsage.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "playback/SourcePlayer.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/MixerPanel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/PanelSplitter.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MetadataPanel.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QCloseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <memory>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window; applyDefaultProportions() scales them to the real size
// whenever the page is resized until the user drags a splitter (QSplitter alone would give the
// whole difference to the stretching viewers, leaving the side columns narrow when the window
// starts small and is then maximised).
constexpr int kLeftColumnWidth = 460; ///< media pool / effects / edit index column
constexpr int kViewerWidth = 490;
constexpr int kRightColumnWidth = 480; ///< inspector / metadata column
/// Version of the stored splitter layout. Layouts stored by older versions are ignored for the
/// top row once (version 1 could store side columns squeezed by the resize problem above).
constexpr int kLayoutVersion = 3; ///< 3: Keyframes docks in the left column
constexpr int kTopRowHeight = 520;
constexpr int kBottomRowHeight = 440;
constexpr int kMixerWidth = 260;
constexpr int kKeyframesMinDockHeight = 160; ///< below this a shown docked panel is resized
constexpr double kKeyframesDockShare = 0.55; ///< of the left column when it opens docked
constexpr int kMinColumnPanelHeight = 120; ///< other left-column panels keep at least this
constexpr int kKeyframesMinColumnWidth = 640; ///< left column width (of 1920) while it is docked
const QSize kKeyframesWindowSize(1400, 520); ///< first floating window size
constexpr int kTearOffGrabY = 12; ///< px below the window top where the pointer holds it
constexpr int kTearOffMoveRetryMs = 15; ///< wait for the torn-off window to be mapped ...
constexpr int kTearOffMoveAttempts = 40; ///< ... at most this often (600 ms)
constexpr int kReferenceWidth = kLeftColumnWidth + 2 * kViewerWidth + kRightColumnWidth;

QSplitter* makeSplitter(Qt::Orientation orientation)
{
    return new PanelSplitter(orientation); // hover-highlighted handles, no collapsing
}

} // namespace

EditPage::EditPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_mediaPool = new MediaPoolPanel(ctx);
    m_effects = new EffectsLibraryPanel(ctx);
    m_editIndex = new EditIndexPanel(ctx);
    m_sourceViewer = new ViewerPanel(ViewerPanel::Mode::Source, ctx);
    m_timelineViewer = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_inspector = new InspectorPanel(ctx);
    m_metadata = new MetadataPanel(ctx);
    m_timeline = new TimelinePanel(ctx);
    // The Edit page's Mixer panel: the Audio page's mixer strips (own controller and meter
    // ballistics; automation Write passes run only from the Audio page).
    auto* mixerController = new MixerController(ctx, this);
    mixerController->setWritePassesEnabled(false);
    auto* meterModel = new MeterModel(this);
    meterModel->setEngine(ctx.playback);
    m_mixer = new MixerPanel(ctx, mixerController, meterModel);
    m_keyframes = new KeyframesPanel(ctx);

    m_leftColumn = makeSplitter(Qt::Vertical);
    m_leftColumn->addWidget(m_mediaPool);
    m_leftColumn->addWidget(m_effects);
    m_leftColumn->addWidget(m_editIndex);
    // Keyframes: its toggle sits on the left of the interface toolbar like Media Pool, Effects
    // and Edit Index, and it docks in their column (Resolve 20); it can also float.
    m_leftColumn->addWidget(m_keyframes);

    m_rightColumn = makeSplitter(Qt::Vertical);
    m_rightColumn->addWidget(m_inspector);
    m_rightColumn->addWidget(m_metadata);

    m_topRow = makeSplitter(Qt::Horizontal);
    m_topRow->addWidget(m_leftColumn);
    m_topRow->addWidget(m_sourceViewer);
    m_topRow->addWidget(m_timelineViewer);
    m_topRow->addWidget(m_rightColumn);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setStretchFactor(2, 1);
    m_topRow->setSizes({kLeftColumnWidth, kViewerWidth, kViewerWidth, kRightColumnWidth});
    // A drag by the user ends the automatic default widths (eventFilter()).
    connect(m_topRow, &QSplitter::splitterMoved, this, [this] { m_defaultWidths = false; });
    m_topRow->installEventFilter(this);

    m_bottomRow = makeSplitter(Qt::Horizontal);
    m_bottomRow->addWidget(m_timeline);
    m_bottomRow->addWidget(m_mixer);
    m_bottomRow->setStretchFactor(0, 1);
    m_bottomRow->setSizes({1920 - kMixerWidth, kMixerWidth});

    m_main = makeSplitter(Qt::Vertical);
    m_main->addWidget(m_topRow);
    m_main->addWidget(m_bottomRow);
    m_main->setStretchFactor(1, 1);
    m_main->setSizes({kTopRowHeight, kBottomRowHeight});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    m_panels = {
        {PanelId::MediaPool, m_mediaPool},
        {PanelId::Effects, m_effects},
        {PanelId::EditIndex, m_editIndex},
        {PanelId::Mixer, m_mixer},
        {PanelId::Metadata, m_metadata},
        {PanelId::Inspector, m_inspector},
        {PanelId::Keyframes, m_keyframes},
    };
    // Default visibility (first start): media pool + inspector, like PLAN §4.
    for (PanelId id : {PanelId::Effects, PanelId::EditIndex, PanelId::Mixer, PanelId::Metadata,
             PanelId::Keyframes})
        m_panels.value(id)->hide();
    updateColumns();

    // The metadata panel follows the media pool selection (one item or batch editing).
    m_metadata->attach(m_mediaPool);
    // Double-click in the media pool: load the clip into the source viewer and make it the
    // target of the transport keys (Space, J/K/L, I/O act on it).
    connect(m_mediaPool, &MediaPoolPanel::openInSourceViewer, this, [this](const QString& id) {
        m_sourceViewer->setSourceMedia(id);
        showSourceInSingleViewer(true); // single mode: the viewer switches to the clip
        m_sourceViewer->activate();
        m_sourceViewer->setFocus();
    });
    // A timeline opened from the media pool shows in the timeline viewer (Resolve).
    connect(m_mediaPool, &MediaPoolPanel::timelineOpened, this, [this] {
        showSourceInSingleViewer(false);
        m_timelineViewer->activate();
    });
    // New timelines and compound clips go into the media pool's current bin (Resolve).
    m_timeline->controller()->setBinProvider([this] { return m_mediaPool->currentBin(); });
    // Multicam: angle clicks / number keys in the multicam viewer cut (or switch) the
    // multicam clip under the timeline playhead; Sync Based on In Points uses the source
    // viewer's marks.
    connect(m_sourceViewer, &ViewerPanel::multicamAngleChosen, m_timeline->controller(),
        &TimelineController::chooseMulticamAngle);
    m_mediaPool->setInPointProvider([this](const QString& mediaId) -> int64_t {
        const SourcePlayer* player = m_sourceViewer->sourcePlayer();
        return player ? player->markInUs(mediaId) : 0;
    });
    // F9 / F10 / F12 take the source viewer's clip and marked range.
    m_timeline->controller()->setSourceProvider([this]() -> std::optional<SourceSelection> {
        const std::optional<SourceRange> range = m_sourceViewer->sourceSelection();
        if (!range) {
            return std::nullopt;
        }
        SourceSelection selection {range->mediaId, range->inUs, range->outUs};
        // Unmarked source: Replace lines up the two playheads (spec 4.7).
        const SourcePlayer* player = m_sourceViewer->sourcePlayer();
        if (player && player->markIn() < 0 && player->engine())
            selection.playheadUs = framesToUs(player->engine()->position(), player->fps());
        return selection;
    });
    // F (match frame): the source of the clip under the playhead, parked on the same frame,
    // with the part the timeline uses marked.
    m_timeline->controller()->setMatchFrameHandler(
        [this](const QString& mediaId, int64_t sourceUs, int64_t inUs, int64_t outUs) {
            m_sourceViewer->setSourceMedia(mediaId);
            showSourceInSingleViewer(true);
            if (SourcePlayer* player = m_sourceViewer->sourcePlayer()) {
                const Rational fps = player->fps();
                player->setMarks(usToFrames(inUs, fps), usToFrames(outUs, fps));
                player->engine()->seek(usToFrames(sourceUs, fps));
            }
            m_sourceViewer->activate();
        });
    // Q: switch the transport target between the source and the timeline viewer.
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.toggle_source_timeline"))) {
        connect(m_ctx.actions->action(QStringLiteral("view.toggle_source_timeline")),
            &QAction::triggered, this, [this] {
                if (!isVisible())
                    return;
                if (isSingleViewer()) {
                    // Single mode: Q swaps what the one viewer shows (spec 24.5).
                    showSourceInSingleViewer(!m_singleShowsSource);
                    ViewerPanel* shown = m_singleShowsSource && !m_sourceViewer->isHidden()
                        ? m_sourceViewer
                        : m_timelineViewer;
                    shown->activate();
                    shown->setFocus();
                    return;
                }
                ViewerPanel* next = ViewerPanel::activeViewer() == m_sourceViewer ? m_timelineViewer
                                                                                  : m_sourceViewer;
                next->activate();
                next->setFocus();
            });
    }
    // Alt+F (Find in Media Pool): reveals the source of the selected timeline clip (else the
    // top clip under the playhead) in the media pool, switching to its bin.
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("timeline.find_in_media_pool"))) {
        connect(m_ctx.actions->action(QStringLiteral("timeline.find_in_media_pool")),
            &QAction::triggered, this, [this] {
                if (isVisible())
                    findInMediaPool();
            });
    }
    // Double-click on a transition / click on an fx badge: show it in the Inspector.
    connect(m_timeline->timelineWidget(), &TimelineWidget::inspectRequested, this,
        [this](quint64 clipId, TimelineWidget::InspectTarget target) {
            setPanelVisible(PanelId::Inspector, true);
            m_inspector->inspect(clipId,
                target == TimelineWidget::InspectTarget::Transition ? InspectorPanel::TransitionTab
                                                                    : InspectorPanel::EffectsTab);
        });
    // Keyframes panel: float / dock, Follow Timeline, "Open in Keyframes Panel" from a lane.
    connect(m_keyframes, &KeyframesPanel::floatToggleRequested, this,
        [this] { setKeyframesFloating(!m_keyframesFloating); });
    connect(m_keyframes, &KeyframesPanel::tearOffRequested, this, &EditPage::tearOffKeyframes);
    TimelineWidget* timelineWidget = m_timeline->timelineWidget();
    connect(timelineWidget, &TimelineWidget::viewChanged, m_keyframes, [this, timelineWidget] {
        const auto [first, last] = timelineWidget->visibleFrameRange();
        m_keyframes->setTimelineView(first, last);
    });
    connect(timelineWidget->keyframeLanes(), &KeyframeLanes::openInPanelRequested, this,
        [this](quint64 clipId) {
            if (!m_ctx.selection->isSelected(clipId))
                m_ctx.selection->selectClip(clipId);
            setPanelVisible(PanelId::Keyframes, true);
        });

    // Focus in the timeline makes the program viewer the transport target (ViewerTransport);
    // in single viewer mode the one viewer follows the active one, like Resolve.
    m_timeline->setProperty("kkProgramFocus", true);
    connect(m_timelineViewer, &ViewerPanel::activated, this,
        [this] { showSourceInSingleViewer(false); });

    // Viewer mode button at the right end of both viewer headers (alpha.2 feedback #5).
    for (ViewerPanel* viewer : {m_sourceViewer, m_timelineViewer}) {
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("ViewerModeButton"));
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
        connect(button, &QToolButton::clicked, this, [this] {
            if (QAction* a = singleViewerAction())
                a->trigger(); // keeps the View menu entry in step
            else
                setSingleViewer(!isSingleViewer());
        });
        viewer->addTitleBarWidget(button);
        m_viewerModeButtons.append(button);
    }
    if (m_ctx.actions) {
        connect(m_ctx.actions, &ActionRegistry::shortcutsChanged, this, [this](const QString& id) {
            if (id == QLatin1String("view.single_viewer"))
                updateViewerModeButtons();
        });
    }
    updateViewerModeButtons();
    setupEditingAids();
}

QAction* EditPage::singleViewerAction() const
{
    return m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.single_viewer"))
        ? m_ctx.actions->action(QStringLiteral("view.single_viewer"))
        : nullptr;
}

void EditPage::updateViewerModeButtons()
{
    QString keys;
    if (const QAction* a = singleViewerAction(); a && !a->shortcut().isEmpty())
        keys = QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
    for (QToolButton* button : std::as_const(m_viewerModeButtons)) {
        // The icon shows the layout a click switches to.
        button->setIcon(kk::icon(
            m_singleViewer ? QStringLiteral("dual_viewer") : QStringLiteral("single_viewer")));
        button->setToolTip(m_singleViewer ? tr("Dual Viewer Mode: source and timeline side by "
                                               "side%1")
                                                .arg(keys)
                                          : tr("Single Viewer Mode: one viewer, switch between "
                                               "source and timeline with Q%1")
                                                .arg(keys));
        button->setAccessibleName(
            m_singleViewer ? tr("Dual Viewer Mode") : tr("Single Viewer Mode"));
    }
}

void EditPage::showSourceInSingleViewer(bool source)
{
    m_singleShowsSource = source;
    updateViewers();
}

void EditPage::updateViewers()
{
    const bool hasClip = !m_sourceViewer->sourceMedia().isEmpty();
    const bool showSource = !m_singleViewer || (m_singleShowsSource && hasClip);
    const bool showTimeline = !m_singleViewer || !showSource;
    if (showSource == !m_sourceViewer->isHidden() && showTimeline == !m_timelineViewer->isHidden())
        return;
    // Keep the width the viewers had together: one viewer gets all of it, two share it.
    QList<int> sizes = m_topRow->sizes();
    const int source = m_topRow->indexOf(m_sourceViewer);
    const int program = m_topRow->indexOf(m_timelineViewer);
    const int space = (m_sourceViewer->isHidden() ? 0 : sizes[source])
        + (m_timelineViewer->isHidden() ? 0 : sizes[program]);
    m_sourceViewer->setVisible(showSource);
    m_timelineViewer->setVisible(showTimeline);
    if (space > 0) {
        sizes[source] = showSource ? (showTimeline ? space / 2 : space) : 0;
        sizes[program] = showTimeline ? space - sizes[source] : 0;
        m_topRow->setSizes(sizes);
    }
    if (showSource && !showTimeline && ViewerPanel::activeViewer() == m_timelineViewer)
        m_sourceViewer->activate();
}

void EditPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    if (!m_proportionsApplied && !m_layoutRestored) {
        m_proportionsApplied = true;
        applyDefaultProportions();
    }
    if (m_keyframesFloating && m_keyframesShown && m_keyframesWindow && !event->spontaneous())
        m_keyframesWindow->show();
}

void EditPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    // The floating Keyframes window belongs to the Edit page: it goes away with the page
    // (another page, minimised main window) and comes back with it.
    if (m_keyframesFloating && m_keyframesWindow && !event->spontaneous())
        m_keyframesWindow->hide();
}

void EditPage::applyDefaultWidths()
{
    const int width = m_topRow->width();
    if (width <= 0)
        return;
    const auto scaled
        = [width](int reference) { return int(qint64(reference) * width / kReferenceWidth); };
    // The left column is wider while the docked Keyframes panel is in it (curve editor room).
    const bool keyframesDocked = !m_keyframesFloating && m_keyframesShown;
    const int left = scaled(keyframesDocked ? kKeyframesMinColumnWidth : kLeftColumnWidth);
    const int right = scaled(kRightColumnWidth);
    // The viewers share the rest; a hidden viewer (single viewer mode) gets nothing.
    const bool source = !m_sourceViewer->isHidden();
    const bool program = !m_timelineViewer->isHidden();
    const int viewers = std::max(0, width - left - right);
    const int sourceWidth = source ? (program ? viewers / 2 : viewers) : 0;
    const int programWidth = program ? viewers - sourceWidth : 0;
    QList<int> sizes(m_topRow->count(), 0);
    sizes[m_topRow->indexOf(m_leftColumn)] = left;
    sizes[m_topRow->indexOf(m_sourceViewer)] = sourceWidth;
    sizes[m_topRow->indexOf(m_timelineViewer)] = programWidth;
    sizes[m_topRow->indexOf(m_rightColumn)] = right;
    m_topRow->setSizes(sizes);
}

void EditPage::applyDefaultProportions(bool widthsOnly)
{
    const auto scaled = [](int reference, int total, int referenceTotal) {
        return int(qint64(reference) * total / referenceTotal);
    };
    applyDefaultWidths();
    const int height = m_main->height();
    if (widthsOnly || height <= 0)
        return;
    const int referenceHeight = kTopRowHeight + kBottomRowHeight;
    m_main->setSizes({scaled(kTopRowHeight, height, referenceHeight),
        scaled(kBottomRowHeight, height, referenceHeight)});
    m_bottomRow->setSizes({m_bottomRow->width() - kMixerWidth, kMixerWidth});
}

void EditPage::setPanelVisible(PanelId id, bool visible)
{
    if (id == PanelId::Keyframes) {
        if (visible == m_keyframesShown)
            return;
        showKeyframes(visible);
        emit panelVisibilityChanged(id, visible);
        return;
    }
    QWidget* panel = m_panels.value(id);
    if (!panel || panel->isHidden() == !visible)
        return;
    panel->setVisible(visible);
    updateColumns();
    emit panelVisibilityChanged(id, visible);
}

bool EditPage::isPanelVisible(PanelId id) const
{
    if (id == PanelId::Keyframes)
        return m_keyframesShown;
    const QWidget* panel = m_panels.value(id);
    return panel && !panel->isHidden();
}

bool EditPage::findInMediaPool()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const ClipData* clip = nullptr;
    for (const quint64 id : m_ctx.selection->clips()) {
        clip = editops::clipById(data, id);
        if (clip && (clip->kind == ClipKind::Media || clip->kind == ClipKind::Nested))
            break;
        clip = nullptr;
    }
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    for (auto t = data.videoTracks.crbegin(); !clip && t != data.videoTracks.crend(); ++t)
        clip = t->clipAt(playhead);
    for (auto t = data.audioTracks.cbegin(); !clip && t != data.audioTracks.cend(); ++t)
        clip = t->clipAt(playhead);
    if (!clip)
        return false;
    const QString id = usage::itemIdOf(*clip, m_ctx.project->mediaPool()->state());
    if (id.isEmpty())
        return false;
    if (m_mediaPool->isHidden())
        setPanelVisible(PanelId::MediaPool, true);
    return m_mediaPool->selectMedia(id);
}

void EditPage::updateColumns()
{
    // A column without visible panels would leave an empty strip: hide the column itself.
    auto anyVisible = [](std::initializer_list<QWidget*> panels) {
        return std::any_of(panels.begin(), panels.end(), [](QWidget* w) { return !w->isHidden(); });
    };
    const bool keyframesDocked = !m_keyframesFloating && m_keyframesShown;
    m_leftColumn->setVisible(keyframesDocked || anyVisible({m_mediaPool, m_effects, m_editIndex}));
    m_rightColumn->setVisible(anyVisible({m_inspector, m_metadata}));
}

// ---- Keyframes panel ----

void EditPage::showKeyframes(bool visible)
{
    m_keyframesShown = visible;
    if (m_keyframesFloating) {
        m_keyframes->setVisible(true); // lives in its window; the window is shown / hidden
        if (visible) {
            m_keyframesWindow->show();
            m_keyframesWindow->raise();
            m_keyframesWindow->activateWindow();
        } else {
            m_keyframesWindow->hide();
        }
        return;
    }
    m_keyframes->setVisible(visible);
    updateColumns();
    if (visible)
        ensureKeyframesHeight();
}

void EditPage::ensureKeyframesHeight()
{
    // Docked in the left column: it takes a good part of the column's height from the other
    // visible panels there (each keeps a usable minimum) ...
    QList<int> sizes = m_leftColumn->sizes();
    const int index = m_leftColumn->indexOf(m_keyframes);
    if (index >= 0 && sizes.size() == m_leftColumn->count()
        && sizes[index] < kKeyframesMinDockHeight) {
        int total = 0;
        int spare = 0;
        for (int i = 0; i < sizes.size(); ++i) {
            total += sizes[i];
            if (i != index && !m_leftColumn->widget(i)->isHidden())
                spare += std::max(0, sizes[i] - kMinColumnPanelHeight);
        }
        const int want = std::max(kKeyframesMinDockHeight, int(total * kKeyframesDockShare));
        const int give = std::min(want - sizes[index], spare);
        if (give > 0) {
            int left = give;
            for (int i = 0; i < sizes.size() && left > 0; ++i) {
                if (i == index || m_leftColumn->widget(i)->isHidden())
                    continue;
                const int own = std::max(0, sizes[i] - kMinColumnPanelHeight);
                const int take = std::min(left, int(qint64(give) * own / spare) + 1);
                sizes[i] -= take;
                left -= take;
            }
            sizes[index] += give - left;
            m_leftColumn->setSizes(sizes);
        }
    }
    // ... and the column gets wide enough for its curve editor (taken from the viewers).
    QList<int> row = m_topRow->sizes();
    const int col = m_topRow->indexOf(m_leftColumn);
    const int width = m_topRow->width();
    const int minWidth = int(qint64(kKeyframesMinColumnWidth) * width / kReferenceWidth);
    if (col < 0 || width <= 0 || row[col] >= minWidth)
        return;
    int need = minWidth - row[col];
    for (QWidget* viewer : {m_sourceViewer, m_timelineViewer}) {
        const int i = m_topRow->indexOf(viewer);
        if (viewer->isHidden() || need <= 0)
            continue;
        const int take = std::min(need, std::max(0, row[i] - 200));
        row[i] -= take;
        row[col] += take;
        need -= take;
    }
    m_topRow->setSizes(row);
}

void EditPage::setKeyframesFloating(bool floating)
{
    if (floating == m_keyframesFloating)
        return;
    m_keyframesFloating = floating;
    if (floating) {
        if (!m_keyframesWindow) {
            // A top-level window owned by the page (transient for the main window; can be moved
            // to a second monitor). The application's actions are added so shortcuts (Space,
            // J/K/L, undo, ...) keep working while it has focus.
            m_keyframesWindow = new QWidget(this, Qt::Window);
            m_keyframesWindow->setObjectName(QStringLiteral("KeyframesWindow"));
            // "Keyframes — Kinoklip": the app name tells the window apart in the task manager
            // and window switcher (KWin shows the title in its decoration).
            m_keyframesWindow->setWindowTitle(
                tr("Keyframes — %1").arg(QString::fromLatin1(appinfo::name)));
            m_keyframesWindow->resize(kKeyframesWindowSize);
            auto* layout = new QVBoxLayout(m_keyframesWindow);
            layout->setContentsMargins(0, 0, 0, 0);
            if (m_ctx.actions)
                m_keyframesWindow->addActions(m_ctx.actions->actions());
            m_keyframesWindow->installEventFilter(this);
        }
        m_keyframesWindow->layout()->addWidget(m_keyframes); // reparents out of the splitter
        m_keyframes->show();
        m_keyframes->setFloating(true);
        if (m_keyframesShown) {
            m_keyframesWindow->show();
            m_keyframesWindow->raise();
        }
    } else {
        if (m_keyframesWindow)
            m_keyframesWindow->hide();
        m_leftColumn->addWidget(m_keyframes);
        m_keyframes->setFloating(false);
        m_keyframes->setVisible(m_keyframesShown);
        updateColumns();
        if (m_keyframesShown)
            ensureKeyframesHeight();
    }
}

void EditPage::tearOffKeyframes()
{
    if (m_keyframesFloating)
        return;
    // X11 lets a client place its window: put the header under the pointer. On Wayland the
    // compositor places it (the move is ignored there) and the interactive move below makes it
    // follow the pointer.
    const QPoint pointer = QCursor::pos();
    if (m_keyframesWindow)
        m_keyframesWindow->move(pointer - QPoint(m_keyframesWindow->width() / 2, kTearOffGrabY));
    setKeyframesFloating(true);
    if (!m_keyframesWindow)
        return;
    if (m_keyframesWindow->pos() == QPoint())
        m_keyframesWindow->move(pointer - QPoint(m_keyframesWindow->width() / 2, kTearOffGrabY));
    // Hand the still-pressed drag over to the window manager once the new window is mapped:
    // xdg_toplevel.move / _NET_WM_MOVERESIZE with the press serial. The compositor accepts it
    // while the button is held (KWin checks the seat's implicit grab, not the surface); if it
    // refuses, the window simply stays where it opened.
    auto attempts = std::make_shared<int>(0);
    auto tryMove = std::make_shared<std::function<void()>>();
    *tryMove = [this, attempts, tryMove] {
        if (!m_keyframesWindow || !m_keyframesFloating
            || !(QGuiApplication::mouseButtons() & Qt::LeftButton))
            return;
        QWindow* handle = m_keyframesWindow->windowHandle();
        if (handle && handle->isExposed()) {
            handle->startSystemMove();
            return;
        }
        if (++*attempts < kTearOffMoveAttempts)
            QTimer::singleShot(kTearOffMoveRetryMs, this, *tryMove);
    };
    QTimer::singleShot(0, this, *tryMove);
}

bool EditPage::eventFilter(QObject* watched, QEvent* event)
{
    // Until the user drags a column splitter, keep the default column proportions at every
    // size (the window often starts small and is maximised right after the first show). Done
    // when the top row receives its new size, before QSplitter::resizeEvent() lays it out:
    // then the row is laid out once per resize step, at the default widths (QSplitter's own
    // pass finds every child at its size already). Re-applying from EditPage::resizeEvent()
    // laid every panel of the row out twice per live-resize step (test_resizebench sweep).
    if (watched == m_topRow && event->type() == QEvent::Resize && m_defaultWidths
        && m_proportionsApplied)
        applyDefaultWidths();
    // Closing the floating window hides the panel (the interface-bar toggle follows); it opens
    // again floating.
    if (watched == m_keyframesWindow && event->type() == QEvent::Close && m_keyframesShown) {
        m_keyframesShown = false;
        emit panelVisibilityChanged(PanelId::Keyframes, false);
    }
    return Page::eventFilter(watched, event);
}

void EditPage::setSingleViewer(bool single)
{
    if (single == m_singleViewer)
        return;
    m_singleViewer = single;
    // Entering single mode shows the viewer that was in use (the source one when it was the
    // transport target and has a clip).
    if (single)
        m_singleShowsSource = ViewerPanel::activeViewer() == m_sourceViewer;
    updateViewers();
    updateViewerModeButtons();
    emit singleViewerChanged(single);
}

bool EditPage::singleViewerShowsSource() const
{
    return m_singleViewer && !m_sourceViewer->isHidden();
}

bool EditPage::isSingleViewer() const
{
    return m_singleViewer;
}

void EditPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    settings.setValue(QStringLiteral("main"), m_main->saveState());
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("bottomRow"), m_bottomRow->saveState());
    settings.setValue(QStringLiteral("leftColumn"), m_leftColumn->saveState());
    settings.setValue(QStringLiteral("rightColumn"), m_rightColumn->saveState());
    settings.setValue(QStringLiteral("singleViewer"), isSingleViewer());
    settings.setValue(QStringLiteral("keyframes/floating"), m_keyframesFloating);
    if (m_keyframesWindow)
        settings.setValue(
            QStringLiteral("keyframes/windowGeometry"), m_keyframesWindow->saveGeometry());
    m_keyframes->saveState(settings, QStringLiteral("keyframes/"));
    for (PanelId id : kAllPanels)
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void EditPage::restoreLayout(const QSettings& settings)
{
    // Splitter sizes stored by an older layout version are ignored (kLayoutVersion): version 1
    // could hold squeezed side columns, version 2 still had the Keyframes panel in the page's
    // main splitter. The page then starts with the default proportions; panel visibility below
    // is restored either way.
    m_layoutRestored = settings.contains(QStringLiteral("main"))
        && settings.value(QStringLiteral("layoutVersion"), 1).toInt() >= kLayoutVersion;
    m_defaultWidths = !m_layoutRestored;
    if (m_layoutRestored) {
        m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
        m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
        m_leftColumn->restoreState(settings.value(QStringLiteral("leftColumn")).toByteArray());
    }
    m_bottomRow->restoreState(settings.value(QStringLiteral("bottomRow")).toByteArray());
    m_rightColumn->restoreState(settings.value(QStringLiteral("rightColumn")).toByteArray());
    // Splitter states may carry visibility too; the explicit flags below win.
    for (PanelId id : kAllPanels) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
    updateColumns();
    setSingleViewer(settings.value(QStringLiteral("singleViewer"), false).toBool());
    m_keyframes->restoreState(settings, QStringLiteral("keyframes/"));
    if (settings.value(QStringLiteral("keyframes/floating"), false).toBool()) {
        setKeyframesFloating(true);
        if (settings.contains(QStringLiteral("keyframes/windowGeometry")))
            m_keyframesWindow->restoreGeometry(
                settings.value(QStringLiteral("keyframes/windowGeometry")).toByteArray());
    }
    // The splitter state may have changed the docked panel's visibility: the flag wins.
    showKeyframes(m_keyframesShown);
}

} // namespace kk
