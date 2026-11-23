// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSession — the grade open on the Color page: which clip (or the timeline grade), the
// selected node, and every Color page command as one undo step (Project::setClipGrade() /
// setTimelineGrade()). The widgets only map input to it; the graph maths is core/Grade.h.
#pragma once

#include "core/ColorMatch.h"
#include "core/Grade.h"
#include "core/KeyRows.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>

#include <algorithm>
#include <cstdint>

namespace kk {

struct AppContext;

/// The Color page's model of "what is being graded".
///
/// Clip: the Color page grades the clip under the playhead on the highest enabled video
/// track that has one (the Resolve habit: move the playhead to grade another clip);
/// openClip() pins another one until the playhead leaves it. Target::Timeline edits the
/// active timeline's grade instead (applied after every clip grade).
///
/// Edits: commit() replaces the grade as one undo step; a drag wraps its many commits in
/// beginGesture() / endGesture() so it is one step too. Parameter edits at the playhead follow
/// AnimatedValue::setValue() (a keyframed parameter gets a key at the current frame).
///
/// Thread-safety: GUI thread only.
class ColorSession : public QObject
{
    Q_OBJECT
public:
    enum class Target {
        Clip,
        Timeline
    };

    explicit ColorSession(AppContext& ctx, QObject* parent = nullptr);

    // ---- What is graded ----

    [[nodiscard]] Target target() const { return m_target; }
    void setTarget(Target target);
    /// The graded clip (0: none, or the timeline grade is the target).
    [[nodiscard]] quint64 clipId() const { return m_target == Target::Clip ? m_clipId : 0; }
    /// The clip the page would show at the playhead (see the class doc).
    [[nodiscard]] quint64 clipAtPlayhead() const;
    /// Shows clip @p clipId (0: the clip at the playhead).
    void openClip(quint64 clipId);
    /// True when there is something to grade (a clip, or the timeline target).
    [[nodiscard]] bool hasGrade() const { return m_target == Target::Timeline || m_clipId != 0; }
    /// The current grade (empty when ungraded or nothing is open).
    [[nodiscard]] const ClipGrade& grade() const { return m_grade; }
    /// The frame the grade's keyframes are evaluated at: clip-local frame of the playhead for
    /// a clip, the playhead for the timeline grade.
    [[nodiscard]] int64_t gradeFrame() const;

    // ---- Nodes ----

    /// The selected node (0: none). Commands without a node act on it.
    [[nodiscard]] quint64 selectedNode() const { return m_selected; }
    void selectNode(quint64 id);
    /// The selected node, or nullptr.
    [[nodiscard]] const GradeNode* currentNode() const { return m_grade.node(m_selected); }
    /// Every selected node: the current node first, then the ones added with Ctrl+click or a
    /// rubber band (Create Compound Node groups them).
    [[nodiscard]] QVector<quint64> selectedNodes() const;
    /// Adds node @p id to the selection, or takes it out (the current node moves on).
    void toggleNodeSelection(quint64 id);
    /// Selects @p ids (the first one becomes the current node).
    void selectNodes(const QVector<quint64>& ids);

    // ---- Compound nodes ----

    /// The Compound node the node editor shows the inside of (0 = the top level).
    [[nodiscard]] quint64 openCompoundId() const { return m_level; }
    /// Shows the inside of Compound node @p id (0: the selected one). False if there is none.
    bool openCompound(quint64 id = 0);
    /// Back to the level the open Compound node sits on. False at the top level.
    bool closeCompound();
    /// The picture the open Compound node's members read (0 = the grade input at the top).
    [[nodiscard]] quint64 levelSource() const;
    /// Groups the selected nodes into a Compound node (Create Compound Node). Returns its id.
    quint64 groupNodes();
    /// Dissolves Compound node @p id (0: the selected one) into its members.
    bool ungroupNode(quint64 id = 0);

    // ---- Commands (each one undo step) ----

