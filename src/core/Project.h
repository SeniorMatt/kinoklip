// SPDX-License-Identifier: GPL-3.0-or-later
// Project — owns the media pool, the timeline, the undo stack and project-level settings.
#pragma once

#include "core/GradeGallery.h"
#include "core/MediaLinks.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/Timeline.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

class QDir;
class QUndoStack;

namespace kk {

/// Project-wide settings. Resolution and frame rate are applied to every timeline that uses
/// the project settings (TimelineData::useProjectSettings).
struct ProjectSettings
{
    QString name = QStringLiteral("Untitled Project");
    int width = 1920;
    int height = 1080;
    Rational fps {24, 1}; ///< 24 for new projects, as in Resolve (spec 25.12)
    /// Default retime process of clips set to "Project Settings" (Master Settings > Frame
    /// Interpolation); mirrored into every timeline's TimelineData::retimeProcess.
    RetimeProcess retimeProcess = RetimeProcess::Nearest;
    /// Starting timecode of new timelines, in frames of `fps` (01:00:00:00 in new projects,
    /// 0 in projects saved before format 17; spec 25.12). Existing timelines keep their own.
    int64_t timelineStart = 0;

    friend bool operator==(const ProjectSettings&, const ProjectSettings&) = default;
};

/// Per-timeline view state remembered across tab switches and saved with the project (not
/// undoable, does not make the project modified).
struct TimelineViewState
{
    double pixelsPerFrame = 0.0; ///< horizontal zoom; 0 = not set yet (view default)
    int scrollX = 0; ///< horizontal scroll in px
    int scrollY = 0; ///< vertical scroll in px

    friend bool operator==(const TimelineViewState&, const TimelineViewState&) = default;
};

/// Per-bin view settings of the media pool (thumbnail / list, sort order), remembered per bin
/// and saved with the project (not undoable, does not make the project modified).
struct BinViewState
{
    QString mode; ///< "grid", "list"; empty = the panel's last choice
    QString sortKey; ///< list column key ("name", "duration", ...); empty = import order
    bool descending = false;

    friend bool operator==(const BinViewState&, const BinViewState&) = default;
};

/// The document: media pool + timelines + undo history + settings + file path.
///
/// **Timelines.** A project holds any number of timelines (TimelineData::uid), each listed in
/// the media pool as a Timeline or CompoundClip item. One of them is *active*: its data lives
/// in the single Timeline object returned by timeline() (what the timeline view, viewer,
/// inspector, keyframes panel and Deliver page show); the others are kept as immutable
/// shared copies. setActiveTimeline() swaps them (tabs, see tabs()). Nested clips reference
/// other timelines by uid; Timeline::snapshot() carries their data for rendering.
///
/// Undo is snapshot based: beginEdit() remembers every timeline (the inactive ones as shared
/// pointers, so this is cheap), the media pool and the project format; endEdit() pushes one
/// QUndoCommand holding the before/after state of what changed (nothing is pushed if nothing
/// changed). Edits nest: only the outermost begin/end pair creates a command. Undoing a step
/// made in another timeline first switches to that timeline, so the user sees what changes.
///
/// Thread-safety: GUI thread only.
class Project : public QObject
{
    Q_OBJECT
public:
    /// Creates a new, empty project (same as newProject()).
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    /// The media pool (owned by the project, never null).
    [[nodiscard]] MediaPool* mediaPool() const { return m_mediaPool; }
    /// The active timeline (owned by the project, never null; the same object for the whole
    /// project lifetime, its data changes when another timeline becomes active).
    [[nodiscard]] Timeline* timeline() const { return m_timeline; }
    /// The undo stack (owned by the project, never null). Prefer undo()/redo() below, which
    /// refuse to run while an edit is open.
    [[nodiscard]] QUndoStack* undoStack() const { return m_undoStack; }

    // ---- Settings & document state ----

