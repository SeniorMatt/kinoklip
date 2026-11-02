// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController — the timeline's commands (split, delete, marks, paste, insert from the
// source viewer, ...) and editor state (tool, snapping, linked selection, track targeting).
#pragma once

#include "core/EditOps.h"
#include "core/TimelineData.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace kk {

struct AppContext;
class Project;
class Timeline;

/// Editing tool of the timeline (actions tool.select / tool.trim / tool.blade).
enum class TimelineTool {
    Select, ///< move, trim with gaps, rubber band
    Trim, ///< ripple trim, roll at edit points, slip (Alt: slide) on clip bodies
    Blade ///< split clips at the pointer
};

/// How a clip from the source viewer (or a drop) is put onto the timeline.
enum class SourceEditMode {
    Insert, ///< ripple everything after the playhead (F9)
    Overwrite, ///< replace what is under the new clip (F10)
    PlaceOnTop, ///< first free track above the target (F12)
    Replace, ///< replace the clip under the playhead, keeping its place and length (F11)
    FitToFill, ///< source in..out retimed into timeline in..out (Shift+F11)
    AppendAtEnd ///< overwrite after the last clip of the timeline (Shift+F12)
};

/// A selected edit point (spec 7.6): the tail of @p tailClip and/or the head of @p headClip.
/// Both set = the cut between two touching clips (nudging rolls it).
struct EditPointSelection
{
    quint64 tailClip = 0;
    quint64 headClip = 0;
    [[nodiscard]] bool isEmpty() const { return !tailClip && !headClip; }
    friend bool operator==(const EditPointSelection&, const EditPointSelection&) = default;
};

/// A selected gap (spec 10.5): empty range [start, end) on one track.
struct GapSelection
{
    TrackRef track;
    int64_t start = 0;
    int64_t end = 0;
    friend bool operator==(const GapSelection&, const GapSelection&) = default;
};

/// A ranged source clip, as offered by the source viewer.
struct SourceSelection
{
    QString mediaId;
    int64_t inUs = 0;
    int64_t outUs = 0; ///< exclusive; <= inUs means "to the end of the media"
};

/// Timeline commands and editor state shared by the timeline widget and the panel.
///
/// Every mutating command is one undoable step (Project::beginEdit()/endEdit()). Commands that
/// act on "the selection" expand it with linked clips when linked selection is on.
/// connectActions() binds the registry actions this module owns (see src/ui/common/readme.md).
///
/// Thread-safety: GUI thread only.
class TimelineController : public QObject
{
    Q_OBJECT
public:
    /// Creates the controller for the project/selection in @p ctx (kept by reference).
    explicit TimelineController(AppContext& ctx, QObject* parent = nullptr);

    [[nodiscard]] AppContext& context() const { return m_ctx; }
    [[nodiscard]] Project* project() const;
    [[nodiscard]] Timeline* timeline() const;

    /// Connects the tool.*, trim.*, edit.*, timeline.*, clip.*, mark.* and the navigation
    /// transport.prev_edit/next_edit/go_to_start/go_to_end actions. Safe without a registry.
    void connectActions();

    // ---- Editor state ----

    [[nodiscard]] TimelineTool tool() const { return m_tool; }
    void setTool(TimelineTool tool);
    [[nodiscard]] bool snapping() const { return m_snapping; }
    void setSnapping(bool on);
    [[nodiscard]] bool linkedSelection() const { return m_linked; }
    void setLinkedSelection(bool on);
    /// Destination video track index for source edits (-1 = video not placed).
    [[nodiscard]] int targetVideoTrack() const { return m_targetVideo; }
    /// Destination audio track index for source edits (-1 = audio not placed).
    [[nodiscard]] int targetAudioTrack() const { return m_targetAudio; }
    /// Sets the destination of @p type (-1 = none); invalid indices fall back to -1.
    void setTargetTrack(TrackType type, int index);
    /// Toggles the destination patch of track @p ref (target it, or untarget if it is).
    void toggleTarget(TrackRef ref);

    /// Selected edit point (empty = none). Selecting one clears the clip and gap selection;
    /// selecting clips clears it.
    [[nodiscard]] const EditPointSelection& selectedEdit() const { return m_selectedEdit; }
    void setSelectedEdit(const EditPointSelection& edit);
    /// Selected gap, if any (cleared by selecting clips or an edit point).
    [[nodiscard]] const std::optional<GapSelection>& selectedGap() const { return m_selectedGap; }
    void setSelectedGap(const std::optional<GapSelection>& gap);
    /// Selects the gap under @p frame on @p ref (clears it if there is none). Returns whether
    /// a gap was selected.
    bool selectGapAt(TrackRef ref, int64_t frame);

    /// The selection, expanded with linked clips when linked selection is on.
    [[nodiscard]] QVector<quint64> effectiveSelection() const;
    /// @p ids expanded with linked clips when linked selection is on.
    [[nodiscard]] QVector<quint64> withLinks(const QVector<quint64>& ids) const;

    /// Callback that returns the source viewer's current clip and range (wired by the page;
    /// used by the edit.insert/overwrite/place_on_top actions).
    void setSourceProvider(std::function<std::optional<SourceSelection>()> provider)
    {
        m_sourceProvider = std::move(provider);
    }

