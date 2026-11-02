// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframesPanel implementation: layout, model refresh, time view, wheel, persistence.
#include "ui/keyframes/KeyframesPanel.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframeRuler.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace kk {

namespace kc = keyframecommands;

namespace {
constexpr int kAxisWidth = 64; ///< value axis column of the curves (ruler / dope sheet align)
constexpr int kTreeWidth = 380; ///< default width of the parameter tree
constexpr double kFitMargin = 0.04; ///< Fit Clip: free space each side, fraction of the span
constexpr double kMinPpf = 0.01; ///< time zoom limits (pixels per frame)
constexpr double kMaxPpf = 80.0;
constexpr double kWheelZoomPerStep = 1.2; ///< time zoom per wheel notch
constexpr double kWheelScrollPx = 60.0; ///< time scroll per wheel notch
constexpr int kToolButtonSize = 24;
} // namespace

KeyframesPanel::KeyframesPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Keyframes"), parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("KeyframesPanel"));
    m_map.originX = kAxisWidth;

    // Title bar: the shown item's name and the float / dock button.
    m_itemLabel = new QLabel;
    m_itemLabel->setObjectName(QStringLiteral("KeyframesItemName"));
    addTitleBarWidget(m_itemLabel);
    m_floatButton = new QToolButton;
    m_floatButton->setObjectName(QStringLiteral("KeyframesFloatButton"));
    m_floatButton->setAutoRaise(true);
    m_floatButton->setFocusPolicy(Qt::NoFocus);
    m_floatButton->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
    connect(m_floatButton, &QToolButton::clicked, this, &KeyframesPanel::floatToggleRequested);
    addTitleBarWidget(m_floatButton);
    setFloating(false);

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
    auto* treeHeader = new QLabel(tr("Parameters"));
    treeHeader->setFixedHeight(m_ruler->sizeHint().height());
    treeHeader->setContentsMargins(metrics::kSpaceM, 0, 0, 0);
    QFont hf = treeHeader->font();
    hf.setBold(true);
    treeHeader->setFont(hf);
    leftLayout->addWidget(treeHeader);
    leftLayout->addWidget(m_tree, 1);

    // Right: ruler, dope sheet over curves, time scroll bar.
    m_right = new QWidget;
    auto* rightLayout = new QVBoxLayout(m_right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    m_dope = new DopeSheetView(ctx, m_map, nullptr);
    m_dope->setLabelWidth(kAxisWidth);
    m_dope->setDrawLabels(false);
    m_dope->setRowHeight(KeyframeParamTree::kRowHeight);
    m_dope->setMinimumHeight(40);
    m_curves = new CurveView(ctx, m_map, nullptr);
    m_curves->setLabelWidth(kAxisWidth);
    m_curves->setAxisLabelsInColumn(true);
    m_curves->setTimeGrid(true);
    m_curves->setMinimumHeight(80);
    m_curves->setTimePanner([this](double dx) { scrollTimePx(-dx); });
    m_viewSplit = new QSplitter(Qt::Vertical);
    m_viewSplit->setChildrenCollapsible(false);
    m_viewSplit->setHandleWidth(3);
    m_viewSplit->addWidget(m_dope);
    m_viewSplit->addWidget(m_curves);
    m_viewSplit->setStretchFactor(1, 2);
    m_hscroll = new QScrollBar(Qt::Horizontal);
    rightLayout->addWidget(m_ruler);
    rightLayout->addWidget(m_viewSplit, 1);
    rightLayout->addWidget(m_hscroll);
    // Wheel events the views ignore propagate here (see eventFilter()).
    m_right->installEventFilter(this);

    m_split = new QSplitter(Qt::Horizontal);
    m_split->setChildrenCollapsible(false);
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
    rebuild();
}

KeyframesPanel::~KeyframesPanel() = default;

