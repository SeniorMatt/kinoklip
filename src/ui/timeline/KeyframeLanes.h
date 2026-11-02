// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeLanes — the per-clip keyframe editor and curve editor that expand below a clip on the
// timeline: open/closed state, lane heights for TimelineLayout, lane widget placement, the two
// toggle buttons painted on animated clips, and the keyframe actions.
#pragma once

#include "core/Animation.h"
#include "core/KeyframeOps.h"
#include "core/SelectionModel.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>

#include <cstdint>

class QPainter;
class QToolButton;
class QWidget;

namespace kk {

struct AppContext;
struct ClipData;
struct TimelineData;
class TimelineController;
class TimelineLayout;
class KeyframeTrackWidget;
class CurveEditorWidget;
class KeyframeViewBase;

/// Owns the expanded keyframe / curve editors of a TimelineWidget.
///
/// Behaviour (docs/behavior-spec.md 15.8 - 15.13): every animated clip shows two small toggle
/// buttons at its bottom-right corner (keyframes, curves). Opening one grows the clip's track
/// by a lane below the clips; the lane spans the whole row (other clips of the track show empty
/// lane) and holds a KeyframeTrackWidget and/or a CurveEditorWidget. At most one clip per track
/// is expanded: opening a second clip on the same track closes the first. The state is view
/// state (not saved in the project) and survives edits as long as the clip exists.
///
/// The lane widgets are children of the timeline viewport, positioned over the lane rects of
/// TimelineLayout (clipped below the ruler), so they take their own input without touching the
/// timeline's mouse state machine. They are the shared DopeSheetView / CurveView of
/// src/ui/keyframes on a time map that goes through TimelineLayout.
///
/// A clip narrower than kMinUsableClipWidthPx on screen is too cramped to edit keys in the lane
/// (owner feedback alpha2-12.png): its lane then shows an "Open in Keyframes Panel" button next
/// to the clip; the lane menus always have that entry (openInPanelRequested()).
/// Thread-safety: GUI thread only.
class KeyframeLanes : public QObject
{
    Q_OBJECT
public:
    /// Creates the manager for the timeline view @p viewport (lane widgets become its
    /// children). @p layout and @p controller must outlive it.
    KeyframeLanes(AppContext& ctx, const TimelineLayout& layout, TimelineController* controller,
        QWidget* viewport, QObject* parent = nullptr);
    ~KeyframeLanes() override;

    /// Which editors of a clip are open.
    struct ClipState
    {
        bool keyframes = false;
        bool curves = false;
        int curveHeight = 0; ///< 0 = default
        quint64 order = 0; ///< opening order (newer wins on a shared track)
    };

    /// True if the keyframe editor of @p clipId is open.
    [[nodiscard]] bool isKeyframeEditorOpen(quint64 clipId) const;
    /// True if the curve editor of @p clipId is open.
    [[nodiscard]] bool isCurveEditorOpen(quint64 clipId) const;
    /// Opens / closes the keyframe editor of @p clipId.
    void setKeyframeEditorOpen(quint64 clipId, bool open);
    /// Opens / closes the curve editor of @p clipId.
    void setCurveEditorOpen(quint64 clipId, bool open);

    /// Below this on-screen clip width the lane offers the Keyframes panel.
    static constexpr int kMinUsableClipWidthPx = 160;
    /// The "Open in Keyframes Panel" button of @p clipId's lane (tests), or nullptr.
    [[nodiscard]] QToolButton* openPanelButton(quint64 clipId) const;

    /// Lane heights per track id for TimelineLayout::setLaneHeights() (drops states of clips
    /// that no longer exist).
    [[nodiscard]] QHash<quint64, int> laneHeights(const TimelineData& data);
    /// Places, shows and hides the lane widgets after a layout / scroll / zoom change.
    void syncGeometry();
    /// Closes every lane (another timeline became active: clip ids mean other clips there).
    void reset();

    /// Paints the toggle buttons on every visible animated clip (after the timeline painter).
    void paintClipButtons(QPainter& p) const;
    /// Handles a left click at viewport @p pos: true if it hit a clip toggle button.
    bool clickClipButton(QPointF pos);

    /// The keyframe editor widget of @p clipId (tests), or nullptr.
    [[nodiscard]] KeyframeTrackWidget* keyframeWidget(quint64 clipId) const;
    /// The curve editor widget of @p clipId (tests), or nullptr.
    [[nodiscard]] CurveEditorWidget* curveWidget(quint64 clipId) const;

    /// Toggle button rects of @p clip in timeline row @p row: keyframes (first) and curves.
    /// Empty when the clip is too small to show them.
    [[nodiscard]] std::pair<QRect, QRect> clipButtonRects(int row, const ClipData& clip) const;

    // ---- Keyframe commands (also used by the lane widgets; each is one undo step) ----

    /// Deletes the selected keyframes. Returns true if any was deleted.
    bool deleteSelectedKeys();
    /// Copies the selected keyframes (of the first selected key's clip).
    bool copySelectedKeys();
    /// True if keyframes were copied (the clipboard is shared with the Keyframes panel).
    [[nodiscard]] bool hasCopiedKeys() const;
    /// Pastes copied keyframes into the current clip at the playhead.
    bool pasteKeys();
    /// Applies preset @p interp to the selected keys.
    bool setSelectedInterp(Interp interp);
    /// Flattens the handles of the selected keys.
    bool flattenSelected();

signals:
    /// Lane heights changed: the timeline must rebuild its layout (and call syncGeometry()).
    void layoutChanged();
    /// The user asked to edit the keys of @p clipId in the Keyframes panel (the page selects
    /// the clip and shows the panel).
    void openInPanelRequested(quint64 clipId);

private:
    struct Lane
    {
        QPointer<KeyframeTrackWidget> keys;
        QPointer<CurveEditorWidget> curves;
        QPointer<QToolButton> openPanel;
    };
    /// KeyframeTimeMap through TimelineLayout: lane widget x equals viewport x.
    class LayoutTimeMap : public KeyframeTimeMap
    {
    public:
        explicit LayoutTimeMap(const TimelineLayout& layout)
            : m_layout(layout)
        { }
        [[nodiscard]] double frameToX(double frame) const override;
        [[nodiscard]] double xToFrame(double x) const override;
        [[nodiscard]] double pixelsPerFrame() const override;

    private:
        const TimelineLayout& m_layout;
    };
    void placeOpenPanelButton(
        Lane& lane, quint64 clipId, const ClipData& clip, const QRect& laneRect);
    void setupLaneWidget(KeyframeViewBase* w, quint64 clipId);
    [[nodiscard]] int keyTrackHeight(const ClipData& clip) const;
    [[nodiscard]] int curveHeight(const ClipState& s) const;
    void setOpen(quint64 clipId, bool keyframes, bool open);
    void toggleForSelection(bool keyframes);
    void dropLane(quint64 clipId);

    AppContext& m_ctx;
    const TimelineLayout& m_layout;
    TimelineController* m_controller;
    QWidget* m_viewport;
    QHash<quint64, ClipState> m_states;
    QHash<quint64, Lane> m_lanes;
    quint64 m_order = 0;
    LayoutTimeMap m_timeMap;
};

} // namespace kk
