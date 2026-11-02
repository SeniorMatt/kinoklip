// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeTrackWidget — the keyframe editor lane below a clip: a master row, one row per
// parameter group and one per animated parameter, with selectable, draggable diamonds.
#pragma once

#include "ui/timeline/ClipLaneWidget.h"

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

namespace kk {

/// Keyframe editor of one clip (spec 15.8 / 15.9).
///
/// Rows: "Keyframes" (master: every key of the clip), then for each inspector group (Transform,
/// Cropping, Title, Audio, each effect) a group row summarising its parameters, followed by
/// one row per animated parameter. Labels are drawn in the track header column.
///
/// Mouse: click a diamond to select (Shift adds, Ctrl toggles; a group / master diamond selects
/// every key at that frame in its rows), drag to move the selection in time (whole frames,
/// snapping to the playhead and the clip edges when snapping is on; clamped to the clip),
/// rubber-band on empty space, double-click jumps the playhead to the key, right-click opens
/// the interpolation menu, click on a row label selects every key of the row.
/// Thread-safety: GUI thread only.
class KeyframeTrackWidget : public ClipLaneWidget
{
    Q_OBJECT
public:
    using ClipLaneWidget::ClipLaneWidget;

    /// Height of one row in px.
    static constexpr int kRowHeight = 18;
    /// Lane height needed for @p clip (rows + padding).
    [[nodiscard]] static int heightFor(const ClipData& clip);

    /// One row of the editor.
    struct Row
    {
        enum class Kind {
            Master,
            Group,
            Param
        };
        Kind kind = Kind::Param;
        QString label;
        QVector<QString> paths; ///< parameters summarised by the row (one for Param)
    };
    /// Rows for the current clip state.
    [[nodiscard]] QVector<Row> rows() const;
    /// Widget-space centre of the diamond of row @p row at clip-local @p localFrame (tests).
    [[nodiscard]] QPoint diamondPos(int row, int64_t localFrame) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    struct Hit
    {
        int row = -1;
        std::optional<int64_t> frame; ///< diamond hit
    };
    [[nodiscard]] Hit hitTest(QPoint pos) const;
    [[nodiscard]] int rowAtY(int widgetY) const;
    [[nodiscard]] QVector<KeyframeRef> refsAt(const Row& row, int64_t frame) const;
    [[nodiscard]] QVector<int64_t> rowFrames(const Row& row) const;

    enum class Mode {
        None,
        Pending, ///< pressed on a diamond, waiting for the drag threshold
        Move,
        RubberBand
    };
    Mode m_mode = Mode::None;
    QPoint m_pressPos;
    int64_t m_pressFrame = 0; ///< frame of the grabbed diamond
    QVector<KeyframeRef> m_dragKeys; ///< selection at drag start
    QVector<KeyframeRef> m_baseSelection; ///< rubber band with Shift/Ctrl keeps these
    QRect m_band; ///< rubber band, widget coordinates
};

} // namespace kk