    /// Current settings (name, resolution, frame rate).
    [[nodiscard]] ProjectSettings settings() const;
    /// Applies new settings; resolution/fps changes are undoable timeline edits.
    void setSettings(const ProjectSettings& settings);
    /// Proxy / optimized media links of the media pool items (owned by the project, never
    /// null; saved with it, not undoable). Changing a link marks the project modified.
    [[nodiscard]] MediaLinks* mediaLinks() const { return m_links; }
    /// The Color page gallery (stills with grades); saved with the project, not undoable
    /// (like a still store). Changing it marks the project modified.
    [[nodiscard]] GradeGallery* gallery() const { return m_gallery; }
    /// Optimized media, proxy and render cache settings + Playback menu modes.
    [[nodiscard]] const MediaCacheSettings& cacheSettings() const { return m_cacheSettings; }
    /// Applies new cache settings (not undoable, marks the project modified); emits
    /// cacheSettingsChanged() if they differ.
    void setCacheSettings(const MediaCacheSettings& settings);
    /// Project display name.
    [[nodiscard]] QString name() const { return m_name; }
    /// Renames the project (marks it modified).
    void setName(const QString& name);
    /// Path of the project file, empty if never saved.
    [[nodiscard]] QString filePath() const { return m_filePath; }
    /// True if there are unsaved changes.
    [[nodiscard]] bool isDirty() const;

    /// Resets to an empty project with default settings and V1/V2/A1/A2 tracks.
    void newProject();
    /// Writes the project to @p path (JSON, see core/readme.md) and makes it the current file.
    /// Returns false and sets @p error on failure.
    bool save(const QString& path, QString* error = nullptr);
    /// Writes a copy to @p path without changing filePath() or the modified state (autosave).
    /// @p extra is stored as the top-level "autosave" object (ignored by load()).
    bool saveCopy(
        const QString& path, QString* error = nullptr, const QJsonObject& extra = {}) const;
    /// Loads a project file, replacing the current contents and clearing the undo history.
    /// On failure the current project is unchanged and @p error describes the problem.
    bool load(const QString& path, QString* error = nullptr);
    /// Loads a project document given as JSON (a single-file project's document, read from the
    /// container) as if it had been read from @p documentPath: relative media paths resolve
    /// against its folder and filePath() becomes @p documentPath. Same rules as load().
    bool loadDocument(
        const QByteArray& json, const QString& documentPath, QString* error = nullptr);
    /// The project file's JSON for the current state as save() would write it at @p path
    /// (a single-file project stores it inside the container instead of writing the file).
    [[nodiscard]] QByteArray documentJson(const QString& path) const;
    /// After the document was stored somewhere other than a plain file (a single-file
    /// project): makes @p path the current file and clears the modified state, like save().
    void markSaved(const QString& path);
    /// Crash recovery: loads the autosave file @p autosavePath as if it were the document
    /// @p documentPath (empty = a never-saved project). Relative media paths resolve against
    /// the document's folder, filePath() becomes @p documentPath and the project is marked
    /// modified, so the user decides whether to save the recovered state over the file.
    bool loadRecovered(
        const QString& autosavePath, const QString& documentPath, QString* error = nullptr);

    // ---- Timelines (ProjectTimelines.cpp) ----

    /// Uids of every timeline (regular and compound), in creation order.
    [[nodiscard]] QVector<quint64> timelineUids() const;
    /// True if timeline @p uid exists.
    [[nodiscard]] bool hasTimeline(quint64 uid) const;
    /// Current data of timeline @p uid (the active one included), or nullptr. The pointer is
    /// immutable; it stays valid as long as the caller holds it.
    [[nodiscard]] std::shared_ptr<const TimelineData> timelineData(quint64 uid) const;
    /// Render snapshot of timeline @p uid (TimelineData::nested filled, like
    /// Timeline::snapshot() for the active one), or nullptr. Thumbnails, export of other
    /// timelines. A multicamViewKey() gives a snapshot of that multicam angle alone.
    [[nodiscard]] std::shared_ptr<const TimelineData> renderSnapshot(quint64 uid) const;
    /// Uid of the active timeline.
    [[nodiscard]] quint64 activeTimelineUid() const { return m_activeUid; }
    /// Makes timeline @p uid the active one (opens a tab for it). Refused (false) while an
    /// edit is open or for an unknown uid. Not undoable.
    bool setActiveTimeline(quint64 uid);
    /// Media pool item id of timeline @p uid (empty if none).
    [[nodiscard]] QString timelineItemId(quint64 uid) const;

