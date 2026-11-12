// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframesPanel implementation: layout, model refresh, time view, wheel, persistence.
#include "ui/keyframes/KeyframesPanel.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/SchemeColors.h"
#include "ui/comp/CompSession.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframeRuler.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>

namespace kk {

namespace kc = keyframecommands;
namespace ko = keyframeops;

namespace {
constexpr int kAxisWidth = 64; ///< value axis column of the curves (ruler / dope sheet align)
constexpr int kTreeMinWidth = 96; ///< smallest width of the tree and of the time views
constexpr int kTreeWidth = 380; ///< default width of the parameter tree
constexpr double kFitMargin = 0.04; ///< Fit Clip: free space each side, fraction of the span
constexpr double kMinPpf = 0.01; ///< time zoom limits (pixels per frame)
constexpr double kMaxPpf = 80.0;
constexpr double kWheelZoomPerStep = 1.2; ///< time zoom per wheel notch
constexpr double kWheelScrollPx = 60.0; ///< time scroll per wheel notch
constexpr int kToolButtonSize = 24;
/// How far (px) the pointer must leave the docked panel while dragging its header before it
/// tears off into its own window.
constexpr int kTearOffDistancePx = 24;
} // namespace

KeyframesPanel::KeyframesPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Keyframes"), parent)
    , m_ctx(ctx)
{
    setPanelInfo(this, tr("Keyframes"),
        tr("Editor for the keyframes of the selected clips: a parameter list beside a dope sheet "
           "or a curve view on the timeline's time."));
    setPanelKey(QStringLiteral("keyframes")); // in-app help: panel:keyframes
    setObjectName(QStringLiteral("KeyframesPanel"));
    m_map.originX = kAxisWidth;
    keyframecommands::bindEasingActions(ctx); // spec 49.4

    // Title bar: the shown item's name and the float / dock button.
    m_itemLabel = new QLabel;
    m_itemLabel->setObjectName(QStringLiteral("KeyframesItemName"));
    addTitleBarWidget(m_itemLabel);
    m_floatButton = new QToolButton;
    m_floatButton->setObjectName(QStringLiteral("KeyframesFloatButton"));
    m_floatButton->setAutoRaise(true);
    m_floatButton->setFocusPolicy(Qt::NoFocus);
    m_floatButton->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
    // Labelled (owner feedback: detaching was not discoverable with an icon alone).
    m_floatButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_floatButton, &QToolButton::clicked, this, &KeyframesPanel::floatToggleRequested);
    addTitleBarWidget(m_floatButton);
    setFloating(false);
    // Header gestures: double-click floats / docks, dragging out tears off, dragging a floating
    // panel's header moves its window.
    titleBar()->installEventFilter(this);

