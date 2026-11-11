// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget — the interactive, custom-painted timeline: ruler, track headers, clips,
// tools (select / trim / blade), zoom and scroll, drag and drop.
#pragma once

#include "core/EditOps.h"
#include "core/SelectionModel.h"
#include "core/TimelineData.h"
#include "ui/timeline/RetimeBar.h"
#include "ui/timeline/TimelineAnimator.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineMedia.h"
#include "ui/timeline/TimelinePainter.h"

#include <QAbstractScrollArea>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QMenu;
class QMimeData;

namespace kk {

struct AppContext;
class KeyframeLanes;
class DroppedFileProbe;
class NestedPreview;
class TimelineController;

/// Per-view behaviour that differs between pages: the Edit page uses the defaults, the Cut
/// page's lower timeline turns on a centred playhead, ripple edge trims and compact audio.
struct TimelineViewOptions
{
    /// The view scrolls so the playhead stays in the middle of the track area (playback,
    /// navigation, zoom); scrolling the view by hand (wheel) moves the playhead instead. The
    /// horizontal scroll bar is hidden.
    bool centredPlayhead = false;
    /// Selection tool: dragging a clip edge is a ripple trim (later clips follow) instead of a
    /// trim that leaves a gap; the centre of a cut still rolls (Cut page default).
    bool rippleEdgeTrims = false;
    /// Height factor of the audio rows (below 1 = compact audio tracks; the row never gets
    /// smaller than timelinemetrics::kMinTrackHeight).
    double audioHeightScale = 1.0;
    /// Remembers zoom and scroll per timeline in Project::viewState() and restores them on a
    /// tab switch. Only one view per timeline should (the Edit page's).
    bool keepsViewState = true;
    /// Double-clicking a media clip opens its source in the source viewer (the Edit page's
    /// view, spec 24.17; the Cut and Audio pages have no source viewer of their own).
    bool doubleClickOpensSource = true;
    /// Height factor of the video and subtitle rows (below 1 = collapsed, the Audio page).
    double videoHeightScale = 1.0;
    /// The rows start at the top of the view even when they all fit (the Audio page: a thin
    /// strip of collapsed video tracks, then the audio tracks, spec 37.36); false: the V/A
    /// divider sits near the middle of the view (the Edit page).
    bool rowsFromTop = false;
    /// Audio page headers: record arm (disabled), channel format and a level meter in the
    /// headers of audio tracks (the meter reads headerMeter).
    bool audioPageHeaders = false;
    /// Level (dBFS, left and right; mono: equal) of the audio track with the given id, for
    /// the header meters; empty = no meters.
    std::function<std::pair<float, float>(quint64 trackId)> headerMeter;
};

/// The timeline editor view.
///
/// Drawing is delegated to TimelinePainter, geometry to TimelineLayout; this class owns the
/// input state machine (see src/ui/timeline/readme.md) and turns gestures into EditOps.
/// Every gesture is one undo step: the edit opens when a drag starts, the timeline is
/// restored to the press-time state and the operation re-applied on every mouse move (so
/// overwrite semantics stay exact while dragging), and the edit closes on release (Esc
/// cancels it).
///
/// Implementation is split over TimelineWidget.cpp (setup, paint, zoom, scroll, menus),
/// TimelineInput.cpp (mouse state machine) and TimelineDrop.cpp (drag and drop).
/// Thread-safety: GUI thread only.
class TimelineWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    /// Creates the view for @p ctx, driven by @p controller (not owned, must outlive it).
    TimelineWidget(AppContext& ctx, TimelineController* controller, QWidget* parent = nullptr);
    ~TimelineWidget() override;

    [[nodiscard]] TimelineController* controller() const { return m_controller; }
    /// Current geometry (for tests and hit-testing from outside).
    [[nodiscard]] const TimelineLayout& layout() const { return m_layout; }
    /// Display animations of this view (v2.2; tests drive and inspect them).
    [[nodiscard]] TimelineAnimator& animator() { return m_anim; }
    /// The per-clip keyframe and curve editors (KeyframeLanes.h).
    [[nodiscard]] KeyframeLanes* keyframeLanes() const { return m_keyframeLanes; }