    /// Creates an empty timeline (project resolution and frame rate) with @p videoTracks video
    /// and @p audioTracks audio tracks (V1, A1 by default, spec 25.14), starting at
    /// @p startTimecode (default: the project's timeline start), and its media pool item in
    /// bin @p binId. One undo step. Returns the uid. Does not activate it.
    quint64 createTimeline(const QString& name, const QString& binId = {}, int videoTracks = 1,
        int audioTracks = 1, std::optional<int64_t> startTimecode = std::nullopt);
    /// Adds a finished timeline (File > Import > Timeline, imported .otio files) and its media
    /// pool item in bin @p binId. @p data gets a new uid and the project's picture size and
    /// retime process; it follows the project's frame rate settings when its own rate equals
    /// the project's, else it keeps its rate. One undo step (part of the open one inside an
    /// edit, so the media it uses can be added in the same step). Returns the uid. Does not
    /// activate it.
    quint64 importTimeline(TimelineData data, const QString& binId = {});
    /// Copies timeline @p uid (all clips; name + " copy") into the same bin. One undo step.
    /// Returns the new uid, or 0 if @p uid does not exist.
    quint64 duplicateTimeline(quint64 uid);
    /// Renames timeline @p uid and its media pool item. One undo step. False if unknown.
    bool renameTimeline(quint64 uid, const QString& name);
    /// Deletes timeline @p uid and its media pool item (one undo step). Nested clips showing
    /// it become offline. The last regular (non-compound) timeline cannot be deleted. When
    /// it is the active timeline, another one is activated first. False if refused.
    bool deleteTimeline(quint64 uid);
    /// Number of Nested clips (in every timeline) that show timeline @p uid.
    [[nodiscard]] int timelineUseCount(quint64 uid) const;
    /// Applies per-timeline format settings to timeline @p uid (one undo step): follow the
    /// project settings, or the given resolution and frame rate.
    bool setTimelineFormat(
        quint64 uid, bool useProjectSettings, int width, int height, Rational fps);
    /// Sets the starting timecode of timeline @p uid (frames of its rate, >= 0; one undo
    /// step "Starting Timecode"; spec 25.12). False if there is no such timeline.
    bool setTimelineStartTimecode(quint64 uid, int64_t frames);
    /// Changes the frame rate of timeline @p uid to @p fps and keeps everything at the same
    /// moment in time (convertTimelineFrameRate()), playhead and zoom included. A timeline that
    /// follows the project settings drags the project along when no other timeline that follows
    /// it has clips (they are converted too); otherwise it leaves the project settings and
    /// keeps the rate as its own. With @p includeCompounds the compound clips (and multicam
    /// clips) it shows, which run at its old rate, are converted as well. One undo step
    /// "Change Frame Rate". False if there is no such timeline or @p fps is invalid.
    bool convertTimelineFrameRate(quint64 uid, Rational fps, bool includeCompounds = false);
    /// Changes the project frame rate to @p fps and converts every timeline that follows the
    /// project settings, content included (one undo step). The other timelines keep their rate.
    /// False if @p fps is invalid.
    bool convertProjectFrameRate(Rational fps);
    /// Generates a free name like "Timeline 2" / "Compound Clip 3".
    [[nodiscard]] QString uniqueTimelineName(const QString& base) const;

    // ---- Nesting / compound clips (ProjectTimelines.cpp) ----

