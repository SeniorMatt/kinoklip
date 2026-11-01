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
    PlaceOnTop ///< first free track above the target (F12)
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
    void nudge(int64_t frames);
    void goToPreviousEdit();
    void goToNextEdit();
    void goToStart();
    void goToEnd();
    void markIn();
    void markOut();
    void markClip(); ///< in/out around the selected clip, or the clip under the playhead
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
    void paste();
    void addDefaultTransition(); ///< cross_dissolve, 1 s, at the edit nearest to the playhead
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
        FlagSolo
    };
    /// The clipboard (last copy/cut).
    [[nodiscard]] const editops::Clipboard& clipboard() const { return m_clipboard; }

signals:
    /// tool(), snapping(), linkedSelection() or the targets changed (repaint).
    void stateChanged();

private:
    void setPlayheadClamped(int64_t frame);
    void setMarks(int64_t in, int64_t out, const QString& label);

    AppContext& m_ctx;
    TimelineTool m_tool = TimelineTool::Select;
    bool m_snapping = true;
    bool m_linked = true;
    int m_targetVideo = 0;
    int m_targetAudio = 0;
    editops::Clipboard m_clipboard;
    std::function<std::optional<SourceSelection>()> m_sourceProvider;
};

} // namespace kk
