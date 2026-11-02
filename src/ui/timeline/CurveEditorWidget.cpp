// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditorWidget implementation: curve list of one clip, header column list and tool bar.
#include "ui/timeline/CurveEditorWidget.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/timeline/TimelineLayout.h"

#include <QPainter>
#include <QToolButton>

#include <cmath>

namespace kk {

using namespace timelinemetrics;
namespace ko = keyframeops;

namespace {
constexpr int kListRowHeight = 16; ///< parameter list rows in the header column
constexpr int kListTop = 4;
constexpr int kButtonSize = 22; ///< tool bar buttons (16 px icons)
constexpr int kButtonBottomGap = 4;
} // namespace

CurveEditorWidget::CurveEditorWidget(
    AppContext& ctx, const KeyframeTimeMap& map, quint64 clipId, QWidget* parent)
    : CurveView(ctx, map, parent)
    , m_clipId(clipId)
{
    setClips({clipId});
    setLabelWidth(kHeaderWidth);
    struct Tool
    {
        const char* icon;
        const char* tip;
        std::optional<Interp> interp;
        int special; ///< 1 = flatten, 2 = fit
    };
    const Tool tools[] = {
        {"interp_linear", QT_TR_NOOP("Linear"), Interp::Linear, 0},
        {"interp_smooth", QT_TR_NOOP("Smooth (Bezier)"), Interp::Smooth, 0},
        {"interp_ease_in", QT_TR_NOOP("Ease In"), Interp::EaseIn, 0},
        {"interp_ease_out", QT_TR_NOOP("Ease Out"), Interp::EaseOut, 0},
        {"interp_ease_in_out", QT_TR_NOOP("Ease In and Out"), Interp::EaseInOut, 0},
        {"interp_hold", QT_TR_NOOP("Static (Hold)"), Interp::Hold, 0},
        {"interp_flatten", QT_TR_NOOP("Flatten Handles"), std::nullopt, 1},
        {"curve_fit", QT_TR_NOOP("Fit Curves"), std::nullopt, 2},
    };
    for (const Tool& t : tools) {
        auto* b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setIcon(kk::icon(QLatin1String(t.icon)));
        b->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
        b->setFixedSize(kButtonSize, kButtonSize);
        b->setToolTip(tr(t.tip));
        b->setObjectName(QLatin1String(t.icon));
        b->setFocusPolicy(Qt::NoFocus);
        const std::optional<Interp> interp = t.interp;
        const int special = t.special;
        connect(b, &QToolButton::clicked, this, [this, interp, special] {
            if (interp)
                keyframecommands::setSelectedInterp(m_ctx, *interp);
            else if (special == 1)
                keyframecommands::flattenSelected(m_ctx);
            else
                fitView();
        });
        m_buttons.append(b);
    }
    connect(ctx.project->timeline(), &Timeline::changed, this, &CurveEditorWidget::refreshCurves);
    refreshCurves();
}

void CurveEditorWidget::refreshCurves()
{
    const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), m_clipId);
    const QVector<ko::AnimatedParam> params
        = c ? ko::animatedParams(*c) : QVector<ko::AnimatedParam> {};
    // Only reassign when the parameter set changed (a drag notifies on every mouse move).
    bool same = params.size() == curves().size();
    for (qsizetype i = 0; same && i < params.size(); ++i)
        same = params[i].path == curves()[i].id.path;
    if (same)
        return;
    QVector<CurveInfo> list;
    list.reserve(params.size());
    for (qsizetype i = 0; i < params.size(); ++i) {
        const ko::AnimatedParam& p = params[i];
        list.append({{m_clipId, p.path}, p.label, p.groupName, p.min, p.max, int(i)});
    }
    setCurves(list);
}

void CurveEditorWidget::placeLane(const QRect& lane, int minTop)
{
    const QRect visible = lane.intersected(QRect(lane.left(), minTop, lane.width(), 1 << 20));
    if (visible.isEmpty()) {
        hide();
        return;
    }
    setGeometry(visible);
    setVerticalWindow(visible.top() - lane.top(), lane.height());
    layoutButtons();
    show();
}

QVector<QString> CurveEditorWidget::shownPaths() const
{
    QVector<QString> out;
    for (const ParamId& id : shownCurves())
        out.append(id.path);
    return out;
}

void CurveEditorWidget::setCurveShown(const QString& path, bool shown)
{
    setCurveShown(ParamId {m_clipId, path}, shown);
}

QPointF CurveEditorWidget::keyPos(const QString& path, int64_t localFrame) const
{
    return keyPosAt({m_clipId, path}, localFrame);
}

QPointF CurveEditorWidget::handlePos(
    const QString& path, int64_t localFrame, ko::HandleSide side) const
{
    return handlePosAt({m_clipId, path}, localFrame, side);
}

void CurveEditorWidget::resizeEvent(QResizeEvent* event)
{
    CurveView::resizeEvent(event);
    layoutButtons();
}

void CurveEditorWidget::layoutButtons()
{
    // Tool bar along the bottom of the header column (logical y, shifted by the visible cut).
    const int y = laneHeight() - kButtonSize - kButtonBottomGap - yOffset();
    int x = metrics::kSpaceS;
    for (QToolButton* b : std::as_const(m_buttons)) {
        b->move(x, y);
        b->setVisible(y >= 0);
        x += kButtonSize + 1;
    }
}

void CurveEditorWidget::paintLabelColumn(QPainter& p)
{
    // Parameter list above the tool bar (logical coordinates).
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    const QFont small = p.font();
    const int listBottom = laneHeight() - kButtonSize - kButtonBottomGap - 2;
    const QVector<CurveInfo>& all = curves();
    for (int i = 0; i < all.size(); ++i) {
        const int y = kListTop + i * kListRowHeight;
        if (y + kListRowHeight > listBottom)
            break;
        const bool shown = !isHiddenIndex(i);
        const QColor col = curveColor(all[i].colorIndex);
        const QRect box(8, y + 4, 9, 9);
        p.setPen(col);
        p.setBrush(shown ? col : Qt::NoBrush);
        p.drawRect(box);
        p.setBrush(Qt::NoBrush);
        QFont f = small;
        f.setBold(all[i].id == activeCurve());
        p.setFont(f);
        p.setPen(shown ? text : dim);
        const QString label = all[i].groupName + QStringLiteral(" / ") + all[i].label;
        p.drawText(QRect(22, y, kHeaderWidth - 28, kListRowHeight), Qt::AlignVCenter,
            p.fontMetrics().elidedText(label, Qt::ElideRight, kHeaderWidth - 28));
    }
    p.setFont(small);
}

bool CurveEditorWidget::pressLabelColumn(QPointF pos)
{
    const double logicalY = pos.y() + yOffset();
    const int row = int(std::floor((logicalY - kListTop) / kListRowHeight));
    const int listBottom = laneHeight() - kButtonSize - kButtonBottomGap - 2;
    if (row < 0 || row >= curves().size() || logicalY >= listBottom)
        return false;
    const ParamId id = curves()[row].id;
    setCurveShown(id, !isCurveShown(id));
    return true;
}

} // namespace kk