    /// True if a clip showing timeline @p inner may be placed into timeline @p outer (it
    /// would not make a timeline contain itself, directly or indirectly).
    [[nodiscard]] bool canNest(quint64 inner, quint64 outer) const;
    /// Clip > New Compound Clip: collapses clips @p clipIds of the active timeline into a new
    /// compound clip named @p name (media pool item in bin @p binId), see compound::collapse().
    /// One undo step. Returns the uid of the compound timeline, or 0 with @p error set.
    quint64 createCompoundClip(const QVector<quint64>& clipIds, const QString& name,
        const QString& binId = {}, QString* error = nullptr);
    /// Clip > Decompose in Place for the Nested clip @p clipId of the active timeline (and
    /// the clips linked to it that show the same timeline). One undo step. Returns the ids of
    /// the restored clips, or nothing with @p error set.
    std::optional<QVector<quint64>> decomposeCompoundClip(quint64 clipId, QString* error = nullptr);
    /// Looks any timeline of the project up by uid (the active one included); valid while
    /// the project lives. For the pure functions of CompoundOps / Multicam.
    [[nodiscard]] compound::TimelineLookup timelineLookup() const;
    /// The nested-timeline map of a render snapshot of @p data (every timeline its Nested
    /// clips reach, plus the angle views of multicam timelines); @p data need not be a
    /// timeline of the project (the source viewer's private timelines).
    [[nodiscard]] NestedTimelines nestedTimelinesFor(const TimelineData& data) const;

    // ---- Multicam clips (ProjectMulticam.cpp) ----

    /// Create New Multicam Clip Using Selected Clips: builds the multicam timeline from the
    /// synced @p sources (multicam::build(); rate @p fps, picture @p width x @p height) and
    /// its media pool item in bin @p binId. With @p moveSources, the source items go into a
    /// new bin "Original Clips" inside @p binId. One undo step. Returns the uid, or 0 with
    /// @p error set.
    quint64 createMulticamClip(const QVector<multicam::AngleSource>& sources, const QString& name,
        Rational fps, int width, int height, const QString& binId = {}, bool moveSources = false,
        QString* error = nullptr);
    /// Flatten Multicam Clip for the multicam clip @p clipId of the active timeline (and the
    /// parts linked to it): see multicam::flatten(). One undo step.
    std::optional<QVector<quint64>> flattenMulticamClip(quint64 clipId, QString* error = nullptr);

    // ---- Tabs and per-timeline view state (ProjectTimelines.cpp) ----

    /// Timelines open as tabs, in tab order (always contains the active timeline).
    [[nodiscard]] const QVector<quint64>& tabs() const { return m_tabs; }
    /// Opens timeline @p uid in a tab (after the active one) and activates it.
    bool openTab(quint64 uid);
    /// Closes the tab of @p uid; activates the neighbour if it was active. The last tab
    /// cannot be closed (false).
    bool closeTab(quint64 uid);
    /// Moves the tab at @p from to index @p to.
    void moveTab(int from, int to);
    /// Replaces the tab list (stacked timelines swap the tabs of their two panes): unknown
    /// uids and duplicates are dropped, the active timeline is kept (appended if missing).
    void setTabs(const QVector<quint64>& tabs);
    /// Playhead of timeline @p uid (the active one's live playhead; 0 for unknown uids).
    [[nodiscard]] int64_t timelinePlayhead(quint64 uid) const;
    /// Moves the playhead of timeline @p uid (stacked timelines: the inactive pane has its own
    /// playhead). Not an edit; emits inactivePlayheadChanged() for inactive timelines.
    void setTimelinePlayhead(quint64 uid, int64_t frame);
    /// View state (zoom, scroll) of timeline @p uid.
    [[nodiscard]] TimelineViewState viewState(quint64 uid) const;
    /// Remembers the view state of timeline @p uid.
    void setViewState(quint64 uid, const TimelineViewState& state);
    /// View settings of media pool bin @p binKey (the media pool panel's bin keys: "" =
    /// Master, a bin id, "smart:<id>", "power:<id>"); default-constructed when never set.
    [[nodiscard]] BinViewState binViewState(const QString& binKey) const
    {
        return m_binViews.value(binKey);
    }
    /// Remembers the view settings of bin @p binKey (a default state forgets them).
    void setBinViewState(const QString& binKey, const BinViewState& state);