    auto* body = new QWidget;
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildToolRow());

    m_stack = new QStackedWidget;
    m_stack->addWidget(createPlaceholderContent(
        tr("Select a clip on the timeline to see and edit its keyframes here.")));

    // Left: header (as tall as the ruler, so tree rows line up with dope sheet rows) + tree.
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    m_tree = new KeyframeParamTree(ctx);
    m_ruler = new KeyframeRuler(ctx, m_map);
    m_ruler->setLabelWidth(kAxisWidth);
    m_treeHeader = new QLabel(tr("Parameters"));
    m_treeHeader->setObjectName(QStringLiteral("KeyframesTreeHeader"));
    m_treeHeader->setFixedHeight(m_ruler->sizeHint().height());
    m_treeHeader->setContentsMargins(metrics::kSpaceM, 0, 0, 0);
    m_treeHeader->setAutoFillBackground(true);
    QFont hf = m_treeHeader->font();
    hf.setBold(true);
    m_treeHeader->setFont(hf);
    leftLayout->addWidget(m_treeHeader);
    leftLayout->addWidget(m_tree, 1);

    // Right: ruler, dope sheet or curves, time scroll bar.
    m_right = new QWidget;
    auto* rightLayout = new QVBoxLayout(m_right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    m_dope = new DopeSheetView(ctx, m_map, nullptr);
    m_dope->setProperty("kkVimContext", QStringLiteral("keyframes")); // Vim mode (spec 83)
    m_dope->setLabelWidth(kAxisWidth);
    m_dope->setDrawLabels(false);
    m_dope->setRowHeight(KeyframeParamTree::kRowHeight);
    m_dope->setMinimumHeight(24);
    m_dope->setPanelColors(true);
    m_curves = new CurveView(ctx, m_map, nullptr);
    m_curves->setProperty("kkVimContext", QStringLiteral("keyframes"));
    m_curves->setLabelWidth(kAxisWidth);
    m_curves->setAxisLabelsInColumn(true);
    m_curves->setTimeGrid(true);
    m_curves->setMinimumHeight(40);
    m_curves->setPanelColors(true);
    m_curves->setTimePanner([this](double dx) { scrollTimePx(-dx); });
    m_dope->setPanner([this](double dx, double dy) {
        scrollTimePx(-dx);
        QScrollBar* rows = m_tree->verticalScrollBar();
        rows->setValue(rows->value() - qRound(dy));
    });
    m_viewStack = new QStackedWidget;
    m_viewStack->setObjectName(QStringLiteral("KeyframesViewStack"));
    m_viewStack->addWidget(m_dope);
    m_viewStack->addWidget(m_curves);
    m_hscroll = new QScrollBar(Qt::Horizontal);
    rightLayout->addWidget(m_ruler);
    rightLayout->addWidget(m_viewStack, 1);
    rightLayout->addWidget(m_hscroll);
    // Wheel events the views ignore propagate here (see eventFilter()).
    m_right->installEventFilter(this);

    m_split = new PanelSplitter(Qt::Horizontal);
    m_split->setChildrenCollapsible(false);
    left->setMinimumWidth(kTreeMinWidth); // narrow, never hidden
    m_tree->setMinimumHeight(40);
    m_right->setMinimumWidth(kTreeMinWidth);
    m_split->setHandleWidth(3);
    m_split->addWidget(left);
    m_split->addWidget(m_right);
    m_split->setStretchFactor(1, 1);
    m_split->setSizes({kTreeWidth, 1000});
    m_stack->addWidget(m_split);
    bodyLayout->addWidget(m_stack, 1);
    setContentWidget(body);

    // ---- Wiring ----
    m_tree->setCurveShownProvider([this](const ParamId& id) { return m_curves->isCurveShown(id); });
    connect(m_tree, &KeyframeParamTree::collapseToggled, this, [this](const QString& key) {
        if (!m_collapsed.remove(key))
            m_collapsed.insert(key);
        rebuild();
    });
    connect(m_tree, &KeyframeParamTree::curvesToggled, this,
        [this](const QVector<ParamId>& ids, bool shown) {
            for (const ParamId& id : ids)
                m_curves->setCurveShown(id, shown);
            if (shown) // a curve was asked for: show it (off has nothing to show)
                setView(View::Curves);
        });
    connect(m_tree, &KeyframeParamTree::rowActivated, this, [this](const QVector<ParamId>& ids) {
        if (ids.size() == 1 && m_curves->isCurveShown(ids.front()))
            m_curves->setActiveCurve(ids.front());
    });
    connect(m_curves, &CurveView::curvesChanged, m_tree, &KeyframeParamTree::refresh);
    connect(m_curves, &CurveView::fitTimeRequested, this, [this](qint64 first, qint64 last) {
        setFreeMode();
        fitFrames(double(first), double(last + 1));
    });
    connect(m_tree->verticalScrollBar(), &QScrollBar::valueChanged, this,
        &KeyframesPanel::syncDopeWindow);
    connect(m_tree->verticalScrollBar(), &QScrollBar::rangeChanged, this,
        &KeyframesPanel::syncDopeWindow);
    connect(m_hscroll, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_syncingScroll)
            return;
        setFreeMode();
        m_map.firstFrame = value / m_map.ppf;
        applyView();
    });
    connect(ctx.selection, &SelectionModel::changed, this, &KeyframesPanel::rebuild);
    connect(ctx.project->timeline(), &Timeline::changed, this, &KeyframesPanel::rebuild);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this,
        &KeyframesPanel::onPlayheadChanged);
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        &KeyframesPanel::applyRegionColors);
    applyRegionColors();
    setupSplineTools();
    setView(View::Keyframes);
    rebuild();
}

KeyframesPanel::~KeyframesPanel() = default;