    // ---- Clip factories ----

    /// Video and/or audio clips for media item @p mediaId covering source [inUs, outUs)
    /// (outUs <= inUs: to the end; stills and media without duration get 5 s) on video track
    /// @p videoTrack and audio track @p audioTrack (-1 = leave that part out). Clips have
    /// start 0, id 0 and no link group: link the placed ids with editops::linkClips().
    /// Does not modify the timeline (used for drop previews too). Empty if the id is unknown.
    [[nodiscard]] QVector<editops::PlacedClip> makeMediaClips(
        const QString& mediaId, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const;
    /// A Title or Solid clip for generator/title descriptor @p effectId (5 s), or nullopt if
    /// the id is not a generator or title.
    [[nodiscard]] static std::optional<ClipData> makeGeneratorClip(
        const QString& effectId, Rational fps);
    /// Default duration of stills, titles and generators (frames).
    [[nodiscard]] static int64_t defaultStillFrames(Rational fps);

    // ---- Public API for the source viewer / playback ----

    /// Puts source range [inUs, outUs) of media @p mediaId at the playhead on the targeted
    /// tracks with @p mode; moves the playhead to the end of the new clip. One undo step.
    /// Returns the new clip ids (empty if nothing was placed).
    QVector<quint64> insertSource(
        const QString& mediaId, int64_t inUs, int64_t outUs, SourceEditMode mode);

public slots:
    // ---- Commands (each one undo step; no-ops when there is nothing to do) ----
    void splitAtPlayhead();
    void deleteSelection(); ///< lift: leaves gaps
    void rippleDeleteSelection(); ///< closes the gaps (or closes the gap under the playhead)
    void selectAll();
    void deselectAll();
    /// Moves the selected clips, or trims the selected edit point (ripple in the trim tool,
    /// roll when both sides are selected), by @p frames.
    void nudge(int64_t frames);
    /// Moves the selected edit point to the playhead (E; spec 8.11).
    void extendEditToPlayhead();
    /// Trims the head (Shift+[) / tail (Shift+]) of the clips under the playhead to the
    /// playhead: the selected clips, or those on auto-select tracks (spec 8.10).
    void trimStartToPlayhead();
    void trimEndToPlayhead();
    void goToPreviousEdit();
    void goToNextEdit();
    void goToPreviousMarker();
    void goToNextMarker();
    void goToStart();
    void goToEnd();
    void markIn();
    void markOut();
    void markClip(); ///< in/out around the selected clip, or the clip under the playhead
    void markSelection(); ///< in/out around all selected clips (spec 3.6)
    void clearIn();
    void clearOut();
    void clearInOut();
    void goToIn();
    void goToOut();
    void addMarker();
    void toggleClipsEnabled();
    void linkSelection();
    void unlinkSelection();
    void copy();
    void cut();
    void paste(); ///< overwrite at the playhead (spec 16.2)
    void pasteInsert(); ///< insert at the playhead, rippling (spec 16.3)
    /// Cross dissolve of the standard length (spec 13.1-13.3): at the selected edit point, at
    /// both ends of the selected clips, or at the edit nearest to the playhead on the
    /// auto-select video tracks; linked audio cut at the same frame gets a cross fade.
    void addDefaultTransition();
    void sourceEdit(SourceEditMode mode); ///< uses the source provider
    void addTrack(TrackType type, int index = -1);
    void removeTrack(TrackRef ref);
    void renameTrack(TrackRef ref, const QString& name);
    /// Toggles a header flag (HeaderButton::Lock/Visible/Mute/Solo given as TrackFlag).
    void toggleTrackFlag(TrackRef ref, int flag);
    /// Closes the gap under @p frame on @p ref (all tracks, keeping sync).
    void closeGapAt(TrackRef ref, int64_t frame);

public:
    /// Track flags for toggleTrackFlag().
    enum TrackFlag {
        FlagEnabled,
        FlagLocked,
        FlagMuted,
        FlagSolo,
        FlagAutoSelect
    };
    /// The clipboard (last copy/cut).
    [[nodiscard]] const editops::Clipboard& clipboard() const { return m_clipboard; }

signals:
    /// tool(), snapping(), linkedSelection() or the targets changed (repaint).
    void stateChanged();

private:
    void setPlayheadClamped(int64_t frame);
    void setMarks(int64_t in, int64_t out, const QString& label);
    void trimToPlayhead(bool head);
    /// Trims the selected edit point by @p frames (one undo step). Returns the applied delta.
    int64_t trimSelectedEdit(int64_t frames);
    void onClipSelectionChanged();

    AppContext& m_ctx;
    TimelineTool m_tool = TimelineTool::Select;
    bool m_snapping = true;
    bool m_linked = true;
    int m_targetVideo = 0;
    int m_targetAudio = 0;
    editops::Clipboard m_clipboard;
    EditPointSelection m_selectedEdit;
    std::optional<GapSelection> m_selectedGap;
    std::function<std::optional<SourceSelection>()> m_sourceProvider;
};

} // namespace kk