    /// Replaces the thumbnail source (default: media::MediaCache). Null = placeholders.
    void setThumbnailProvider(ThumbnailProvider provider);
    /// Replaces the waveform source (default: media::MediaCache). Null = placeholders.
    void setWaveformProvider(WaveformProvider provider);

    /// Horizontal zoom in pixels per frame, keeping the frame under view x @p anchorX fixed.
    void setPixelsPerFrame(double pixelsPerFrame, double anchorX);
    [[nodiscard]] double pixelsPerFrame() const { return m_layout.pixelsPerFrame(); }
    /// Scrolls so that frame @p frame is at the left edge of the track area.
    void scrollToFrame(double frame);

    /// Page-specific behaviour (see TimelineViewOptions); applies at once.
    void setViewOptions(const TimelineViewOptions& options);
    [[nodiscard]] const TimelineViewOptions& viewOptions() const { return m_options; }
    /// Centred-playhead views: scrolls so the playhead is in the middle of the track area.
    void centreOnPlayhead();

    /// When on (default), a playhead that leaves the visible range (playback, keyboard
    /// navigation) scrolls the view by a page so it stays visible.
    void setFollowPlayhead(bool follow) { m_followPlayhead = follow; }
    [[nodiscard]] bool followPlayhead() const { return m_followPlayhead; }

    /// What a double-click on a transition or a click on the fx badge asks the Inspector
    /// to show (spec 13.10).
    enum class InspectTarget {
        Effects,
        Transition
    };
    Q_ENUM(InspectTarget)
    /// The transition (clip + end) drawn at @p pos, if any (TimelineGestures.cpp).
    [[nodiscard]] std::optional<TransitionRef> transitionAt(QPointF pos) const;
    /// The clip whose "fx" badge is at @p pos, or 0.
    [[nodiscard]] quint64 fxBadgeAt(QPointF pos) const;

    /// Visible frame range of the track area (first, last; fractional).
    [[nodiscard]] std::pair<double, double> visibleFrameRange() const;

    /// The retime controls of clip @p clipId as drawn (empty if hidden / not visible).
    [[nodiscard]] retimebar::Geometry retimeGeometry(quint64 clipId) const;
    /// The speed menu or speed point menu currently open (tests), or nullptr.
    [[nodiscard]] QMenu* retimeMenu() const { return m_retimeMenu; }
    /// Opens the speed menu of segment @p segment of @p clipId at viewport @p pos (non-modal).
    void showSpeedMenu(quint64 clipId, int segment, QPoint pos);
    /// Opens the menu of the speed point at clip-local @p localFrame (non-modal).
    void showSpeedPointMenu(quint64 clipId, int64_t localFrame, QPoint pos);