    /// Replaces the grade (undo label @p label). False when nothing is open or the edit is
    /// refused (locked track).
    bool commit(const ClipGrade& grade, const QString& label);
    /// Alt+S / Alt+P / Alt+L: a corrector after / parallel to / layered over the selected node.
    /// An ungraded clip shows node 01 as Resolve does (the node editor draws it): it gets that
    /// node and the new one after / beside it. The new node becomes selected. Returns its id.
    quint64 addSerialNode();
    quint64 addParallelNode();
    quint64 addLayerNode();
    /// Shift+S: a corrector before the selected node (before node 01 of an ungraded clip).
    quint64 addSerialNodeBefore();
    /// Alt+O: an Outside node after the selected node: a corrector keyed with the inverse of
    /// the selected node's key.
    quint64 addOutsideNode();
    /// A Key Mixer fed by the selected node's key.
    quint64 addKeyMixer();
    /// A Splitter and a Combiner after the selected node (red, green and blue wired through).
    quint64 addSplitterCombiner();
    /// The Alpha Output, keyed by the selected node (an existing one is re-connected).
    quint64 addAlphaOutput();
    /// Node editor: connects the key output of @p from to the key input of @p to (input
    /// @p inputIndex of a Key Mixer); see gradeops::connectKey().
    bool connectKeys(quint64 from, quint64 to, int inputIndex, bool invert = false);
    /// Node editor: removes the key link into @p to (Key Mixer input @p inputIndex).
    bool disconnectKeyInput(quint64 to, int inputIndex);
    /// Inverts the key link into @p to (Key Mixer input @p inputIndex), or sets @p invert.
    bool setKeyInverted(quint64 to, int inputIndex, bool invert);
    /// Sets the gain of Key Mixer @p mixer's input @p inputIndex.
    bool setKeyGain(quint64 mixer, int inputIndex, double gain);
    /// Node editor, inside a Compound node: makes @p from (a member) the compound's output.
    bool setCompoundOutput(quint64 from);
    /// Node editor: connects node @p from (0 = the source picture) to input @p inputIndex of
    /// @p to (0 = the grade output); see gradeops::connect(). One undo step.
    bool connectNodes(quint64 from, quint64 to, int inputIndex);
    /// Node editor: removes input @p inputIndex of @p to (gradeops::disconnect()).
    bool disconnectInput(quint64 to, int inputIndex);
    /// Node editor: moves nodes to new editor positions (GradeNode::pos), one undo step.
    bool moveNodes(const QVector<std::pair<quint64, QPointF>>& positions);
    /// Deletes node @p id (0: the selected one).
    bool removeNode(quint64 id = 0);
    /// Ctrl+D: bypasses or enables node @p id (0: the selected one).
    bool toggleNodeBypass(quint64 id = 0);
    /// Resets the corrections of node @p id (0: the selected one).
    bool resetNode(quint64 id = 0);
    /// Removes the whole grade.
    bool resetGrade();
    /// Sets parameter @p key of node @p nodeId (0: selected; a missing grade gets its first
    /// node) at the current frame. Inside a gesture several calls make one step.
    bool setParam(quint64 nodeId, const QString& key, double value, const QString& label);
    /// Several parameters of one node in one commit (a wheel's R, G and B).
    bool setParams(
        quint64 nodeId, const QVector<std::pair<QString, double>>& values, const QString& label);
    /// Generate 3D LUT (CUBE): writes the grade of clip @p clipId as a @p size point .cube file
    /// (render::writeGradeCube() at the playhead's frame of that clip, or its first frame).
    /// @return false with a reason in @p error (no grade, unwritable path).
    bool generateLut(quint64 clipId, int size, const QString& path, QString* error = nullptr);
    /// Auto Color on node @p nodeId (0: selected): sets its per-channel lift, gamma and gain from
    /// @p picture (the node's input), grademath::autoColor(), in one undo step. False without
    /// a grade, with a mixer, or when the picture is too flat to analyse.
    bool autoColor(quint64 nodeId, const QImage& picture);
    /// Color Match palette: writes the chart match @p match into node @p nodeId (0: selected) as
    /// its Primaries lift / gamma / gain and RGB Mixer matrix (colormatch::applyChartToNode()),
    /// one undo step. False without a grade or with a mixer node.
    bool applyChartMatch(quint64 nodeId, const colormatch::ChartMatch& match, const QString& label);
    /// How far Shot Match goes (0..1; the Color Match palette's Strength).
    [[nodiscard]] double shotMatchStrength() const { return m_shotStrength; }
    void setShotMatchStrength(double strength) { m_shotStrength = std::clamp(strength, 0.0, 1.0); }
    /// Replaces curve @p curve of node @p nodeId (0: selected).
    bool setCurve(
        quint64 nodeId, GradeCurve curve, const CurvePoints& points, const QString& label);
    /// Several curves of one node in one commit (linked R, G, B editing, Reset).
    bool setCurves(quint64 nodeId, const QVector<std::pair<GradeCurve, CurvePoints>>& curves,
        const QString& label);
    /// Replaces the qualifier of node @p nodeId (0: selected).
    bool setQualifier(quint64 nodeId, const GradeQualifier& qualifier, const QString& label);
    /// Replaces the Color Warper of node @p nodeId (0: selected); a pin drag calls this between
    /// beginGesture() and endGesture(), so the whole drag is one undo step.
    bool setWarp(quint64 nodeId, const GradeWarp& warp, const QString& label);
    /// Sets the LUT of node @p nodeId (0: selected; empty path removes it).
    bool setLut(quint64 nodeId, const QString& path);
    /// Color > Add / Remove Keyframe: keys every parameter of node @p nodeId (0: selected; an
    /// ungraded clip gets node 01) and of its windows at the current frame with its value
    /// there, as a Resolve corrector keyframe does; later edits at other frames then add keys
    /// (AnimatedValue::setValue()). When the node already has a key at this frame its keys at
    /// this frame are removed instead. One undo step.
    bool toggleKeyframe(quint64 nodeId = 0);
    /// True if node @p nodeId (0: selected) or one of its windows has a key at the current
    /// frame.
    [[nodiscard]] bool hasKeyframe(quint64 nodeId = 0) const;