QWidget* KeyframesPanel::buildToolRow()
{
    // A QToolBar: what does not fit moves, in order, into its extension menu ("»"), and its
    // minimum width is only that button, so the panel can be made narrow (spec 15.27).
    auto* row = new QToolBar;
    row->setObjectName(QStringLiteral("KeyframesToolRow"));
    row->setMovable(false);
    row->setFloatable(false);
    row->setContextMenuPolicy(Qt::PreventContextMenu);
    row->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
    row->layout()->setContentsMargins(metrics::kSpaceS, 0, metrics::kSpaceS, 0);
    row->layout()->setSpacing(metrics::kSpaceXs);
    struct Adder
    {
        QToolBar* bar;
        QAction* addWidget(QWidget* w) { return bar->addWidget(w); }
        void addSpacing(int px)
        {
            auto* w = new QWidget;
            w->setFixedSize(px, 1);
            bar->addWidget(w);
        }
        void addStretch(int)
        {
            auto* w = new QWidget;
            w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            w->setMinimumWidth(0);
            bar->addWidget(w);
        }
    } hh {row};
    Adder* h = &hh;
    auto button
        = [&](const QString& icon, const QString& text, const QString& tip, bool checkable) {
              auto* b = new QToolButton(row);
              b->setAutoRaise(true);
              b->setFocusPolicy(Qt::NoFocus);
              b->setCheckable(checkable);
              b->setToolTip(tip);
              b->setObjectName(icon.isEmpty() ? text : icon);
              if (!icon.isEmpty()) {
                  b->setIcon(kk::icon(icon));
                  b->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
                  b->setFixedSize(kToolButtonSize, kToolButtonSize);
              } else {
                  b->setText(text);
                  b->setFixedHeight(kToolButtonSize);
              }
              m_toolActions.insert(b, h->addWidget(b));
              return b;
          };
    auto separator = [&] { h->addSpacing(metrics::kSpaceL); };

    // The view switch: two checkable tool buttons in an exclusive group, like the other
    // buttons of this row (spec 15.22).
    auto segment = [&](const QString& text, const QString& name, const QString& tip, View view) {
        QToolButton* b = button({}, text, tip, true);
        b->setObjectName(name);
        m_viewGroup->addButton(b);
        connect(b, &QToolButton::clicked, this, [this, view] { setView(view); });
        return b;
    };
    m_viewGroup = new QButtonGroup(this);
    m_viewGroup->setExclusive(true);
    m_keyframesViewButton = segment(tr("Keyframes"), QStringLiteral("KeyframesViewKeyframes"),
        tr("Keyframes view: the keys of each parameter along time"), View::Keyframes);
    m_curvesViewButton = segment(tr("Curves"), QStringLiteral("KeyframesViewCurves"),
        tr("Curves view: the values of the parameters as curves"), View::Curves);
    separator();

    struct Preset
    {
        const char* icon;
        const char* tip;
        Interp interp;
    };
    const Preset presets[] = {
        {"interp_linear", QT_TR_NOOP("Linear"), Interp::Linear},
        {"interp_smooth", QT_TR_NOOP("Smooth (Bezier)"), Interp::Smooth},
        {"interp_ease_in", QT_TR_NOOP("Ease In"), Interp::EaseIn},
        {"interp_ease_out", QT_TR_NOOP("Ease Out"), Interp::EaseOut},
        {"interp_ease_in_out", QT_TR_NOOP("Ease In and Out"), Interp::EaseInOut},
        {"interp_hold", QT_TR_NOOP("Static (Hold)"), Interp::Hold},
    };
    for (const Preset& p : presets) {
        QToolButton* b = button(QLatin1String(p.icon), {},
            tr("%1: applies to the selected keyframes").arg(tr(p.tip)), false);
        const Interp interp = p.interp;
        connect(b, &QToolButton::clicked, this,
            [this, interp] { kc::setSelectedInterp(m_ctx, interp); });
    }
    connect(button(QStringLiteral("interp_flatten"), {}, tr("Flatten Handles"), false),
        &QToolButton::clicked, this, [this] { kc::flattenSelected(m_ctx); });
    // Spline tools (Fusion's Spline editor): Step In, Reverse, Time Stretch and the loop modes.
    m_stepInButton = button({}, tr("Step In"),
        tr("Step In (I): the value jumps right after the previous key. Step Out is the "
           "Static (Hold) button."),
        false);
    connect(m_stepInButton, &QToolButton::clicked, this, [this] { kc::stepSelected(m_ctx, true); });
    m_reverseButton = button(
        {}, tr("Reverse"), tr("Reverse (V): mirror the selected keyframes in time"), false);
    connect(m_reverseButton, &QToolButton::clicked, this, [this] { kc::reverseSelected(m_ctx); });
    m_stretchButton = button({}, tr("Time Stretch"),
        tr("Time Stretch: scale the time of the selected keyframes around the playhead"), false);
    connect(m_stretchButton, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const double percent = QInputDialog::getDouble(this, tr("Time Stretch"),
            tr("Scale the selected keyframes around the playhead (percent of their time):"), 200.0,
            1.0, 10000.0, 1, &ok);
        if (ok)
            stretchSelectedKeys(percent / 100.0);
    });
    m_loopButton = button({}, tr("Loop"),
        tr("Set Loop: repeat the keyed range after the last key. The arrow lists Ping-Pong, "
           "Relative, Linear, the Pre-Loop variants and Remove Loop."),
        false);
    m_loopButton->setPopupMode(QToolButton::MenuButtonPopup);
    auto* loopMenu = new QMenu(m_loopButton);
    m_loopButton->setMenu(loopMenu);
    connect(loopMenu, &QMenu::aboutToShow, this, [this, loopMenu] {
        loopMenu->clear();
        fillSplineMenu(*loopMenu);
    });
    connect(m_loopButton, &QToolButton::clicked, this, [this] {
        kc::setExtrapolation(m_ctx, false, true, Extrapolation::Loop, shownCurveRefs());
    });
    m_timeBox = new QDoubleSpinBox(row);
    m_timeBox->setObjectName(QStringLiteral("KeyframesTimeBox"));
    m_timeBox->setPrefix(tr("T "));
    m_timeBox->setDecimals(0);
    m_timeBox->setRange(-1000000, 1000000);
    m_timeBox->setKeyboardTracking(false);
    m_timeBox->setToolTip(tr("Frame of the selected keyframe (clip frames); type to move it"));
    m_valueBox = new QDoubleSpinBox(row);
    m_valueBox->setObjectName(QStringLiteral("KeyframesValueBox"));
    m_valueBox->setPrefix(tr("V "));
    m_valueBox->setDecimals(3);
    m_valueBox->setRange(-1e9, 1e9);
    m_valueBox->setKeyboardTracking(false);
    m_valueBox->setToolTip(tr("Value of the selected keyframe; type to change it"));
    m_timeBox->setEnabled(false);
    m_valueBox->setEnabled(false);
    connect(m_timeBox, &QDoubleSpinBox::valueChanged, this, [this] { applyKeyBox(true); });
    connect(m_valueBox, &QDoubleSpinBox::valueChanged, this, [this] { applyKeyBox(false); });
    h->addWidget(m_timeBox);
    h->addWidget(m_valueBox);
    // Compositing page: which nodes' curves the list shows and which curves are drawn.
    m_selOnlyButton = button({}, tr("Sel"),
        tr("Sel: list only the selected nodes' parameters; off: every node of the composition"),
        true);
    m_selOnlyButton->setChecked(true);
    connect(m_selOnlyButton, &QToolButton::toggled, this, [this](bool on) {
        m_selOnly = on;
        rebuild();
    });
    m_allCurvesButton
        = button({}, tr("All"), tr("Show the curves of every listed parameter"), false);
    connect(m_allCurvesButton, &QToolButton::clicked, this, [this] {
        for (const CurveInfo& c : m_curves->curves())
            m_curves->setCurveShown(c.id, true);
    });
    m_noCurvesButton = button({}, tr("None"), tr("Hide every curve"), false);
    connect(m_noCurvesButton, &QToolButton::clicked, this, [this] {
        for (const CurveInfo& c : m_curves->curves())
            m_curves->setCurveShown(c.id, false);
    });
    for (QToolButton* b : {m_selOnlyButton, m_allCurvesButton, m_noCurvesButton})
        m_toolActions.value(b)->setVisible(false); // Compositing page only
    separator();

    m_normalisedButton = button({}, tr("Normalised"),
        tr("Scale every curve to its own value range (the axis shows the active curve); off: "
           "one shared value axis"),
        true);
    m_normalisedButton->setChecked(true);
    connect(m_normalisedButton, &QToolButton::toggled, this, [this](bool on) {
        m_curves->setAxisMode(on ? CurveView::AxisMode::Normalised : CurveView::AxisMode::Absolute);
    });
    connect(button(QStringLiteral("curve_fit"), {}, tr("Fit All: every curve and the whole clip"),
                false),
        &QToolButton::clicked, this, [this] { fitAll(); });
    m_fitSelectionButton
        = button({}, tr("Fit Selection"), tr("Zoom to the selected keyframes"), false);
    connect(
        m_fitSelectionButton, &QToolButton::clicked, this, [this] { m_curves->fitSelection(); });
    h->addStretch(1);

    auto* viewLabel = new QLabel(tr("Time:"), row);
    h->addWidget(viewLabel);
    m_viewCombo = new QComboBox(row);
    m_viewCombo->setObjectName(QStringLiteral("KeyframesViewMode"));
    m_viewCombo->setFocusPolicy(Qt::NoFocus);
    m_viewCombo->addItem(tr("Fit Clip"), int(ViewMode::FitClip));
    m_viewCombo->addItem(tr("Follow Timeline"), int(ViewMode::FollowTimeline));
    m_viewCombo->addItem(tr("Free"), int(ViewMode::Free));
    m_viewCombo->setToolTip(tr("Fit Clip: the clip fills the width. Follow Timeline: the same "
                               "range as the timeline. Free: zoom with Ctrl+wheel, scroll with "
                               "Shift+wheel."));
    connect(m_viewCombo, &QComboBox::activated, this,
        [this](int index) { setViewMode(ViewMode(m_viewCombo->itemData(index).toInt())); });
    h->addWidget(m_viewCombo);
    h->addSpacing(metrics::kSpaceM);
    m_showAllButton = button({}, tr("Show All"),
        tr("List every animatable parameter (to add keys to a parameter that has none); off: "
           "only animated parameters"),
        true);
    connect(m_showAllButton, &QToolButton::toggled, this, &KeyframesPanel::setShowAllParameters);

    // The tool row's width does not depend on the view: Normalised and Fit Selection are
    // hidden by action, and the toolbar's own minimum is just the extension button, so a
    // docked column does not jump when the view switches.
    return row;
}

