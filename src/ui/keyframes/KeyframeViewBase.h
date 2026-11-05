// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeViewBase — common base of every keyframe view (dope sheet, curve view; inline
// timeline lanes and the Keyframes panel): time mapping, the shown clips, keyframe selection,
// the drag / undo protocol, snapping, the keyframe menu, painting helpers and keyboard.
#pragma once

#include "core/SelectionModel.h"
#include "core/TimelineData.h"

#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>

class QMenu;

namespace kk {

struct AppContext;
class KeyframeTimeMap;

/// One keyframeable parameter of one clip (a curve, a dope-sheet row of one parameter).
struct ParamId
{
    quint64 clipId = 0;
    QString path; ///< keyframeops path, e.g. "transform/posX"

    friend bool operator==(const ParamId&, const ParamId&) = default;
};

/// Base class of the keyframe views. It shows the keys of one or more clips (the inline lanes
/// show one clip, the Keyframes panel a selected item: a clip or a linked video + audio pair,
/// or several items) on the horizontal axis of a KeyframeTimeMap, in timeline frames.
///
/// Geometry: x < labelWidth() is a label / axis column (inline: the timeline's track header
/// column; panel: the value axis), the data area starts there. Vertically the view may show a
/// window of a taller logical content: logical y = widget y + yOffset() (inline lanes cut under
/// the ruler, the panel's dope sheet scrolled with the parameter tree).
///
/// Editing follows the timeline's protocol: a drag copies the timeline at its start, every move
/// restores that copy and re-applies the operation with the total delta, release closes the
/// edit (one undo step), Esc cancels it. Every change goes through Project, so the inspector,
/// the other keyframe views and the viewers update immediately.
/// Thread-safety: GUI thread only.
class KeyframeViewBase : public QWidget
{
    Q_OBJECT
public:
    /// Creates a view on @p map (must outlive the view).
    KeyframeViewBase(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent);
    ~KeyframeViewBase() override;

    /// Sets the clips whose keys are shown (first = primary: selected when the user selects a
    /// key and none of these clips is selected).
    void setClips(const QVector<quint64>& clipIds);
    /// The clips shown.
    [[nodiscard]] const QVector<quint64>& clipIds() const { return m_clips; }
    /// Width of the label / axis column at the left (0 = none).
    void setLabelWidth(int px);
    [[nodiscard]] int labelWidth() const { return m_labelWidth; }
    /// Shows the window [@p yOffset, yOffset + height()) of a logical content of height
    /// @p logicalHeight (0 = the widget height).
    void setVerticalWindow(int yOffset, int logicalHeight);
    [[nodiscard]] int yOffset() const { return m_yOffset; }
    /// Logical content height (the widget height unless setVerticalWindow() set one).
    [[nodiscard]] int logicalHeight() const
    {
        return m_logicalHeight > 0 ? m_logicalHeight : height();
    }
    /// The checked state of the "timeline.snapping" action (false without it).
    [[nodiscard]] static bool snappingAction(const AppContext& ctx);
    /// Panel colours (SchemeColors::keyRows...) instead of the inline lanes' (spec 15.23).
    void setPanelColors(bool on);
    /// An edit gesture is running in this view.
    [[nodiscard]] bool dragActive() const { return m_editOpen; }
    /// Snapping switch (default: snappingAction()).
    void setSnappingProvider(std::function<bool()> provider) { m_snapping = std::move(provider); }
    /// Adds entries at the end of the keyframe context menu (e.g. "Open in Keyframes Panel").
    void setMenuExtender(std::function<void(QMenu&)> extender)
    {
        m_menuExtender = std::move(extender);
    }