    /// Stacked timelines: called (instead of continuing the move) when a clip move is dragged
    /// out of the view above or below it, with the moved clips and the grab distance in frames
    /// from the earliest one's start; the handler starts the copy drag (TimelineClips MIME).
    /// Null (default) = moves stay inside the view.
    using ClipDragOut = std::function<void(const QVector<quint64>& clips, int64_t grabFrames)>;
    void setClipDragOut(ClipDragOut handler) { m_clipDragOut = std::move(handler); }
    /// The offset label shown next to the pointer during a move / trim (empty otherwise).
    [[nodiscard]] const DragInfoLabel& dragInfo() const { return m_overlay.dragInfo; }
    /// The ghost clips of the drag over the timeline (tests).
    [[nodiscard]] const std::vector<TimelineGhost>& dropGhosts() const { return m_overlay.ghosts; }
    /// The frames on both sides of the edit being trimmed (inactive when not trimming).
    [[nodiscard]] const TrimPreview& trimPreview() const { return m_trimPreview; }

signals:
    /// The user asked to inspect @p clipId (the page shows the Inspector on that tab).
    void inspectRequested(quint64 clipId, kk::TimelineWidget::InspectTarget target);
    /// Zoom or horizontal scroll changed (the Keyframes panel's Follow Timeline mode).
    void viewChanged();
    /// A trim, roll, slip or slide drag changed the frames at its edit (spec 8.15); an
    /// inactive preview when the gesture ends. The page shows them in the timeline viewer.
    void trimPreviewChanged(const kk::TrimPreview& preview);

public slots:
    /// View-only vertical zoom of all tracks (Shift+wheel in the Resolve wheel scheme).
    void setTrackHeightScale(double scale);
    /// Timeline View Options (spec 12.11): what video clips show (filmstrip, head and tail
    /// frames, none) and whether audio clips draw waveforms. View only; repaints at once.
    void setClipThumbnails(ClipThumbnails mode);
    [[nodiscard]] ClipThumbnails clipThumbnails() const { return m_thumbMode; }
    void setShowWaveforms(bool on);
    [[nodiscard]] bool showWaveforms() const { return m_showWaveforms; }
    /// Media clips show their source in timecode after the name (spec 12.14; off by default).
    void setShowSourceTimecode(bool on);
    [[nodiscard]] bool showSourceTimecode() const { return m_showSourceTimecode; }
    /// Adds the Track Height submenu (Small / Medium / Large, spec 12.10) for track @p ref, or
    /// for every track with @p allTracks (the Timeline View Options menu).
    void addTrackHeightMenu(QMenu& menu, TrackRef ref, bool allTracks);
    [[nodiscard]] double trackHeightScale() const { return m_layout.heightScale(); }
    void zoomIn();
    void zoomOut();
    /// Opens the inline track name editor if @p pos is on a track name in a header (a
    /// double-click, spec 12.15): Enter commits in one undo step, Esc cancels, clicking away
    /// commits. Returns false if @p pos is not on a track name.
    bool beginTrackRename(QPointF pos);
    /// The open inline track name editor, or null.
    [[nodiscard]] QLineEdit* trackNameEditor() const { return m_trackNameEditor; }
    /// Fits the whole timeline into the view.
    void zoomToFit();
    /// Shift+Z (spec 19.9): fits the whole timeline into the view; pressed again before the
    /// zoom was changed by other means, it returns to the zoom and scroll from before the fit.
    void toggleZoomToFit();
    /// Repaints clips after thumbnails/waveforms became available.
    void refreshMedia();
    /// Page-scrolls if the playhead is outside the visible range.
    void ensurePlayheadVisible();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    /// Context menu of a transition (spec 13.10): delete, duration, alignment.
    void transitionMenu(QMenu& menu, const TransitionRef& ref);
    /// Pieces of the clip menu, shared by both menu layouts: the angle switch of a multicam
    /// clip, Remove Transitions, the alignment of the transitions at a cut, Remove Effects.
    /// Each adds nothing when it does not apply.
    void addMulticamAngleMenu(QMenu& menu, quint64 clipId);
    void addRemoveTransitionsEntry(QMenu& menu, quint64 clipId, TrackRef track);
    void addTransitionAlignmentMenus(QMenu& menu, quint64 clipId, TrackRef track);
    void addRemoveEffectsEntry(QMenu& menu, quint64 clipId, TrackRef track);
    /// The clip menu of the Streamlined menu layout (spec 82): at most 12 entries, grouped,
    /// what does not apply is left out.
    void streamlinedClipMenu(QMenu& menu, const TimelineHit& hit);
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    /// Tool tips: a ruler marker or a clip marker under the pointer shows its name, timecode,
    /// duration and notes (spec 18.15).
    bool viewportEvent(QEvent* event) override;

private:
    /// Applies the remembered zoom / scroll of timeline @p uid (fit when there is none).
    void restoreViewState(quint64 uid);
    // ---- Input state machine (TimelineInput.cpp) ----
    enum class DragMode {
        None,
        Pending, ///< button down, waiting for the drag threshold; becomes `pending`
        Scrub,
        RubberBand,
        Move,
        TrimHead,
        TrimTail,
        Roll,
        Slip,
        Slide,
        ResizeTrack,
        Pan,
        TransitionEdge, ///< dragging an outer edge of a transition changes its length (13.9)
        RetimePoint, ///< a speed point / clip end handle of the retime controls (31.4)
        Shuffle, ///< Ctrl+Shift drag: swap / shuffle insert within the track (21.6)
        Fade, ///< a fade handle at a clip's top corner (spec 31.4)
        FadeCurve, ///< the dot on a fade line: the fade's shape (spec 59.2)
        LevelLine, ///< the audio level line: volume up / down (spec 31.1)
        LevelKey, ///< a volume key on the level line: time and value (spec 31.2)
        MarkerDuration, ///< Alt+drag a ruler marker / drag a ranged marker's end (spec 18.11)
        MarkerMove, ///< drag a ruler marker's flag: moves the marker (spec 18.14)
        TransitionCopy ///< Alt+drag a transition block onto another edit point (spec 13.17)
    };
    /// An outer edge of a transition under the pointer.
    struct TransitionEdgeHit
    {
        quint64 clipId = 0; ///< clip holding the transition
        bool head = true; ///< transitionIn (else transitionOut)
        bool leftEdge = true; ///< the window's left edge (else its right edge)
    };
    struct Drag
    {
        DragMode mode = DragMode::None;
        DragMode pending = DragMode::None;
        QPointF pressPos;
        QPointF lastPos;
        TimelineHit hit;
        Qt::KeyboardModifiers modifiers;
        bool ripple = false; ///< trims: ripple (trim tool) or leave gaps (select tool)
        bool editOpen = false;
        bool selectOnRelease = false; ///< click on a selected clip without dragging
        bool altOneSideOnRelease = false; ///< Alt-click without dragging: only the clicked side
        TimelineData origin; ///< state at drag start; restored before every live update
        QVector<quint64> clips; ///< affected clips (primary first)
        QVector<std::pair<quint64, quint64>> rolls; ///< roll: (left, right) pairs
        quint64 primary = 0;
        TrackRef primaryTrack;
        int64_t primaryStart = 0;
        int64_t primaryEnd = 0;
        /// Trims: where in the trim zone the press was, in frames from the edge (set on the
        /// first move), so the edge follows the pointer by the same offset instead of jumping.
        std::optional<double> trimGrab;
        int64_t minStart = 0; ///< earliest start of the moved clips
        int lastDy = 0;
        editops::Clipboard board; ///< Alt-drag duplicate source
        bool duplicate = false;
        QVector<quint64> resultIds; ///< ids created by the gesture (duplicate)
        std::vector<int64_t> snaps; ///< snap candidates (dragged clips excluded)
        QVector<quint64> baseSelection; ///< rubber band with Shift/Ctrl keeps these
        int originHeight = 0;
        int scrollXAtPress = 0;
        int scrollYAtPress = 0;
        TransitionEdgeHit transition; ///< TransitionEdge: which edge is dragged
        TransitionRef copySource; ///< TransitionCopy: the transition being copied
        std::optional<TransitionRef> copyTarget; ///< TransitionCopy: where the copy is now
        retimebar::Hit retime; ///< RetimePoint: which handle
        quint64 retimeClip = 0;
        // Fade / LevelLine / LevelKey (TimelineLevelsInput.cpp)
        quint64 levelClip = 0;
        bool fadeHead = true; ///< Fade / FadeCurve: the fade-in handle (else the fade-out)
        double curveGrabDy = 0.0; ///< FadeCurve: pointer y minus the dot's centre y at the press
        int64_t levelFrame = 0; ///< LevelLine: clip-local frame grabbed
        double levelDb = 0.0; ///< accumulated dB offset (Shift = fine steps)
        double levelLastY = 0.0; ///< pointer y of the previous move
        QRectF levelArea; ///< LevelLine / LevelKey: the clip's waveform area at the press
        KeyframeRef levelKey; ///< LevelKey: the key at the press (origin)
        int64_t markerFrame = -1; ///< MarkerDuration / MarkerMove: Marker::frame at the press
        int64_t markerNow = -1; ///< MarkerMove: where the marker is now
        double levelKeyDb = 0.0; ///< LevelKey: its value at the press
        /// Scrub: the playhead's head itself was grabbed (it does not jump to the pointer);
        /// scrubGrabPx = pointer x minus the playhead frame's x at the press (spec 48.1).
        bool scrubGrabbed = false;
        double scrubGrabPx = 0.0;
        /// A ruler press away from the head with "jump at once" off: a click (no drag) moves
        /// the playhead on release, a drag does nothing (spec 48.1).
        bool rulerClick = false;
    };