    // ---- Keyframes palette: one row per corrector node plus Input / Output Sizing ----

    /// One keyframe row of the Keyframes palette.
    struct KeyRow
    {
        enum class Kind {
            Node, ///< a corrector node of the grade (and its windows)
            Input, ///< Input Sizing: the clip's transform (Inspector parameters)
            Output ///< Output Sizing: the timeline's
        };
        Kind kind = Kind::Node;
        quint64 nodeId = 0; ///< Node rows only
        friend bool operator==(const KeyRow&, const KeyRow&) = default;
    };
    /// The rows in display order: every corrector of the grade, then Input Sizing (a clip is
    /// open) and Output Sizing.
    [[nodiscard]] QVector<KeyRow> keyRows() const;
    /// Frames the strip shows: clip-local for a clip, timeline frames for the timeline target.
    /// False when nothing is open.
    bool stripRange(int64_t* firstFrame, int64_t* lastFrame) const;
    /// The playhead in strip frames (gradeFrame()).
    [[nodiscard]] int64_t stripPlayhead() const { return gradeFrame(); }
    /// Moves the playhead to strip frame @p frame (clamped into the range).
    void seekStrip(int64_t frame);
    /// Strip frames of the keys of @p row, sorted.
    [[nodiscard]] QVector<int64_t> rowFrames(const KeyRow& row) const;
    [[nodiscard]] bool rowHasKey(const KeyRow& row, int64_t frame) const;
    /// Static or Dynamic (see core/KeyRows.h) and the dissolve type of the Dynamic key.
    [[nodiscard]] keyrows::Mode rowKeyMode(const KeyRow& row, int64_t frame) const;
    [[nodiscard]] Interp rowKeyInterp(const KeyRow& row, int64_t frame) const;
    /// Keys every parameter of @p row at strip frame @p frame. Each of these is one undo step.
    bool addRowKey(const KeyRow& row, int64_t frame);
    bool deleteRowKey(const KeyRow& row, int64_t frame);
    /// Moves the key of @p row from strip frame @p from to @p to.
    bool moveRowKey(const KeyRow& row, int64_t from, int64_t to);
    bool setRowKeyMode(const KeyRow& row, int64_t frame, keyrows::Mode mode);
    /// Dissolve type: Linear, EaseIn, EaseOut, EaseInOut or Smooth.
    bool setRowKeyInterp(const KeyRow& row, int64_t frame, Interp interp);
    /// Auto Keyframe (the palette's toggle): while on, an edit of a parameter at the playhead
    /// sets a key there even when the parameter has none yet. Off by default.
    [[nodiscard]] bool autoKeyframe() const { return m_autoKey; }
    void setAutoKeyframe(bool on);

