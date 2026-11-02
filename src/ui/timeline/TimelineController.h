// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController — the timeline's commands (split, delete, marks, paste, insert from the
// source viewer, ...) and editor state (tool, snapping, linked selection, track targeting).
#pragma once

#include "core/EditOps.h"
#include "core/SelectionModel.h"
#include "core/TimelineData.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace kk {

struct AppContext;
struct MediaItem;
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
    AppendAtEnd, ///< overwrite after the last clip of the timeline (Shift+F12)
    /// replace the timeline in..out range (else the clip under the playhead) with the source
    /// at its own length, rippling the rest by the difference (Shift+F10, spec 4.11)
    RippleOverwrite
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

/// Answer to the "not enough media handles" question when adding a transition (spec 13.7).
enum class HandleChoice {
    Cancel, ///< add nothing
    TrimClips, ///< ripple-trim the clips at the cut so the media covers the transition
    Shorten, ///< make the transition as long as the handles allow
    AddAnyway ///< add it at full length; missing frames repeat the edge frame
};

/// Attribute groups for paste attributes (Alt+V, spec 16.4).
struct PasteAttributeSet
{
    bool transform = true; ///< position, zoom, rotation, crop, opacity, flips (with keyframes)
    bool effects = true; ///< the video effect chain
    bool volume = true; ///< audio volume (with keyframes)
    bool pan = true; ///< audio pan (with keyframes)
    bool speed = false; ///< clip speed (changes the duration; off by default)
    [[nodiscard]] bool any() const { return transform || effects || volume || pan || speed; }
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
    /// Selects the transition @p ref (SelectionModel::selectTransition; clears clips, the edit
    /// point and the gap). The Inspector shows it on its Transition tab (spec 13.10).
    void selectTransition(const TransitionRef& ref);

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
    /// Match frame target (wired by the page): loads media @p mediaId in the source viewer,
    /// parks it on source time @p sourceUs and marks [@p inUs, @p outUs) (the part of the
    /// media the timeline clip uses).
    using MatchFrameHandler = std::function<void(
        const QString& mediaId, int64_t sourceUs, int64_t inUs, int64_t outUs)>;
    void setMatchFrameHandler(MatchFrameHandler handler) { m_matchFrame = std::move(handler); }
    /// Asks what to do when a new transition lacks @p missingFrames frames of media handles;
    /// @p canShorten tells whether a shorter transition fits. Without a prompt (tests) the
    /// transition is added anyway (HandleChoice::AddAnyway).
    using HandlePrompt = std::function<HandleChoice(int64_t missingFrames, bool canShorten)>;
    void setHandlePrompt(HandlePrompt prompt) { m_handlePrompt = std::move(prompt); }
    /// Marker editor (spec 18.5): edits @p marker in place (name, colour, note) and returns
    /// true on OK. @p clipMarker tells whether it belongs to a clip. Wired by the panel.
    using MarkerEditor = std::function<bool(Marker& marker, bool clipMarker)>;
    void setMarkerEditor(MarkerEditor editor) { m_markerEditor = std::move(editor); }
    /// Asks which attributes to paste (Alt+V); std::nullopt = cancelled. Wired by the panel;
    /// without it every default group is pasted.
    using PasteAttributesPrompt = std::function<std::optional<PasteAttributeSet>()>;
    void setPasteAttributesPrompt(PasteAttributesPrompt prompt)
    {
        m_pastePrompt = std::move(prompt);
    }
    /// Whether the program engine is playing (dynamic trim follows playback). Default: asks
    /// AppContext::playback; tests inject their own.
    void setPlayingProvider(std::function<bool()> provider) { m_playing = std::move(provider); }

    // ---- Timelines and compound clips (TimelineCompound.cpp) ----