    void startDrag(DragMode mode);
    void updateDrag(QPointF pos);
    void finishDrag();
    void cancelDrag();
    void openEdit(const QString& label);
    void restoreOrigin();
    void updateMove(QPointF pos);
    void updateTrim(QPointF pos);
    void updateRoll(QPointF pos);
    void updateSlipSlide(QPointF pos);
    void updateRubberBand(QPointF pos);
    void updateTransitionEdge(QPointF pos);
    void updateTransitionCopy(QPointF pos);
    void updateShuffle(QPointF pos);
    /// Updates the drag offset label and the trim preview from the live state of the gesture
    /// (TimelineDragInfo.cpp); clearDragInfo() when the gesture ends.
    void refreshDragInfo();
    void clearDragInfo();
    // ---- Level lines and fade handles (TimelineLevelsInput.cpp) ----
    /// Left press with the selection tool on a clip: a fade handle, a volume key or the
    /// level line (Alt+click adds a key). Returns whether the press was taken.
    bool pressLevels(QPointF pos, const TimelineHit& hit);
    /// Double-click on a fade curve dot: back to linear, one undo step (spec 59.4).
    bool resetFadeCurveAt(QPointF pos);
    void updateLevelDrag(QPointF pos);
    /// Cursor for @p pos over clip @p hit's level line / fade handles, if any.
    [[nodiscard]] std::optional<Qt::CursorShape> levelCursor(
        QPointF pos, const TimelineHit& hit) const;
    /// Right-click on a volume key: its menu (Delete Keyframe). Returns whether one was there.
    bool levelKeyMenu(QPointF pos, QPoint globalPos);
    // ---- Retime controls (TimelineRetimeInput.cpp) ----
    /// The retime control under @p pos (clip id in @p clipId), Kind::None if there is none.
    [[nodiscard]] retimebar::Hit retimeHitAt(QPointF pos, quint64* clipId) const;
    /// Left press on the retime controls: opens the menu or starts a handle drag. Returns
    /// whether the press was taken.
    bool pressRetime(QPointF pos);
    void updateRetimeDrag(QPointF pos);
    /// Opens the curve editor of @p clipId showing only its retime curve.
    void showRetimeCurve(quint64 clipId);
    /// The transition edge within grab distance of @p pos, if any (TimelineGestures.cpp).
    [[nodiscard]] std::optional<TransitionEdgeHit> transitionEdgeAt(QPointF pos) const;
    /// Ruler double-click: opens the marker editor for a marker near @p pos. Returns whether
    /// one was there.
    bool editMarkerNear(QPointF pos);
    /// Timeline marker whose flag is within grab distance of @p pos in the ruler (its frame),
    /// or -1.
    [[nodiscard]] int64_t markerFlagNear(QPointF pos) const;
    /// Ranged timeline marker whose end is within grab distance of @p pos in the ruler's
    /// marker band (its frame), or -1.
    [[nodiscard]] int64_t markerEndNear(QPointF pos) const;
    /// Ruler press on a marker: Alt on its flag or a press on a ranged marker's end starts a
    /// MarkerDuration drag (spec 18.11); a plain press on a flag waits to move the marker
    /// (MarkerMove, spec 18.14; a click without moving jumps to it). Returns true if the
    /// press was taken.
    bool pressRulerMarker(QPointF pos, Qt::KeyboardModifiers modifiers);
    /// The clip marker whose diamond is under @p pos (in the clip's name band): its clip and
    /// clip-local frame.
    [[nodiscard]] std::optional<std::pair<quint64, int64_t>> clipMarkerNear(QPointF pos) const;
    /// Tool tip text of @p marker (name, timecode, duration of a ranged one, notes).
    [[nodiscard]] QString markerToolTip(const Marker& marker, int64_t timelineFrame) const;
    /// MarkerMove drag: the marker follows the pointer by whole frames (snapping); it never
    /// lands on another marker's frame.
    void updateMarkerMove(QPointF pos);
    /// MarkerDuration drag: the marker's end follows the pointer (snapping), at least 1 frame.
    void updateMarkerDuration(QPointF pos);
    /// Double-click on a compound clip / nested timeline body: opens its timeline in a tab
    /// with the playhead at the frame under @p pos. Returns whether it did (spec 26.14).
    bool openNestedAt(QPointF pos);
    /// Whether @p y lies past the outermost track of @p type: above the top video track or
    /// below the bottom audio track (moves there create a new track, spec 21.8).
    [[nodiscard]] bool beyondOutermostTrack(TrackType type, double y) const;
    void scrubTo(QPointF pos);
    /// Left press in the ruler (no marker): grab the head, scrub, or a pending click (48.1).
    void pressRuler(QPointF pos);
    void bladeAt(const TimelineHit& hit);
    void clickSelect(const TimelineHit& hit, Qt::KeyboardModifiers modifiers);
    /// @p clipId with its linked clips (linked selection on), @p clipId last = current clip.
    [[nodiscard]] QVector<quint64> clickedWithLinks(quint64 clipId) const;
    void headerClick(const TimelineHit& hit);
    void updateHover(QPointF pos);
    void autoScrollTick();
    /// Snaps @p frame to the drag's candidates when snapping is on (and sets the indicator).
    [[nodiscard]] std::optional<int64_t> snapFrame(int64_t frame) const;
    /// What a playhead scrub snaps to (spec 11.8, 11.9).
    [[nodiscard]] std::vector<int64_t> scrubSnapCandidates() const;
    [[nodiscard]] int64_t snapToleranceFrames() const;