// ---- Compositing page mode and spline tools ----

void KeyframesPanel::attachComposition(CompSession* session, View view)
{
    m_session = session;
    const bool spline = view == View::Curves;
    setTitle(spline ? tr("Spline") : tr("Keyframes"));
    setPanelInfo(this, spline ? tr("Spline") : tr("Keyframes"),
        spline ? tr("Curves of the animated parameters of the selected nodes, with the spline "
                    "tools: Smooth, Linear, Step, Ease, Reverse, Time Stretch and Loop.")
               : tr("The keyframes of the selected nodes' animated parameters along time."));
    setPanelKey(spline ? QStringLiteral("spline") : QStringLiteral("keyframes_comp"));
    setObjectName(spline ? QStringLiteral("SplinePanel") : QStringLiteral("CompKeyframesPanel"));
    m_floatButton->hide();
    m_keyframesViewButton->hide();
    m_curvesViewButton->hide();
    m_toolActions.value(m_keyframesViewButton)->setVisible(false);
    m_toolActions.value(m_curvesViewButton)->setVisible(false);
    for (QToolButton* b : {m_selOnlyButton, m_allCurvesButton, m_noCurvesButton})
        m_toolActions.value(b)->setVisible(b == m_selOnlyButton || spline);
    QWidget* oldPlaceholder = m_stack->widget(0);
    m_stack->removeWidget(oldPlaceholder);
    oldPlaceholder->deleteLater();
    m_stack->insertWidget(0,
        createPlaceholderContent(
            tr("Select a node with animated parameters. Animate a parameter with the keyframe "
               "button in the Inspector, or turn on Show All to list every parameter.")));
    connect(session, &CompSession::selectionChanged, this, &KeyframesPanel::rebuild);
    connect(session, &CompSession::compositionChanged, this, &KeyframesPanel::rebuild);
    connect(session, &CompSession::clipChanged, this, &KeyframesPanel::rebuild);
    setView(view);
    rebuild();
}