    // ---- Sizing palette ----

    enum class SizingScope {
        Input, ///< the open clip's transform: the Inspector's own parameters
        Output ///< the timeline's Output Sizing
    };
    /// True when the scope can be edited (Input needs an open clip).
    [[nodiscard]] bool hasSizing(SizingScope scope) const;
    /// The parameters of the scope (keys::PosX, ZoomX, ... and the Flip statics).
    [[nodiscard]] ParamSet sizingOf(SizingScope scope) const;
    /// The frame the scope's keys are evaluated at: clip-local for Input, the timeline's for
    /// Output.
    [[nodiscard]] int64_t sizingFrame(SizingScope scope) const;
    /// Sets transform parameters (keys::PosX ...) at the playhead; one undo step (inside a
    /// gesture they join it). A key is set when Auto Keyframe is on or the parameter is keyed.
    bool setSizing(SizingScope scope, const QVector<std::pair<QString, double>>& values,
        const QString& label);
    /// Sets Zoom (Width and Height by the same factor).
    bool setSizingZoom(SizingScope scope, double zoom, const QString& label);
    /// Sets keys::FlipH or keys::FlipV.
    bool setSizingFlip(SizingScope scope, const QString& key, bool on, const QString& label);
    /// Sets the whole scope back (keys and flips removed).
    bool resetSizing(SizingScope scope);

    // ---- Windows (of the selected node) ----

    /// Adds a window of @p shape to the selected corrector (an ungraded clip gets its first
    /// node) and selects it. Returns its id (0 when refused: a mixer, or the node is full).
    quint64 addWindow(GradeWindowShape shape);
    /// Removes window @p windowId (0: the selected window).
    bool removeWindow(quint64 windowId = 0);
    /// The selected window of the selected node (0: none).
    [[nodiscard]] quint64 selectedWindow() const { return m_selectedWindow; }
    void selectWindow(quint64 windowId);
    /// The window @p windowId (0: selected) of the grade, or nullptr.
    [[nodiscard]] const GradeWindow* window(quint64 windowId = 0) const;
    /// Sets parameters of window @p windowId (0: selected) at the current frame.
    bool setWindowParams(
        quint64 windowId, const QVector<std::pair<QString, double>>& values, const QString& label);
    /// Replaces the Polygon / Curve shape of window @p windowId (0: selected): a key at the
    /// current frame when the window already has shape keys, else its static shape. The
    /// point count must stay the same (use addWindowPoint / removeWindowPoint).
    bool setWindowShape(
        quint64 windowId, const GradeWindowShapePoints& points, const QString& label);
    /// Inserts a point after @p index at parameter @p t of that span, in the static shape and
    /// in every shape key (the outline does not change).
    bool addWindowPoint(quint64 windowId, int index, double t, const QString& label);
    /// Removes point @p index from the static shape and from every shape key (three stay).
    bool removeWindowPoint(quint64 windowId, int index, const QString& label);
    /// Sets a shape key at the current frame holding the shape shown there.
    bool keyWindowShape(quint64 windowId = 0);
    /// Removes the shape key at the current frame (the shape goes on between its neighbours).
    bool removeWindowShapeKey(quint64 windowId = 0);
    /// Removes every shape key; the static shape becomes the one shown at the current frame.
    bool clearWindowShapeKeys(quint64 windowId = 0);
    /// Sets the enabled / invert flags of window @p windowId (0: selected).
    bool setWindowFlags(quint64 windowId, bool enabled, bool invert);