    // ---- Drag and drop (TimelineDrop.cpp) ----
    struct DropPlan
    {
        enum class Kind {
            None,
            Clips,
            Effect,
            Transition,
            Files, ///< files from a file manager: imported into the media pool on drop
            TimelineClips ///< clips of another timeline (stacked timelines): copied
        };
        Kind kind = Kind::None;
        /// Clips: groups of clips created from one source each (linked after placing).
        QVector<QVector<editops::PlacedClip>> groups;
        bool insert = false; ///< Ctrl: insert instead of overwrite
        /// Clips: dropped in the empty space above the top video track / below the last audio
        /// track (Resolve: a new track is created there); the clips' refs already use the new
        /// index (= the current track count).
        bool newVideoTrack = false;
        bool newAudioTrack = false;
        /// Clips: subtitle tracks to add first (a subtitle file's overlapping cues need more
        /// than the existing ones); the refs already use the new indices.
        int newSubtitleTracks = 0;
        QRect newTrackBand; ///< where the ghost of a new track's clips is drawn
        TrackType newTrackBandType = TrackType::Video; ///< the new track shown in the band
        quint64 clipId = 0; ///< Effect/Transition target
        QString effectId;
        bool head = true; ///< Transition: at the clip head (transitionIn) or tail
        int64_t transitionFrames = 0;
        int64_t previewFrom = 0;
        int64_t previewTo = 0;
        int previewRow = -1;
        // TimelineClips: what to copy and where (TimelineController::copyClipsFrom()).
        quint64 sourceUid = 0;
        QVector<quint64> sourceClips;
        int64_t copyFrame = 0;
        TrackRef copyTrack;
    };
    /// The TimelineClips part of planDrop() (TimelineStackedDrop.cpp).
    void planClipCopy(const QMimeData* mime, const TimelineHit& hit, QPointF pos, DropPlan& plan);
    [[nodiscard]] DropPlan planDrop(
        const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers);
    void showDropPreview(const DropPlan& plan);
    bool executeDrop(const DropPlan& plan);
    void clearDropPreview();
    /// DragEnter of files from a file manager: remembers their paths and starts the probes
    /// behind their ghost clips (spec 5.6).
    void startFileDrag(const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers);
    /// Drop of files: imports those not in the media pool yet (current bin), returns the ids
    /// of every file in drag order (existing pool items reused).
    QStringList importDroppedFiles(const QStringList& files);