void KeyframesPanel::setupSplineTools()
{
    // Right-click menu of both views: the loop, reverse and stretch commands after the presets.
    auto extender = [this](QMenu& menu) {
        menu.addSeparator();
        fillSplineMenu(menu);
    };
    m_curves->setMenuExtender(extender);
    m_dope->setMenuExtender(extender);
    kc::bindSplineActions(m_ctx);
    if (!m_ctx.actions)
        return;
    // The registry's commands (Clip > Keyframes, keys S L I O V T F) act while this panel has
    // the focus: the actions are added to the panel and have a widget-with-children context.
    QStringList ids;
    for (const kc::SplineAction& a : kc::splineActions())
        ids << QLatin1String(a.id);
    for (const kc::EasingAction& a : kc::easingActions())
        ids << QLatin1String(a.id);
    ids << QStringLiteral("clip.keyframe_fit");
    for (const QString& id : std::as_const(ids)) {
        if (m_ctx.actions->contains(id))
            addAction(m_ctx.actions->action(id));
    }
    if (m_ctx.actions->contains(QStringLiteral("clip.keyframe_fit"))) {
        connect(m_ctx.actions->action(QStringLiteral("clip.keyframe_fit")), &QAction::triggered,
            this, [this] {
                const QWidget* focus = QApplication::focusWidget();
                if (focus && (focus == this || isAncestorOf(focus)))
                    fitAll();
            });
    }
}

void KeyframesPanel::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    if (m_session) {
        rebuild();
        if (!m_treeWidthSet) {
            // Names and the value box readable at once: about 340 px, at most 60 % of the panel.
            m_treeWidthSet = true;
            QTimer::singleShot(0, this, [this] {
                const int total = std::max(1, m_split->width());
                const int tree = std::min(340, total * 60 / 100);
                m_split->setSizes({tree, total - tree});
            });
        }
    }
}

QVector<KeyframeRef> KeyframesPanel::shownCurveRefs() const
{
    QVector<KeyframeRef> refs;
    for (const ParamId& id : m_curves->shownCurves())
        refs.append({id.clipId, id.path, 0});
    return refs;
}

void KeyframesPanel::fillSplineMenu(QMenu& menu)
{
    const QVector<KeyframeRef> fallback = shownCurveRefs();
    auto add = [&](QMenu& into, const QString& text, bool pre, Extrapolation mode) {
        into.addAction(text, this, [this, pre, mode, fallback] {
            kc::setExtrapolation(m_ctx, pre, !pre, mode, fallback);
        });
    };
    add(menu, tr("Set Loop"), false, Extrapolation::Loop);
    add(menu, tr("Set Ping-Pong"), false, Extrapolation::PingPong);
    add(menu, tr("Set Relative"), false, Extrapolation::Relative);
    add(menu, tr("Linear Extrapolation"), false, Extrapolation::Linear);
    add(menu, tr("Remove Loop"), false, Extrapolation::Constant);
    QMenu* pre = menu.addMenu(tr("Pre-Loop"));
    add(*pre, tr("Set Pre-Loop"), true, Extrapolation::Loop);
    add(*pre, tr("Set Pre-Ping-Pong"), true, Extrapolation::PingPong);
    add(*pre, tr("Set Pre-Relative"), true, Extrapolation::Relative);
    add(*pre, tr("Linear Extrapolation Before"), true, Extrapolation::Linear);
    add(*pre, tr("Remove Pre-Loop"), true, Extrapolation::Constant);
    menu.addSeparator();
    menu.addAction(tr("Step In"), this, [this] { kc::stepSelected(m_ctx, true); });
    menu.addAction(tr("Reverse"), this, [this] { kc::reverseSelected(m_ctx); });
    menu.addAction(tr("Time Stretch…"), this, [this] { m_stretchButton->click(); });
}

bool KeyframesPanel::stretchSelectedKeys(double factor)
{
    return kc::stretchSelectedAtPlayhead(m_ctx, factor);
}

void KeyframesPanel::fitAll()
{
    m_curves->fitView();
    setViewMode(ViewMode::FitClip);
}

void KeyframesPanel::updateKeyBoxes()
{
    // One selected key shows its clip frame and value; typing in a box edits that key.
    const QVector<KeyframeRef> sel = m_ctx.selection->keyframes();
    const ClipData* clip = nullptr;
    const AnimatedValue* v = nullptr;
    qsizetype index = -1;
    if (sel.size() == 1) {
        clip = editops::clipById(m_ctx.project->timeline()->data(), sel.front().clipId);
        v = clip ? ko::findValue(*clip, sel.front().paramPath) : nullptr;
        index = v ? v->indexOf(sel.front().frame) : -1;
    }
    m_updatingBoxes = true;
    const bool on = index >= 0 && !ko::isShapePath(sel.front().paramPath);
    m_timeBox->setEnabled(on);
    m_valueBox->setEnabled(on);
    if (on) {
        double sign = 1.0;
        for (const CurveInfo& c : m_curves->curves()) {
            if (c.id.clipId == sel.front().clipId && c.id.path == sel.front().paramPath)
                sign = c.displayNegated ? -1.0 : 1.0;
        }
        m_timeBox->setValue(double(sel.front().frame));
        m_valueBox->setValue(sign * v->keys[index].value);
    }
    m_updatingBoxes = false;
}