QWidget* KeyframesPanel::buildToolRow()
{
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("KeyframesToolRow"));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(metrics::kSpaceS, metrics::kSpaceXs, metrics::kSpaceS, metrics::kSpaceXs);
    h->setSpacing(metrics::kSpaceXs);
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
              h->addWidget(b);
              return b;
          };
    auto separator = [&] { h->addSpacing(metrics::kSpaceL); };

    m_dopeButton = button(QStringLiteral("keyframe_editor"), {}, tr("Dope Sheet"), true);
    m_curvesButton = button(QStringLiteral("curve_editor"), {}, tr("Curve Editor"), true);
    connect(m_dopeButton, &QToolButton::clicked, this,
        [this](bool on) { setViews(on, m_curvesButton->isChecked() || !on); });
    connect(m_curvesButton, &QToolButton::clicked, this,
        [this](bool on) { setViews(m_dopeButton->isChecked() || !on, on); });
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
        &QToolButton::clicked, this, [this] {
            m_curves->fitView();
            setViewMode(ViewMode::FitClip);
        });
    connect(button({}, tr("Fit Selection"), tr("Zoom to the selected keyframes"), false),
        &QToolButton::clicked, this, [this] { m_curves->fitSelection(); });
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

    m_dopeButton->setChecked(true);
    m_curvesButton->setChecked(true);
    return row;
}

// ---- Options ----

void KeyframesPanel::setFloating(bool floating)
{
    m_floating = floating;
    m_floatButton->setIcon(
        kk::icon(floating ? QStringLiteral("window_dock") : QStringLiteral("window_float")));
    m_floatButton->setToolTip(floating ? tr("Dock the Keyframes panel back into the Edit page")
                                       : tr("Open the Keyframes panel in its own window (e.g. on "
                                            "a second monitor)"));
    m_floatButton->setAccessibleName(floating ? tr("Dock") : tr("Float"));
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

void KeyframesPanel::setViews(bool dopeSheet, bool curves)
{
    if (!dopeSheet && !curves)
        curves = true;
    m_dopeButton->setChecked(dopeSheet);
    m_curvesButton->setChecked(curves);
    m_dope->setVisible(dopeSheet);
    m_curves->setVisible(curves);
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
    m_model = keyframeitems::build(data, m_ctx.selection->clips(), m_showAll);
    const QVector<quint64> clips = m_model.allClips();
    const bool newItems = clips != m_shownClips;
    m_shownClips = clips;

    // Title: the item's name ("+ N more" for a multi-item selection).
    QString name = m_model.firstItemName(data);
    if (m_model.items.size() > 1)
        name = tr("%1 + %n more", nullptr, int(m_model.items.size() - 1)).arg(name);
    if (m_itemLabel->text() != name)
        m_itemLabel->setText(name);
    m_stack->setCurrentIndex(m_model.items.isEmpty() ? 0 : 1);

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
            e.desc.max, e.colorIndex});
    }
    m_dope->setClips(clips);
    m_dope->setRows(keyRows);
    m_curves->setClips(clips);
    m_curves->setCurves(curves);
    m_ruler->setClips(clips);
    m_tree->setRows(m_model.entries, rows, m_collapsed);
    syncDopeWindow();
    if (newItems || m_viewMode == ViewMode::FitClip)
        applyView();
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

bool KeyframesPanel::eventFilter(QObject* watched, QEvent* event)
{
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
    settings.setValue(prefix + QStringLiteral("viewSplit"), m_viewSplit->saveState());
    settings.setValue(prefix + QStringLiteral("dopeSheet"), m_dopeButton->isChecked());
    settings.setValue(prefix + QStringLiteral("curves"), m_curvesButton->isChecked());
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
    if (settings.contains(prefix + QStringLiteral("viewSplit")))
        m_viewSplit->restoreState(value("viewSplit", {}).toByteArray());
    setViews(value("dopeSheet", true).toBool(), value("curves", true).toBool());
    m_normalisedButton->setChecked(value("normalised", true).toBool());
    setShowAllParameters(value("showAll", false).toBool());
    const int mode = value("viewMode", int(ViewMode::FitClip)).toInt();
    setViewMode(mode >= 0 && mode <= int(ViewMode::Free) ? ViewMode(mode) : ViewMode::FitClip);
}

} // namespace kk