    // ---- Helpers (TimelineWidget.cpp) ----
    void onTimelineChanged();
    void onSelectionChanged();
    void onPlayheadChanged(int64_t frame);
    /// Animations: takes the current model as the animator's previous state when the last
    /// changes happened during an open edit (a drag), so the next edit animates from it.
    void syncAnimations();
    /// Animations turned on / off or their speed changed (uianim::Notifier).
    void onAnimationSettingsChanged();
    void updateScrollBars();
    void zoomBy(double factor, double anchorX);
    [[nodiscard]] double zoomAnchorX() const;

    AppContext& m_ctx;
    TimelineController* m_controller = nullptr;
    TimelineLayout m_layout;
    TimelinePainter m_painter;
    /// Project timelines by uid for the painter (multicam angle names); set once.
    std::function<const TimelineData*(quint64)> m_timelineLookup;
    KeyframeLanes* m_keyframeLanes = nullptr; ///< child QObject: keyframe / curve editor lanes
    QPointer<QLineEdit> m_trackNameEditor; ///< beginTrackRename()
    QPointer<QMenu> m_retimeMenu; ///< open speed / speed point menu
    NestedPreview* m_nestedPreview = nullptr; ///< child QObject: compound clip previews
    TimelineOverlay m_overlay;
    DroppedFileProbe* m_fileProbe = nullptr; ///< child: probes of dragged-in files (lazy)
    QStringList m_dragFiles; ///< paths of the file drag over the timeline (empty: none)
    QPointF m_dragPos; ///< last DragMove position of that drag (re-planned after a probe)
    Qt::KeyboardModifiers m_dragModifiers;
    QVector<CacheBarRange> m_cacheBar; ///< render cache status on the ruler
    QSet<quint64> m_selected; ///< mirror of the SelectionModel for O(1) lookups while painting
    Drag m_drag;
    QTimer m_autoScroll;
    bool m_followPlayhead = true;
    TimelineViewOptions m_options;
    ClipThumbnails m_thumbMode = ClipThumbnails::Filmstrip;
    bool m_showWaveforms = true;
    bool m_showSourceTimecode = false;
    bool m_syncingScrollBars = false;
    bool m_fitOnFirstShow = true;
    /// toggleZoomToFit(): the zoom and scroll before the fit, and the fitted zoom (0 = none).
    struct
    {
        double pixelsPerFrame = 0.0;
        double scrollX = 0.0;
        double fitted = 0.0;
    } m_beforeFit;
    quint64 m_hoverClip = 0; ///< clip under the mouse (fade handles shown)
    ClipDragOut m_clipDragOut;
    TimelineAnimator m_anim; ///< display-only tweens (TimelineAnimator.h)
    int64_t m_lastPlayhead = 0; ///< playhead at the last playheadChanged() (glide start)
    quint64 m_rowsTimelineUid = 0; ///< timeline of the layout rows (onTimelineChanged())
    TrimPreview m_trimPreview; ///< refreshDragInfo()
};

} // namespace kk