void KeyframesPanel::applyKeyBox(bool time)
{
    const QVector<KeyframeRef> sel = m_ctx.selection->keyframes();
    if (m_updatingBoxes || sel.size() != 1)
        return;
    const KeyframeRef ref = sel.front();
    const ClipData* clip = editops::clipById(m_ctx.project->timeline()->data(), ref.clipId);
    const AnimatedValue* v = clip ? ko::findValue(*clip, ref.paramPath) : nullptr;
    const qsizetype index = v ? v->indexOf(ref.frame) : -1;
    if (index < 0)
        return;
    if (time) {
        const int64_t delta = int64_t(std::llround(m_timeBox->value())) - ref.frame;
        if (delta == 0)
            return;
        m_ctx.project->beginEdit(tr("Move Keyframes"));
        const QVector<KeyframeRef> moved
            = ko::moveKeys(m_ctx.project->timeline()->mutableData(), {ref}, delta);
        m_ctx.project->endEdit();
        m_ctx.selection->setKeyframes(moved);
    } else {
        double sign = 1.0;
        for (const CurveInfo& c : m_curves->curves()) {
            if (c.id.clipId == ref.clipId && c.id.path == ref.paramPath)
                sign = c.displayNegated ? -1.0 : 1.0;
        }
        const double delta = sign * m_valueBox->value() - v->keys[index].value;
        if (delta == 0.0)
            return;
        m_ctx.project->beginEdit(tr("Change Keyframe Value"));
        ko::offsetKeyValues(m_ctx.project->timeline()->mutableData(), {ref}, delta);
        m_ctx.project->endEdit();
    }
}

// ---- Options ----

void KeyframesPanel::setFloating(bool floating)
{
    m_floating = floating;
    m_floatButton->setIcon(
        kk::icon(floating ? QStringLiteral("window_dock") : QStringLiteral("window_float")));
    m_floatButton->setText(floating ? tr("Dock") : tr("Float"));
    m_floatButton->setToolTip(floating
            ? tr("Dock the Keyframes panel back into the Edit page (or double-click the header)."
                 "\nDrag the header to move the window.")
            : tr("Open the Keyframes panel in its own window, e.g. on a second monitor.\n"
                 "You can also double-click the header, or drag the header out of the panel."));
    m_floatButton->setAccessibleName(floating ? tr("Dock") : tr("Float"));
    titleBar()->setToolTip(floating ? tr("Drag to move the window, double-click to dock")
                                    : tr("Double-click or drag out to open in its own window"));
}

void KeyframesPanel::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    const int index = m_viewCombo->findData(int(mode));
    if (index >= 0 && m_viewCombo->currentIndex() != index)
        m_viewCombo->setCurrentIndex(index);
    applyView();
}

void KeyframesPanel::setFreeMode()
{
    if (m_viewMode != ViewMode::Free)
        setViewMode(ViewMode::Free);
}

void KeyframesPanel::setView(View view)
{
    KeyframeViewBase* current = m_view == View::Curves ? static_cast<KeyframeViewBase*>(m_curves)
                                                       : static_cast<KeyframeViewBase*>(m_dope);
    if (view != m_view && current->dragActive()) {
        // A shortcut in mid-drag: hiding the view would strand its mouse grab and open edit.
        (m_view == View::Curves ? m_curvesViewButton : m_keyframesViewButton)->setChecked(true);
        return;
    }
    KeyframeViewBase* next = view == View::Curves ? static_cast<KeyframeViewBase*>(m_curves)
                                                  : static_cast<KeyframeViewBase*>(m_dope);
    const bool hadFocus = current->hasFocus();
    m_view = view;
    (view == View::Curves ? m_curvesViewButton : m_keyframesViewButton)->setChecked(true);
    m_viewStack->setCurrentWidget(next);
    if (hadFocus)
        next->setFocus(); // Delete and Ctrl+C keep acting on the selected keys
    // These two act on the value axis, which only the curves have.
    m_toolActions.value(m_normalisedButton)->setVisible(view == View::Curves);
    m_toolActions.value(m_fitSelectionButton)->setVisible(view == View::Curves);
}

void KeyframesPanel::setNormalised(bool normalised)
{
    m_normalisedButton->setChecked(normalised); // toggled() switches the curve view
}

void KeyframesPanel::setShowAllParameters(bool all)
{
    if (m_showAllButton->isChecked() != all)
        m_showAllButton->setChecked(all); // re-enters through toggled()
    if (m_showAll == all)
        return;
    m_showAll = all;
    rebuild();
}

// ---- Model ----

void KeyframesPanel::rebuild()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    if (m_session) {
        // A hidden panel (the page's toggle is off) skips the work; showEvent() rebuilds.
        if (!isVisible())
            return;
        QVector<quint64> nodes;
        if (m_selOnly) {
            nodes = m_session->selectedNodes();
            if (nodes.isEmpty())
                nodes = {0}; // matches no node: Sel with nothing selected lists nothing
        }
        m_model = keyframeitems::buildComp(data, m_session->clipId(), nodes, m_showAll);
    } else {
        m_model = keyframeitems::build(data, m_ctx.selection->clips(), m_showAll);
    }
    const QVector<quint64> clips = m_model.allClips();
    const bool newItems = clips != m_shownClips;
    m_shownClips = clips;

    // Title: the item's name ("+ N more" for a multi-item selection).
    QString name = m_model.firstItemName(data);
    if (m_model.items.size() > 1)
        name = tr("%1 + %n more", nullptr, int(m_model.items.size() - 1)).arg(name);
    if (m_itemLabel->text() != name)
        m_itemLabel->setText(name);
    // Compositing page: without a listed parameter the placeholder explains how to get one.
    const bool noRows = m_session && m_model.entries.size() <= 1;
    m_stack->setCurrentIndex(m_model.items.isEmpty() || noRows ? 0 : 1);

    const QVector<int> rows = keyframeitems::visibleRows(m_model.entries, m_collapsed);
    QVector<KeyRow> keyRows;
    keyRows.reserve(rows.size());
    for (int i : rows) {
        const KeyframeEntry& e = m_model.entries[i];
        keyRows.append({e.kind, e.label, e.params});
    }
    QVector<CurveInfo> curves;
    for (const KeyframeEntry& e : std::as_const(m_model.entries)) {
        if (e.kind != KeyRow::Kind::Param)
            continue;
        const KeyframeEntry* group = nullptr;
        for (const KeyframeEntry& g : std::as_const(m_model.entries)) {
            if (g.key == e.parentKey) {
                group = &g;
                break;
            }
        }
        curves.append({e.params.front(), e.label, group ? group->label : QString(), e.desc.min,
            e.desc.max, e.colorIndex, e.desc.displayNegated});
    }
    m_dope->setClips(clips);
    m_dope->setRows(keyRows);
    m_curves->setClips(clips);
    m_curves->setCurves(curves);
    m_ruler->setClips(clips);
    m_tree->setRows(m_model.entries, rows, m_collapsed);
    syncDopeWindow();
    updateKeyBoxes();
    if (newItems || m_viewMode == ViewMode::FitClip)
        applyView();
}