    /// Every key of the shown clips at exactly timeline frame @p frame restricted to
    /// @p params (empty = every parameter).
    [[nodiscard]] QVector<KeyframeRef> keysAtFrame(
        int64_t frame, const QVector<ParamId>& params) const;

protected:
    /// The clip @p clipId (current state), or nullptr.
    [[nodiscard]] const ClipData* clipData(quint64 clipId) const;
    /// The primary clip (first of clipIds()), or nullptr.
    [[nodiscard]] const ClipData* primaryClip() const;
    /// The time mapping.
    [[nodiscard]] const KeyframeTimeMap& timeMap() const { return m_map; }
    /// Widget x of clip-local frame @p localFrame of @p clip (fractional allowed).
    [[nodiscard]] double xOfLocal(const ClipData& clip, double localFrame) const;
    /// Fractional timeline frame at widget x.
    [[nodiscard]] double frameAtX(double x) const;
    /// Pixels per timeline frame.
    [[nodiscard]] double pixelsPerFrame() const;

    // ---- Selection ----
    /// Selected keys of the shown clips.
    [[nodiscard]] QVector<KeyframeRef> selectedKeys() const;
    /// True if @p ref is selected.
    [[nodiscard]] bool isSelected(const KeyframeRef& ref) const;
    /// Click selection of @p refs: replace (no modifier; keeps a selection that already holds
    /// all of @p refs so they can be dragged together), Shift adds, Ctrl toggles.
    void clickSelect(const QVector<KeyframeRef>& refs, Qt::KeyboardModifiers modifiers);
    /// Replaces the keyframe selection (selecting the primary clip if none of the shown clips
    /// is selected, so the Inspector follows).
    void setSelection(const QVector<KeyframeRef>& refs);

    // ---- Drag / undo ----
    /// Opens an edit labelled @p label and remembers the timeline state.
    void beginDragEdit(const QString& label);
    /// Restores the state of beginDragEdit() and returns the mutable timeline data.
    [[nodiscard]] TimelineData& restoreDragOrigin();
    /// The remembered state (valid during a drag).
    [[nodiscard]] const TimelineData& dragOrigin() const { return m_origin; }
    /// Live redraw of every view during a drag.
    void notifyDragChanged();
    /// Closes (true) or cancels the open edit.
    void endDragEdit(bool commit);
    [[nodiscard]] bool dragEditOpen() const { return m_editOpen; }

    /// Snaps timeline frame @p frame when snapping is on and a target is within the snap
    /// distance, else returns it. Targets: the playhead, the first / last frame of the shown
    /// clips and, during a drag, editops::snapCandidates() plus the other keys of the shown
    /// clips (spec 15.24).
    [[nodiscard]] int64_t snapFrame(int64_t frame) const;
    /// Moves the playhead to timeline frame @p frame.
    void jumpToFrame(int64_t frame);

    /// The right-click menu: interpolation presets for the selected keys, copy / paste /
    /// delete (spec 15.6, 15.9), then the extender's entries.
    void showKeyMenu(const QPoint& globalPos);

    // ---- Painting helpers (logical coordinates: the painter is translated by -yOffset) ----
    /// Fills the label column and the data area; the spans of the shown clips are lighter and
    /// their edges marked (keys outside a clip were trimmed away and fade).
    void paintBackground(QPainter& p, const QRect& logicalRect) const;
    /// The playhead line across the view.
    void paintPlayhead(QPainter& p) const;
    /// The rubber band (widget coordinates; call with an untranslated painter).
    void paintBand(QPainter& p, const QRectF& band) const;
    /// True when setPanelColors() is on.
    [[nodiscard]] bool panelColors() const { return m_panelColors; }
    /// Draws a keyframe diamond centred at @p c.
    static void drawDiamond(QPainter& p, QPointF c, double radius);

    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    AppContext& m_ctx;

private:
    const KeyframeTimeMap& m_map;
    QVector<quint64> m_clips;
    int m_labelWidth = 0;
    int m_yOffset = 0;
    int m_logicalHeight = 0;
    std::function<bool()> m_snapping;
    std::function<void(QMenu&)> m_menuExtender;
    TimelineData m_origin;
    bool m_editOpen = false;
    std::vector<int64_t> m_dragSnaps; ///< snap targets of the open drag (sorted)
    bool m_panelColors = false;
};

/// Colour helper shared by the keyframe views: linear mix of @p a and @p b.
[[nodiscard]] QColor keyframeMix(const QColor& a, const QColor& b, double t);

} // namespace kk
