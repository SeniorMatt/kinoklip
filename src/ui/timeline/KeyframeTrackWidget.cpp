// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeTrackWidget implementation: rows of one clip and lane placement.
#include "ui/timeline/KeyframeTrackWidget.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/keyframes/KeyframeTimeMap.h"
#include "ui/timeline/TimelineLayout.h"

#include <cmath>

namespace kk {

namespace ko = keyframeops;

KeyframeTrackWidget::KeyframeTrackWidget(
    AppContext& ctx, const KeyframeTimeMap& map, quint64 clipId, QWidget* parent)
    : DopeSheetView(ctx, map, parent)
    , m_clipId(clipId)
{
    setClips({clipId});
    setLabelWidth(timelinemetrics::kHeaderWidth);
    setRowHeight(kRowHeight);
    connect(ctx.project->timeline(), &Timeline::changed, this, &KeyframeTrackWidget::refreshRows);
    refreshRows();
}

int KeyframeTrackWidget::heightFor(const ClipData& clip)
{
    return int(rowsFor(0, clip).size()) * kRowHeight + 2;
}

QVector<KeyRow> KeyframeTrackWidget::rowsFor(quint64 clipId, const ClipData& clip)
{
    QVector<KeyRow> out;
    const QVector<ko::AnimatedParam> params = ko::animatedParams(clip);
    KeyRow master {KeyRow::Kind::Master, tr("Keyframes"), {}};
    for (const ko::AnimatedParam& p : params)
        master.params.append({clipId, p.path});
    out.append(master);
    QString last;
    for (const ko::AnimatedParam& p : params) {
        if (p.groupId != last) {
            last = p.groupId;
            KeyRow g {KeyRow::Kind::Group, p.groupName, {}};
            for (const ko::AnimatedParam& q : params) {
                if (q.groupId == p.groupId)
                    g.params.append({clipId, q.path});
            }
            out.append(g);
        }
        out.append(KeyRow {KeyRow::Kind::Param, p.label, {{clipId, p.path}}});
    }
    return out;
}

void KeyframeTrackWidget::refreshRows()
{
    const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), m_clipId);
    // Rows only change with the set of animated parameters; comparing first keeps a drag
    // (one timeline change per mouse move) from re-assigning them.
    QVector<KeyRow> fresh = c ? rowsFor(m_clipId, *c) : QVector<KeyRow> {};
    const QVector<KeyRow>& current = rows();
    bool same = fresh.size() == current.size();
    for (qsizetype i = 0; same && i < fresh.size(); ++i)
        same = fresh[i].label == current[i].label && fresh[i].params == current[i].params;
    if (!same)
        setRows(fresh);
}

void KeyframeTrackWidget::placeLane(const QRect& lane, int minTop)
{
    const QRect visible = lane.intersected(QRect(lane.left(), minTop, lane.width(), 1 << 20));
    if (visible.isEmpty()) {
        hide();
        return;
    }
    setGeometry(visible);
    setVerticalWindow(visible.top() - lane.top(), lane.height());
    show();
}

QPoint KeyframeTrackWidget::diamondPos(int row, int64_t localFrame) const
{
    const ClipData* c = primaryClip();
    return diamondPosAt(row, (c ? c->start : 0) + localFrame);
}

} // namespace kk
