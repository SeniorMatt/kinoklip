// SPDX-License-Identifier: GPL-3.0-or-later
// DopeSheetView — rows of keyframe diamonds (master, group and parameter rows) with click /
// rubber-band selection and time drags. The inline keyframe lane and the Keyframes panel's
// dope sheet are both this view.
#pragma once

#include "ui/keyframes/KeyframeViewBase.h"

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

namespace kk {

/// One row of a dope sheet.
struct KeyRow
{
    enum class Kind {
        Master, ///< every key of the shown clips
        Group, ///< summary of an inspector group (Transform, Cropping, an effect, ...)
        Param, ///< one parameter
        Item ///< a clip / linked pair header in a multi-item view (summary of its keys)
    };
    Kind kind = Kind::Param;
    QString label;
    QVector<ParamId> params; ///< parameters summarised by the row (one for Param)
};

/// Dope sheet (spec 15.9): every row shows the keys of its parameters as diamonds; master,
/// item and group rows merge the keys of all their parameters per frame.
///
/// Mouse: click a diamond to select (Shift adds, Ctrl toggles; a summary diamond selects every
/// key at that frame in its rows), drag to move the selection in time (whole frames, snapping
/// to the playhead and the clip edges when snapping is on; clamped to the clips), rubber-band
/// on empty space, double-click jumps the playhead to the key, right-click opens the
/// interpolation menu. With a label column (labelWidth() > 0) the row labels are drawn there
/// and a click on a label selects every key of the row.
/// Thread-safety: GUI thread only.
class DopeSheetView : public KeyframeViewBase
{
    Q_OBJECT
public:
    using KeyframeViewBase::KeyframeViewBase;

    /// Replaces the rows (repaints).
    void setRows(const QVector<KeyRow>& rows);
    /// Current rows.
    [[nodiscard]] const QVector<KeyRow>& rows() const { return m_rows; }
    /// Row height in px (default 18).
    void setRowHeight(int px);
    [[nodiscard]] int rowHeight() const { return m_rowHeight; }
    /// Draws the row labels in the label column (default true; the panel has its own tree).
    void setDrawLabels(bool on);
    /// Logical height of the rows (+ padding).
    [[nodiscard]] int contentHeight() const;
    /// Widget-space centre of the diamond of row @p row at timeline frame @p frame (tests).
    [[nodiscard]] QPoint diamondPosAt(int row, int64_t frame) const;

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
        std::optional<int64_t> frame; ///< diamond hit (timeline frame)
    };
    /// One diamond of a row: timeline frame plus flags merged over the row's parameters.
    struct Mark
    {
        int64_t frame = 0;
        bool outside = true; ///< every key at this frame lies outside its clip
    };
    [[nodiscard]] Hit hitTest(QPoint pos) const;
    [[nodiscard]] int rowAtY(int widgetY) const;
    /// Diamonds of @p row in frame order (fills the reused buffer @p out).
    void rowMarks(const KeyRow& row, QVector<Mark>& out) const;
    /// Every key of @p row at any frame.
    [[nodiscard]] QVector<KeyframeRef> rowKeys(const KeyRow& row) const;

    QVector<KeyRow> m_rows;
    int m_rowHeight = 18;
    bool m_drawLabels = true;
    mutable QVector<Mark> m_marks; ///< reused paint buffer

    enum class Mode {
        None,
        Pending, ///< pressed on a diamond, waiting for the drag threshold
        Move,
        RubberBand
    };
    Mode m_mode = Mode::None;
    QPoint m_pressPos;
    int64_t m_pressFrame = 0; ///< timeline frame of the grabbed diamond
    QVector<KeyframeRef> m_dragKeys; ///< selection at drag start
    QVector<KeyframeRef> m_baseSelection; ///< rubber band with Shift/Ctrl keeps these
    QRect m_band; ///< rubber band, widget coordinates
};

} // namespace kk