    // ---- Tracker data (the Tracker palette) ----

    /// The tracker data of the selected node (empty when there is none).
    [[nodiscard]] GradeTrack track() const;
    /// Replaces the tracker data of node @p nodeId (0: selected), one undo step labelled
    /// @p label. An ungraded clip gets node 01.
    bool setTrack(quint64 nodeId, const GradeTrack& track, const QString& label);
    /// Removes the tracked frames of the selected node and keeps its checkboxes.
    bool clearTrack();
    /// What a track made now covers: the grade frames the clip (or the timeline) has and the
    /// number to add to a grade frame to get the timeline frame. False without a grade.
    bool trackRange(int64_t* firstFrame, int64_t* lastFrame, int64_t* timelineOffset) const;
    /// Picture width / height the windows are graded on (the timeline frame).
    [[nodiscard]] double pictureAspect() const;

    // ---- Clips strip ----

    /// Every graded-able clip of the timeline (video tracks, not subtitles) in timeline order
    /// (start, then track bottom to top): the Color page's thumbnail strip.
    [[nodiscard]] QVector<quint64> stripClips() const;
    /// Grades clip @p clipId: moves the playhead into it (to its first frame when it is not
    /// there yet) and opens it even when a higher track covers it.
    void gradeClip(quint64 clipId);
    /// Clips selected in the strip (paste targets); empty = the current clip.
    [[nodiscard]] const QVector<quint64>& selectedClips() const { return m_selectedClips; }
    void setSelectedClips(const QVector<quint64>& clipIds);
    /// Ctrl+C: remembers the current grade. False when nothing is open.
    bool copyCurrentGrade();
    /// True if copyCurrentGrade() holds a grade.
    [[nodiscard]] bool hasCopiedGrade() const { return m_hasClipboard; }
    /// Ctrl+V: gives the copied grade to @p clipIds (empty: the strip selection, else the
    /// current clip), one undo step. Returns the number of clips changed.
    int pasteGrade(const QVector<quint64>& clipIds = {});

    /// Starts / ends a drag: the commits in between are one undo step labelled @p label.
    void beginGesture(const QString& label);
    void endGesture();

    // ---- Bypass all, gallery ----