void KeyframesPanel::applyRegionColors()
{
    QPalette pal = m_treeHeader->palette();
    const SchemeColors& sc = schemeColors();
    pal.setColor(QPalette::Window, sc.keyRuler);
    m_treeHeader->setPalette(pal);
}

void KeyframesPanel::syncDopeWindow()
{
    m_dope->setVerticalWindow(m_tree->verticalScrollBar()->value(), m_dope->contentHeight());
}

// ---- Time view ----

double KeyframesPanel::dataWidth() const
{
    return std::max(50.0, double(m_right->width() - kAxisWidth));
}

void KeyframesPanel::fitFrames(double first, double last)
{
    const double span = std::max(1.0, last - first);
    m_map.ppf = std::clamp(dataWidth() / (span * (1.0 + 2.0 * kFitMargin)), kMinPpf, kMaxPpf);
    m_map.firstFrame = first - span * kFitMargin;
    // Centre when the zoom limit keeps the range from filling the width.
    const double shown = dataWidth() / m_map.ppf;
    if (shown > span * (1.0 + 2.0 * kFitMargin))
        m_map.firstFrame = (first + last) / 2.0 - shown / 2.0;
    updateScrollBar();
    m_ruler->update();
    m_dope->update();
    m_curves->update();
}

void KeyframesPanel::applyView()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    if (m_viewMode == ViewMode::FitClip) {
        std::optional<std::pair<int64_t, int64_t>> range;
        for (quint64 id : std::as_const(m_shownClips)) {
            if (const ClipData* c = editops::clipById(data, id)) {
                range = range ? std::pair {std::min(range->first, c->start),
                                    std::max(range->second, c->start + c->duration)}
                              : std::pair {c->start, c->start + c->duration};
            }
        }
        if (range) {
            fitFrames(double(range->first), double(range->second));
            return;
        }
    } else if (m_viewMode == ViewMode::FollowTimeline && m_timelineLast > m_timelineFirst) {
        m_map.ppf = std::clamp(dataWidth() / (m_timelineLast - m_timelineFirst), kMinPpf, kMaxPpf);
        m_map.firstFrame = m_timelineFirst;
    }
    updateScrollBar();
    m_ruler->update();
    m_dope->update();
    m_curves->update();
}

void KeyframesPanel::updateScrollBar()
{
    // Pixel units: value = first visible frame * ppf. The range covers the timeline and the
    // current view (which may lie beyond the end).
    const TimelineData& data = m_ctx.project->timeline()->data();
    const double page = dataWidth();
    const double content
        = std::max<double>(double(data.duration()), m_map.firstFrame + page / m_map.ppf)
        * m_map.ppf;
    m_syncingScroll = true;
    m_hscroll->setRange(0, std::max(0, int(std::ceil(content - page))));
    m_hscroll->setPageStep(std::max(1, int(page)));
    m_hscroll->setSingleStep(std::max(1, int(page / 20)));
    m_hscroll->setValue(int(std::lround(std::max(0.0, m_map.firstFrame * m_map.ppf))));
    m_syncingScroll = false;
}

void KeyframesPanel::setTimelineView(double firstFrame, double lastFrame)
{
    m_timelineFirst = firstFrame;
    m_timelineLast = lastFrame;
    if (m_viewMode == ViewMode::FollowTimeline)
        applyView();
}

void KeyframesPanel::zoomTime(double factor, double anchorX)
{
    setFreeMode();
    const double anchorFrame = m_map.xToFrame(anchorX);
    m_map.ppf = std::clamp(m_map.ppf * factor, kMinPpf, kMaxPpf);
    m_map.firstFrame = anchorFrame - (anchorX - m_map.originX) / m_map.ppf;
    applyView();
}

void KeyframesPanel::scrollTimePx(double dx)
{
    setFreeMode();
    m_map.firstFrame += dx / m_map.ppf;
    applyView();
}

void KeyframesPanel::onPlayheadChanged()
{
    m_tree->refresh();
    // Free / follow modes page-scroll when the playhead leaves the view (not while the user
    // scrubs the panel's own ruler: the frame under the pointer must stay put).
    if (m_viewMode == ViewMode::FitClip || m_ruler->isScrubbing() || m_shownClips.isEmpty())
        return;
    const double x = m_map.frameToX(double(m_ctx.project->timeline()->playhead()));
    if (x < m_map.originX || x > m_map.originX + dataWidth()) {
        if (m_viewMode == ViewMode::FollowTimeline)
            return; // the timeline scrolls and the panel follows it
        m_map.firstFrame
            = double(m_ctx.project->timeline()->playhead()) - dataWidth() * 0.1 / m_map.ppf;
        applyView();
    }
}