    // ---- Compositing page (ProjectCompositions.cpp) ----

    /// Replaces the composition of clip @p clipId in timeline @p timelineUid (the active one
    /// or any other) as one undo step labelled @p label ("Add Node", "Connect", ...);
    /// std::nullopt removes it (Clip > Reset Composition). Every Compositing page change goes
    /// through here. Inside an open edit (beginEdit(), e.g. a slider drag) it joins that
    /// edit, so repeated calls during a drag make one step. False (nothing changes) for an
    /// unknown timeline or clip or a locked track.
    bool setClipComposition(quint64 timelineUid, quint64 clipId,
        std::optional<Composition> composition, const QString& label);
    /// The composition of clip @p clipId in timeline @p timelineUid (std::nullopt when the
    /// clip has none or does not exist).
    [[nodiscard]] std::optional<Composition> clipComposition(
        quint64 timelineUid, quint64 clipId) const;

    // ---- Color page (ProjectGrades.cpp) ----

    /// Replaces the grade of clip @p clipId in timeline @p timelineUid (the active one or any
    /// other) as one undo step labelled @p label ("Add Serial Node", "Lift", ...); an empty
    /// grade removes it. Every Color page change of a clip goes through here. Inside an open
    /// edit (beginEdit(), e.g. a wheel drag) it joins that edit, so repeated calls during a
    /// drag make one step. False (nothing changes) for an unknown timeline or clip or a locked
    /// track; true without an undo step when the grade is unchanged.
    bool setClipGrade(
        quint64 timelineUid, quint64 clipId, const ClipGrade& grade, const QString& label);
    /// Gives every clip of @p clipIds (in timeline @p timelineUid) @p grade in one undo step
    /// (apply a gallery still, copy a grade to the selected clips). Clips on locked tracks and
    /// unknown ids are skipped. Returns the number of clips changed.
    int setClipGrades(quint64 timelineUid, const QVector<quint64>& clipIds, const ClipGrade& grade,
        const QString& label);
    /// The grade of clip @p clipId (empty when ungraded or unknown).
    [[nodiscard]] ClipGrade clipGrade(quint64 timelineUid, quint64 clipId) const;
    /// Replaces the timeline grade of @p timelineUid as one undo step (same rules as
    /// setClipGrade()).
    bool setTimelineGrade(quint64 timelineUid, const ClipGrade& grade, const QString& label);
    /// The timeline grade of @p timelineUid (empty when ungraded or unknown).
    [[nodiscard]] ClipGrade timelineGrade(quint64 timelineUid) const;

    // ---- Editing protocol ----

    /// Starts an undoable edit labelled @p label (shown in Edit ▸ Undo). Nesting-safe.
    void beginEdit(const QString& label);
    /// Finishes the edit; the outermost call pushes one undo command if anything changed and
    /// notifies the timeline.
    /// The open edit is a value change or follows playback (Inspector, Dynamic Trim): it does not
    /// count as an edit for "After an edit during playback" (spec 48.8), whatever it changes.
    void markEditKeepsPlaying() { m_editKeepsPlaying = true; }
    void endEdit();
    /// Abandons the edit. When the outermost edit closes (by endEdit() or cancelEdit()), the
    /// state from the outermost beginEdit() is restored and nothing is pushed.
    void cancelEdit();
    /// True while an edit is open.
    [[nodiscard]] bool isEditing() const { return m_editDepth > 0; }
    /// Name of the open edit (beginEdit()); empty when none is open.
    [[nodiscard]] QString openEditLabel() const
    {
        return m_editDepth > 0 ? m_editLabel : QString();
    }
    /// Closes an edit that was left open (a gesture whose end never arrived), keeping its
    /// changes as one undo step. Returns its name. Undo calls it when no mouse button is down,
    /// so a lost mouse release can never block Undo for the rest of the session.
    QString closeStaleEdit();