    /// Shift+D: the viewers show the pictures without grades (PlaybackEngine::setBypassGrades).
    [[nodiscard]] bool bypassAll() const;
    void setBypassAll(bool bypass);
    /// Ctrl+Alt+G: adds a gallery still of the current frame (the viewer's picture) with the
    /// current grade. Returns the still id (0 when nothing is open).
    quint64 grabStill();
    /// Gives clips @p clipIds (empty: the current clip) the grade of still @p stillId, one undo
    /// step. Returns the number of clips changed.
    int applyStill(quint64 stillId, const QVector<quint64>& clipIds = {});
    /// Gives clips @p clipIds (empty: the current clip; the timeline grade while the timeline is
    /// the target) @p grade as one undo step labelled @p label: replacing their grade, or with
    /// @p append adding its node graph after the nodes they have (Append Node Graph). Returns
    /// the number of clips changed.
    int applyGrade(const ClipGrade& grade, const QVector<quint64>& clipIds, const QString& label,
        bool append = false);
    /// Append Node Graph: applyGrade() of still @p stillId with append.
    int appendStill(quint64 stillId, const QVector<quint64>& clipIds = {});
    /// The picture of the current frame as the viewer shows it (null when nothing is open or the
    /// render is not ready in time).
    [[nodiscard]] QImage currentPicture();
    /// Saves the current grade into gallery memory @p index (0 = A, at most 25); false when
    /// there is nothing to save.
    bool saveMemory(int index);
    /// Gives clips @p clipIds (empty: the current clip) the grade of memory @p index, one undo
    /// step. Returns the number of clips changed (0 for an empty memory).
    int applyMemory(int index, const QVector<quint64>& clipIds = {});
    /// Copies the grade of clip @p fromClipId to clips @p toClipIds (empty: the current clip),
    /// one undo step ("copy grade" between clips). Returns the number of clips changed.
    int copyGrade(quint64 fromClipId, const QVector<quint64>& toClipIds = {});

Q_SIGNALS:
    /// Another clip, the target or the playhead's clip changed.
    void clipChanged();
    /// The grade changed (an edit, undo / redo, another clip, or the playhead moved over a
    /// keyframed grade: its values at the playhead changed).
    void gradeChanged();
    void selectedNodeChanged(quint64 id);
    /// The set of selected nodes or the open Compound node changed.
    void selectionChanged();
    void openCompoundChanged(quint64 id);
    void selectedWindowChanged(quint64 id);
    void selectedClipsChanged();
    void bypassAllChanged(bool bypass);
    /// Input / Output Sizing changed (an edit, undo, another clip, or the playhead moved).
    void sizingChanged();
    void autoKeyframeChanged(bool on);

private:
    void reload();
    [[nodiscard]] int64_t gradeFrameOf(quint64 clipId) const;
    /// The grade as it would be after making sure it has a node (for setParam on an empty
    /// grade); @p node gets the node to edit.
    ClipGrade editableGrade(quint64 wanted, quint64* node) const;
    /// The grade with node 01 when it is empty (@p node then gets its id, else stays).
    ClipGrade withFirstNode(quint64* node) const;
    /// Edits window @p windowId (0: selected) of a copy of the grade with @p edit, commits.
    template<typename Fn>
    bool editWindow(quint64 windowId, const QString& label, Fn&& edit);
    /// Keeps m_selectedWindow on a window of the selected node.
    void fixWindowSelection();

    AppContext& m_ctx;
    Target m_target = Target::Clip;
    quint64 m_clipId = 0;
    quint64 m_pinned = 0; ///< openClip() choice while the playhead stays on it
    quint64 m_selected = 0;
    QVector<quint64> m_extraSelected; ///< selected besides m_selected
    quint64 m_level = 0; ///< the open Compound node (0 = the top level)
    ClipGrade m_grade;
    int64_t m_frame = 0; ///< gradeFrame() at the last reload()
    int m_gesture = 0;
    quint64 m_selectedWindow = 0;
    QVector<quint64> m_selectedClips;
    double m_shotStrength = 1.0;
    ClipGrade m_clipboard;
    bool m_hasClipboard = false;
    bool m_autoKey = false;
    ParamSet m_inputSizing; ///< of the open clip at the last reload()
    ParamSet m_outputSizing;
    int64_t m_playhead = -1;
    /// Number to add to a strip frame of @p row to get the frame its keys are stored at.
    [[nodiscard]] int64_t rowOffset(const KeyRow& row) const;
    /// Applies @p edit(target) to the row (a GradeNode or the sizing ParamSet; frames are the
    /// stored ones) and commits the result as @p label. @p edit returns the number of changes.
    template<typename Fn>
    bool editRow(const KeyRow& row, const QString& label, Fn&& edit);
    bool commitSizing(SizingScope scope, const ParamSet& params, const QString& label);
};

} // namespace kk
