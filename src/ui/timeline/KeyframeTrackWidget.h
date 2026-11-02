// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeTrackWidget — the keyframe editor lane below a clip on the timeline: a DopeSheetView
// of one clip (master, group and parameter rows) placed over the clip's lane.
#pragma once

#include "ui/keyframes/DopeSheetView.h"

#include <QRect>

namespace kk {

/// Keyframe editor lane of one clip (spec 15.8 / 15.9).
///
/// Rows: "Keyframes" (master: every key of the clip), then for each inspector group (Transform,
/// Cropping, Title, Audio, each effect) a group row summarising its parameters, followed by
/// one row per animated parameter. Labels are drawn in the track header column. The editing
/// behaviour is DopeSheetView's (shared with the Keyframes panel).
/// Thread-safety: GUI thread only.
class KeyframeTrackWidget : public DopeSheetView
{
    Q_OBJECT
public:
    /// Lane of clip @p clipId on the timeline time map @p map, child of the timeline viewport.
    KeyframeTrackWidget(
        AppContext& ctx, const KeyframeTimeMap& map, quint64 clipId, QWidget* parent);

    /// Height of one row in px.
    static constexpr int kRowHeight = 18;
    /// Lane height needed for @p clip (rows + padding).
    [[nodiscard]] static int heightFor(const ClipData& clip);
    /// Rows of @p clip: master, then group and parameter rows in inspector order.
    [[nodiscard]] static QVector<KeyRow> rowsFor(quint64 clipId, const ClipData& clip);

    /// The clip whose keys this lane edits.
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Places the lane at logical viewport rect @p lane, cut to the area below @p minTop.
    void placeLane(const QRect& lane, int minTop);
    /// Logical lane height (independent of the visible cut).
    [[nodiscard]] int laneHeight() const { return logicalHeight(); }
    /// Widget-space centre of the diamond of row @p row at clip-local @p localFrame (tests).
    [[nodiscard]] QPoint diamondPos(int row, int64_t localFrame) const;

private:
    void refreshRows();

    quint64 m_clipId = 0;
};

} // namespace kk