    /// Undoes the last command (ignored while an edit is open).
    void undo();
    /// Redoes the next command (ignored while an edit is open).
    void redo();

    /// Imports files into the media pool as one undoable step. Returns ids + warnings.
    ImportResult importMedia(const QStringList& paths, const QString& binId = {});

    /// Restores the active timeline + media pool state (not undoable itself).
    void restoreState(const TimelineData& timeline, const MediaPoolState& pool);

    /// Every timeline's data, in creation order (undo snapshots; immutable).
    using TimelineSet = std::vector<std::shared_ptr<const TimelineData>>;
    /// Resolution + frame rate of the project (part of the undo state).
    struct Format
    {
        int width = 1920;
        int height = 1080;
        Rational fps {24, 1};
        RetimeProcess retimeProcess = RetimeProcess::Nearest;
        int64_t timelineStart = 0; ///< ProjectSettings::timelineStart
        friend bool operator==(const Format&, const Format&) = default;
    };
    /// Restores a complete state (undo commands). @p primary: the timeline the step was made
    /// in; it is activated first when it exists and is not active.
    void restoreProjectState(const TimelineSet& timelines, const MediaPoolState& pool,
        const Format& format, quint64 primary);

    /// Everything a project file holds, captured as values: the immutable timeline snapshots
    /// (shared, no copy), the implicitly shared media pool state and small settings. Taking
    /// one costs microseconds on the GUI thread; writeSnapshot() turns it into the file on any
    /// thread (the background autosave, AutosaveController).
    struct SaveSnapshot
    {
        /// Per-timeline view and playhead.
        struct View
        {
            quint64 uid = 0;
            int64_t playhead = 0;
            TimelineViewState view;
        };
        QString name;
        Format format;
        MediaPoolState pool;
        TimelineSet timelines; ///< creation order
        QVector<View> views; ///< same order as `timelines`
        quint64 activeUid = 0;
        QVector<quint64> tabs;
        QJsonArray mediaLinks; ///< MediaLinks::toJson() (small; built at capture time)
        QJsonArray gallery; ///< GradeGallery::toJson() (empty = no stills)
        QJsonObject cacheSettings;
        QHash<QString, BinViewState> binViews;
    };
    /// The current state as a SaveSnapshot (GUI thread).
    [[nodiscard]] SaveSnapshot captureSave() const;
    /// Writes @p snapshot to @p path as a project file; media paths are stored relative to
    /// @p path's folder where possible. @p extra becomes the top-level "autosave" object.
    /// Thread-safe: reads only the snapshot. Returns false and sets @p error on failure.
    static bool writeSnapshot(const SaveSnapshot& snapshot, const QString& path,
        QString* error = nullptr, const QJsonObject& extra = {});

signals:
    /// The modified state changed.
    void dirtyChanged(bool dirty);
    /// The project name changed.
    void nameChanged(const QString& name);
    /// A save is about to start (ProjectController): views close the undo groups they keep
    /// open while the user types, so the saved project is clean afterwards.
    void aboutToSave();
    /// The file path changed (save as, load, new).
    void filePathChanged(const QString& path);
    /// An undo step was finished, or one was undone or redone (the timeline is already updated).
    /// @p keepsPlaying: markEditKeepsPlaying() was called for it; the program engine never stops
    /// because of it (spec 48.8).
    void editCommitted(bool keepsPlaying);
    /// The whole project was replaced (new or load); views should rebuild themselves.
    void projectReset();
    /// The active timeline is about to change from @p uid (views save their view state).
    void activeTimelineAboutToChange(quint64 uid);
    /// Timeline @p uid became the active one (Timeline::changed() was emitted as well).
    void activeTimelineChanged(quint64 uid);
    /// A timeline was added, removed or renamed.
    void timelinesChanged();
    /// The tabs (open timelines or their order) changed.
    void tabsChanged();
    /// The playhead of inactive timeline @p uid moved (setTimelinePlayhead()).
    void inactivePlayheadChanged(quint64 uid, int64_t frame);
    /// cacheSettings() changed (also after new/load).
    void cacheSettingsChanged();

private:
    /// One timeline of the project. `data` is null for the active timeline (its data lives
    /// in m_timeline).
    struct TimelineSlot
    {
        quint64 uid = 0;
        std::shared_ptr<const TimelineData> data;
        int64_t playhead = 0;
        TimelineViewState view;
    };

