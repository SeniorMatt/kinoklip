// SPDX-License-Identifier: GPL-3.0-or-later
// ClipLaneWidget — common base of the keyframe editor and curve editor lanes below a clip:
// geometry (timeline x <-> clip-local frames), keyframe selection, the drag/undo protocol,
// snapping, the keyframe context menu and keyboard handling.
#pragma once

#include "core/SelectionModel.h"
#include "core/TimelineData.h"

#include <QVector>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace kk {

struct AppContext;
class KeyframeLanes;
class TimelineController;
class TimelineLayout;

/// Base class of the lane widgets. The widget always spans the whole width of the timeline
/// viewport, so a widget x equals a viewport x and TimelineLayout maps it to frames directly.
/// Vertically the widget may be cut at the top (lane scrolled under the ruler): logical lane y
/// = widget y + yOffset().
///
/// Editing follows the timeline's protocol: a drag copies the timeline at its start, every move
/// restores that copy and re-applies the operation with the total delta, release closes the
/// edit (one undo step), Esc cancels it.
/// Thread-safety: GUI thread only.
class ClipLaneWidget : public QWidget
{
    Q_OBJECT
public:
    ClipLaneWidget(AppContext& ctx, const TimelineLayout& layout, TimelineController* controller,
        KeyframeLanes* lanes, quint64 clipId, QWidget* parent);
    ~ClipLaneWidget() override;

    /// The clip whose keys this lane edits.
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Places the lane at logical viewport rect @p lane, cut to the area below @p minTop.
    void placeLane(const QRect& lane, int minTop);
    /// Logical lane height (independent of the visible cut).
    [[nodiscard]] int laneHeight() const { return m_laneHeight; }

protected:
    /// The clip, or nullptr if it no longer exists.
    [[nodiscard]] const ClipData* clip() const;
    /// Widget x of clip-local frame @p localFrame (fractional allowed).
    [[nodiscard]] double xOfLocal(double localFrame) const;
    /// Fractional clip-local frame at widget x @p x.
    [[nodiscard]] double localAtX(double x) const;
    /// Logical lane y of widget y @p y.
    [[nodiscard]] int yOffset() const { return m_yOffset; }
    /// Pixels per timeline frame.
    [[nodiscard]] double pixelsPerFrame() const;

    // ---- Selection ----
    /// Selected keys of this clip.
    [[nodiscard]] QVector<KeyframeRef> selectedKeys() const;
    /// True if @p ref is selected.
    [[nodiscard]] bool isSelected(const KeyframeRef& ref) const;
    /// Click selection of @p refs: replace (no modifier; keeps a selection that already holds
    /// all of @p refs so they can be dragged together), Shift adds, Ctrl toggles. Selects the
    /// clip first if needed (the Inspector follows).
    void clickSelect(const QVector<KeyframeRef>& refs, Qt::KeyboardModifiers modifiers);
    /// Replaces the keyframe selection (selecting the clip if needed).
    void setSelection(const QVector<KeyframeRef>& refs);

    // ---- Drag / undo ----
    /// Opens an edit labelled @p label and remembers the timeline state.
    void beginDragEdit(const QString& label);
    /// Restores the state of beginDragEdit() and returns the mutable timeline data.
    [[nodiscard]] TimelineData& restoreDragOrigin();
    /// The remembered state (valid during a drag).
    [[nodiscard]] const TimelineData& dragOrigin() const { return m_origin; }
    /// Live redraw during a drag.
    void notifyDragChanged();
    /// Closes (true) or cancels the open edit.
    void endDragEdit(bool commit);
    [[nodiscard]] bool dragEditOpen() const { return m_editOpen; }

    /// Snaps clip-local frame @p localFrame to the playhead and the clip edges when timeline
    /// snapping is on and one is within the snap tolerance; else returns it unchanged.
    [[nodiscard]] int64_t snapLocal(int64_t localFrame) const;
    /// Moves the playhead to clip-local frame @p localFrame.
    void jumpTo(int64_t localFrame);

    /// The right-click menu: interpolation presets for the selected keys, copy / paste /
    /// delete (spec 15.6, 15.9).
    void showKeyMenu(const QPoint& globalPos);

    // ---- Painting helpers ----
    /// Fills the label column (x < header width) and the lane background.
    void paintLaneBackground(QPainter& p, const QRect& logicalRect) const;
    /// The playhead line across the lane.
    void paintPlayhead(QPainter& p) const;
    /// Draws a keyframe diamond centred at @p c.
    static void drawDiamond(QPainter& p, QPointF c, double radius);

    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    AppContext& m_ctx;
    const TimelineLayout& m_layout;
    TimelineController* m_controller;
    KeyframeLanes* m_lanes;

private:
    quint64 m_clipId = 0;
    int m_yOffset = 0;
    int m_laneHeight = 0;
    TimelineData m_origin;
    bool m_editOpen = false;
};

} // namespace kk