    /// Asks for a name (dialog @p title, field @p label, prefilled with @p suggestion, accept
    /// button @p okText); nothing = cancelled. Without a prompt the suggestion is used.
    using NamePrompt = std::function<std::optional<QString>(const QString& title,
        const QString& label, const QString& suggestion, const QString& okText)>;
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }
    /// Asks a yes/no question; without a prompt the answer is yes.
    using ConfirmPrompt = std::function<bool(const QString& title, const QString& text)>;
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirmPrompt = std::move(prompt); }
    /// Tells the user why a command did nothing (e.g. "a timeline cannot contain itself").
    using MessageSink = std::function<void(const QString& title, const QString& text)>;
    void setMessageSink(MessageSink sink) { m_messageSink = std::move(sink); }
    /// Shows the Timeline Settings dialog for timeline @p uid (installed by the panel).
    void setTimelineSettingsPrompt(std::function<void(quint64 uid)> prompt)
    {
        m_timelineSettingsPrompt = std::move(prompt);
    }
    /// Media pool bin new timelines and compound clips go into (default: the root bin;
    /// EditPage wires the media pool's current bin, as in Resolve).
    void setBinProvider(std::function<QString()> provider) { m_binProvider = std::move(provider); }

    /// Clip > New Compound Clip: collapses the selection (with linked clips when linked
    /// selection is on) into a compound clip named @p name (empty: asks, suggesting
    /// "Compound Clip N"). One undo step. Returns the compound timeline's uid, or 0.
    quint64 newCompoundClip(const QString& name = {});
    /// Clip > Decompose in Place for the selected compound clip(s) / nested timelines. One
    /// undo step per instance pair. Returns whether anything was decomposed.
    bool decomposeSelection();
    /// Clip > Open in Timeline: opens the selected compound clip's timeline in a tab.
    bool openSelectedInTimeline();
    /// File > New Timeline: asks for the name (empty @p name), creates and opens it.
    quint64 newTimeline(const QString& name = {});
    /// Duplicates the active timeline and opens the copy.
    quint64 duplicateActiveTimeline();
    /// Deletes the active timeline after confirmation (refused for the last timeline).
    bool deleteActiveTimeline();
    /// Activates the next (@p step = 1) or previous (-1) tab, wrapping around.
    void cycleTab(int step);
    /// Closes the active tab (not the last one).
    bool closeActiveTab();

    // ---- Clip factories ----

    /// Video and/or audio clips for media item @p mediaId covering source [inUs, outUs)
    /// (outUs <= inUs: to the end; stills and media without duration get 5 s) on video track
    /// @p videoTrack and audio track @p audioTrack (-1 = leave that part out). Clips have
    /// start 0, id 0 and no link group: link the placed ids with editops::linkClips().
    /// Does not modify the timeline (used for drop previews too). Empty if the id is unknown.
    [[nodiscard]] QVector<editops::PlacedClip> makeMediaClips(
        const QString& mediaId, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const;
    /// makeMediaClips() for a timeline / compound clip item: Nested clips (source times in the
    /// nested timeline's µs). Empty when placing it would make the active timeline contain
    /// itself (Project::canNest()).
    [[nodiscard]] QVector<editops::PlacedClip> makeNestedClips(
        const MediaItem& item, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const;
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
    /// Backspace (spec 10.1, 10.7): lifts the selected clips (gaps stay); a selected gap is
    /// closed; with no clip selected and in + out marks set, lifts the marked range on the
    /// auto-select tracks.
    void deleteSelection();
    /// Delete / Shift+Backspace (spec 10.2-10.8): ripple-deletes the selected clips (later
    /// clips on their tracks close up, other auto-select tracks follow where they have room);
    /// closes a selected gap; with no clip selected and in + out marks set, extracts the
    /// marked range on the auto-select tracks (playhead to the in mark, marks cleared).
    void rippleDeleteSelection();
    /// Ctrl+Shift+X (spec 10.9): copies the selected clips (else the marked range) to the
    /// clipboard and ripple-deletes them, in one undo step.
    void rippleCut();
    /// Removes every gap common to the auto-select tracks (keeps sync; Edit > Delete Gaps).
    void deleteGaps();
    /// Selects the clips starting at or after (@p forward) / ending at or before the playhead
    /// on the auto-select tracks, or on every unlocked track with @p allTracks.
    void selectClipsFromPlayhead(bool forward, bool allTracks);
    /// V: selects the edit point closest to the playhead on the auto-select tracks (video
    /// first); at a cut both sides (roll).
    void selectNearestEdit();
    /// Shift+V: selects the clip under the playhead (auto-select tracks, V1 upwards), else the
    /// gap under the playhead on the target video track.
    void selectNearestClipOrGap();
    /// Joins through edits: the selected edit point, or touching selected clips that continue
    /// each other (linked parts join along). One undo step.
    void joinSelection();
    /// Swaps the selected clip (and its linked parts) with its previous (-1) / next (+1)
    /// neighbour (spec 21.6).
    void swapSelection(int direction);
    /// F: loads the source of the clip under the playhead into the source viewer at the same
    /// frame (via the match frame handler).
    void matchFrame();
    /// Alt+n / Ctrl+Alt+n: makes track number @p number (1 = V1/A1) the destination.
    void setTargetByNumber(TrackType type, int number);
    /// Ctrl+n / Ctrl+Shift+n: toggles the auto track selector of track number @p number.
    void toggleAutoSelectByNumber(TrackType type, int number);
    /// Alt+M: deletes the timeline marker at the playhead.
    void deleteMarkerAtPlayhead();
    /// Ctrl+D: gives the selected clips (with linked parts) a duration of @p frames, rippling
    /// later clips. Limited by the media. Returns whether anything changed.
    bool setSelectionDuration(int64_t frames);
    /// R: plays the selected clips (with linked parts) at @p speed (1.0 = 100 %); the
    /// duration changes accordingly (limited by the media) and later clips ripple.
    bool setSelectionSpeed(double speed);
    void selectAll();
    void deselectAll();
    /// Moves the selected clips, or trims the selected edit point (ripple in the trim tool,
    /// roll when both sides are selected), by @p frames.
    void nudge(int64_t frames);
    /// Moves the selected edit point to the playhead (E; spec 8.11).
    void extendEditToPlayhead();
    /// U (spec 8.16): cycles the selected edit point at a cut between the outgoing tail, the
    /// roll (both sides) and the incoming head. No-op without a touching neighbour.
    void cycleEditPoint();
    /// Alt-click on an auto track selector (spec 6.6): only @p ref keeps it on among the
    /// tracks of its type. One undo step.
    void soloAutoSelect(TrackRef ref);
    /// Trims the head (Shift+[) / tail (Shift+]) of the clips under the playhead to the
    /// playhead, leaving a gap: the selected clips, or those on auto-select tracks (spec 8.10).
    void trimStartToPlayhead();
    void trimEndToPlayhead();
    /// Same clips, rippling (Ctrl+Shift+[ / Ctrl+Shift+]): the gap closes, later clips move
    /// left and the playhead lands on the old start (spec 8.10). One undo step.
    void rippleTrimStartToPlayhead();
    void rippleTrimEndToPlayhead();
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
    void addMarker(); ///< timeline marker at the playhead (never two on one frame)
    void toggleClipsEnabled();
    /// Ctrl+Alt+L: links the selected clips, or unlinks them if they already form one group.
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
    /// Changes the alignment of the transition at the head (@p head) or tail of @p clipId
    /// (spec 13.5); one undo step. Returns whether it changed.
    bool setTransitionAlignment(quint64 clipId, bool head, TransitionAlignment alignment);
    /// Sets the length of that transition (clamped to the clips); one undo step.
    bool setTransitionDuration(quint64 clipId, bool head, int64_t frames);
    /// Removes the transition @p ref (not on a locked track); one undo step. Delete and
    /// Backspace call it when a transition is selected (spec 13.10).
    bool deleteTransition(const TransitionRef& ref);
    /// M (spec 18.1, 18.4, 18.5): a clip marker on the selected clip under the playhead, else
    /// a timeline marker; on an existing marker opens the marker editor instead.
    void addMarkerAtPlayhead();
    /// Ctrl+M: adds the marker (as addMarkerAtPlayhead()) and opens the editor for it.
    void addAndModifyMarker();
    /// Opens the editor for the timeline marker at @p frame (double-click in the ruler).
    /// Returns whether a marker was changed.
    bool editTimelineMarker(int64_t frame);
    /// Alt+V: asks for the attribute groups, then pastes them (pasteAttributes()).
    void pasteAttributesInteractive();
    /// Copies the chosen attributes of the first clipboard clip onto the selected clips (with
    /// linked parts: video groups onto video clips, audio groups onto audio clips). One undo
    /// step. Returns whether anything changed.
    bool pasteAttributes(const PasteAttributeSet& attributes);
    /// W (spec 8.17): dynamic trim mode. While on and an edit point is selected, playback
    /// (J/K/L, Space) drags the edit point along with the playhead: a roll, or a ripple trim in
    /// the trim tool. One undo step per playback run.
    void setDynamicTrim(bool on);
    [[nodiscard]] bool dynamicTrim() const { return m_dynamicTrim; }
    /// Playback stopped: closes a dynamic-trim run (one undo step). Connected to the program
    /// engine's rate changes.
    void playbackStopped() { endDynamicRun(); }
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
    /// True if the timeline has both an in and an out mark (out > in).
    [[nodiscard]] bool hasMarkedRange() const;
    void setMarks(int64_t in, int64_t out, const QString& label);
    void trimToPlayhead(bool head, bool ripple);
    /// Trims the selected edit point by @p frames (one undo step). Returns the applied delta.
    int64_t trimSelectedEdit(int64_t frames);
    /// The trim of trimSelectedEdit() on @p data without opening an edit (roll when both
    /// sides are set; linked clips follow). Returns the applied delta.
    int64_t applyEditTrim(
        TimelineData& data, const EditPointSelection& edit, int64_t frames, bool ripple) const;
    /// M / Ctrl+M: a clip marker on the selected clip under the playhead, else a timeline
    /// marker; opens the editor on an existing marker or with @p forceEdit.
    void placeMarker(bool forceEdit);
    /// Opens the editor for the marker at clip-local @p localFrame of @p clipId.
    bool editClipMarker(quint64 clipId, int64_t localFrame);
    void onClipSelectionChanged();
    /// Dynamic trim: the playhead moved to @p frame.
    void onPlayheadMoved(int64_t frame);
    /// Dynamic trim: closes the current run (one undo step).
    void endDynamicRun();
    [[nodiscard]] bool isPlaying() const;

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
    MatchFrameHandler m_matchFrame;
    HandlePrompt m_handlePrompt;
    MarkerEditor m_markerEditor;
    PasteAttributesPrompt m_pastePrompt;
    std::function<bool()> m_playing;
    NamePrompt m_namePrompt;
    ConfirmPrompt m_confirmPrompt;
    MessageSink m_messageSink;
    std::function<void(quint64)> m_timelineSettingsPrompt;
    std::function<QString()> m_binProvider;
    bool m_dynamicTrim = false;
    bool m_dynamicOpen = false; ///< an undoable dynamic-trim run is in progress
    int64_t m_dynamicLast = 0; ///< playhead at the last dynamic-trim step
};

} // namespace kk