    bool loadFrom(const QString& path, const QString& documentPath, QString* error);
    /// loadFrom() on bytes already read; @p path names the source in messages and the
    /// fallback name, @p documentPath (empty = @p path) is where relative paths resolve.
    bool loadBytes(
        const QByteArray& json, const QString& path, const QString& documentPath, QString* error);
    /// The project file's JSON for @p snapshot written at @p path (pure function).
    [[nodiscard]] static QJsonObject toJson(const SaveSnapshot& snapshot, const QString& path);
    bool fromJson(const QJsonObject& root, int version, const QDir& projectDir,
        const QString& fallbackName, QString* error);
    /// Converts timeline @p uid to @p fps inside an open edit (playhead, zoom, project follow).
    void convertOneTimeline(quint64 uid, Rational fps, bool followProject);
    TimelineSlot* slot(quint64 uid);
    const TimelineSlot* slot(quint64 uid) const;
    const TimelineData* lookup(quint64 uid) const;
    TimelineSet currentSet() const;
    /// Replaces the data of an inactive timeline (or the active one through m_timeline).
    void replaceTimeline(quint64 uid, std::shared_ptr<const TimelineData> data);
    /// Adds a timeline + media pool item (inside an open edit).
    void addTimeline(TimelineData data, const QString& binId);
    /// Derived data after every edit: nested clip lengths, media pool items of timelines.
    void syncDerived();
    /// Applies a set (undo/redo/cancel); emits the change signals.
    void applySet(const TimelineSet& set);
    /// Activates @p uid without storing the current active data (it no longer exists).
    void activateSlot(quint64 uid, bool storeCurrent);
    void resetTimelines(
        std::vector<TimelineSlot> timelineSlots, quint64 active, QVector<quint64> tabs);
    void installResolver();
    void notifyNestedChanges(const TimelineSet& before);
    void setFilePath(const QString& path);
    void setSettingsDirty(bool dirty);
    void emitDirtyIfChanged();

    MediaPool* m_mediaPool = nullptr;
    MediaLinks* m_links = nullptr;
    GradeGallery* m_gallery = nullptr;
    bool m_galleryLoading = false; ///< gallery reset by new / load: not a user change
    MediaCacheSettings m_cacheSettings;
    Timeline* m_timeline = nullptr;
    std::vector<TimelineSlot> m_slots; ///< every timeline, creation order
    quint64 m_activeUid = 0;
    QVector<quint64> m_tabs;
    QHash<QString, BinViewState> m_binViews; ///< media pool bin key -> view settings
    quint64 m_nextTimelineUid = 1;
    Format m_format;
    QUndoStack* m_undoStack = nullptr;
    QString m_name;
    QString m_filePath;
    bool m_settingsDirty = false; ///< non-undoable changes (rename) since the last save
    bool m_lastDirty = false;

    int m_editDepth = 0;
    bool m_editCancelled = false;
    bool m_editKeepsPlaying = false;
    QString m_editLabel;
    TimelineSet m_editBefore;
    MediaPoolState m_editPoolBefore;
    Format m_editFormatBefore;
    quint64 m_editPrimary = 0;

    /// Angle views of multicam timelines (multicam::angleView()), rebuilt when the multicam
    /// timeline's data pointer changes, so render snapshots share them.
    struct AngleViews
    {
        std::shared_ptr<const TimelineData> source;
        QVector<std::shared_ptr<const TimelineData>> views;
    };
    mutable QHash<quint64, AngleViews> m_angleViews;
};

} // namespace kk