void KeyframesPanel::resizeEvent(QResizeEvent* event)
{
    PanelFrame::resizeEvent(event);
    applyView();
}

bool KeyframesPanel::headerEvent(QEvent* event)
{
    // Only the bar itself: clicks on its buttons / labels reach those children first (a label
    // passes them on, which is fine: it is part of the header).
    switch (event->type()) {
    case QEvent::MouseButtonDblClick: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton || m_session)
            return false;
        m_headerArmed = false;
        emit floatToggleRequested();
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        m_headerArmed = mouse->button() == Qt::LeftButton && !m_session;
        m_headerPressGlobal = mouse->globalPosition().toPoint();
        return false;
    }
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (!m_headerArmed || !(mouse->buttons() & Qt::LeftButton))
            return false;
        const QPoint global = mouse->globalPosition().toPoint();
        if (m_floating) {
            // Move the floating window with the compositor's interactive move: works on
            // Wayland (xdg_toplevel.move with the press serial) and X11 (_NET_WM_MOVERESIZE);
            // a client cannot position its own top-level on Wayland.
            if ((global - m_headerPressGlobal).manhattanLength()
                < QApplication::startDragDistance())
                return false;
            m_headerArmed = false;
            if (QWindow* handle = window()->windowHandle())
                handle->startSystemMove();
            return true;
        }
        // Docked: tear off once the pointer leaves the panel by kTearOffDistancePx (a short
        // drag inside the header does nothing, so a sloppy click never undocks).
        const QRect panel(mapToGlobal(QPoint(0, 0)), size());
        if (panel
                .marginsAdded(QMargins(
                    kTearOffDistancePx, kTearOffDistancePx, kTearOffDistancePx, kTearOffDistancePx))
                .contains(global))
            return false;
        m_headerArmed = false;
        emit tearOffRequested();
        return true;
    }
    case QEvent::MouseButtonRelease:
        m_headerArmed = false;
        return false;
    default:
        return false;
    }
}

bool KeyframesPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == titleBar() && headerEvent(event))
        return true;
    if (watched == m_right && event->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        const int delta = we->angleDelta().y() != 0 ? we->angleDelta().y() : we->angleDelta().x();
        const double steps = delta / 120.0;
        const QPointF pos = we->position();
        const Qt::KeyboardModifiers mods = we->modifiers();
        if (mods & (Qt::ControlModifier | Qt::AltModifier)) {
            zoomTime(std::pow(kWheelZoomPerStep, steps), pos.x());
        } else if (mods & Qt::ShiftModifier || we->angleDelta().x() != 0) {
            scrollTimePx(-steps * kWheelScrollPx);
        } else if (m_dope->isVisible()
            && m_dope->geometry().contains(
                m_dope->parentWidget()->mapFrom(m_right, pos.toPoint()))) {
            QScrollBar* v = m_tree->verticalScrollBar();
            v->setValue(v->value() - int(steps * KeyframeParamTree::kRowHeight * 3));
        } else if (m_curves->isVisible()) {
            m_curves->panValue(steps * kWheelScrollPx);
        }
        we->accept();
        return true;
    }
    return PanelFrame::eventFilter(watched, event);
}

// ---- Persistence ----

void KeyframesPanel::saveState(QSettings& settings, const QString& prefix) const
{
    settings.setValue(prefix + QStringLiteral("split"), m_split->saveState());
    settings.setValue(prefix + QStringLiteral("view"),
        m_view == View::Curves ? QStringLiteral("curves") : QStringLiteral("keyframes"));
    // Keys of the stacked layout (before 15.22): restoreState() has read them by now.
    for (const char* old : {"viewSplit", "dopeSheet", "curves"})
        settings.remove(prefix + QLatin1String(old));
    settings.setValue(prefix + QStringLiteral("normalised"), m_normalisedButton->isChecked());
    settings.setValue(prefix + QStringLiteral("showAll"), m_showAll);
    settings.setValue(prefix + QStringLiteral("viewMode"), int(m_viewMode));
}

void KeyframesPanel::restoreState(const QSettings& settings, const QString& prefix)
{
    auto value = [&](const char* key, const QVariant& fallback) {
        return settings.value(prefix + QLatin1String(key), fallback);
    };
    if (settings.contains(prefix + QStringLiteral("split")))
        m_split->restoreState(value("split", {}).toByteArray());
    // A layout stored before 15.22 showed curves alone only if the user had turned the dope
    // sheet off; its "viewSplit" sizes have no splitter to go to any more and are ignored.
    const bool oldCurvesOnly = !value("dopeSheet", true).toBool() && value("curves", true).toBool();
    const QString view = value("view", QString()).toString();
    setView(view == QLatin1String("curves") || (view.isEmpty() && oldCurvesOnly) ? View::Curves
                                                                                 : View::Keyframes);
    m_normalisedButton->setChecked(value("normalised", true).toBool());
    setShowAllParameters(value("showAll", false).toBool());
    const int mode = value("viewMode", int(ViewMode::FitClip)).toInt();
    setViewMode(mode >= 0 && mode <= int(ViewMode::Free) ? ViewMode(mode) : ViewMode::FitClip);
}

} // namespace kk
