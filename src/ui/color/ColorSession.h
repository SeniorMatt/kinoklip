// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSession — the grade open on the Color page: which clip (or the timeline grade), the
// selected node, and every Color page command as one undo step (Project::setClipGrade() /
// setTimelineGrade()). The widgets only map input to it; the graph maths is core/Grade.h.
#pragma once

#include "core/Grade.h"

#include <QObject>
#include <QPointer>
#include <QString>

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

    // ---- Commands (each one undo step) ----

    /// Replaces the grade (undo label @p label). False when nothing is open or the edit is
    /// refused (locked track).
    bool commit(const ClipGrade& grade, const QString& label);
    /// Alt+S / Alt+P / Alt+L: a corrector after / parallel to / layered over the selected node
    /// (an ungraded clip gets its first node). The new node becomes selected. Returns its id.
    quint64 addSerialNode();
    quint64 addParallelNode();
    quint64 addLayerNode();
    /// Shift+S: a corrector before the selected node (the first node of an ungraded clip).
    quint64 addSerialNodeBefore();
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
    /// Replaces curve @p curve of node @p nodeId (0: selected).
    bool setCurve(
        quint64 nodeId, GradeCurve curve, const CurvePoints& points, const QString& label);
    /// Several curves of one node in one commit (linked R, G, B editing, Reset).
    bool setCurves(quint64 nodeId, const QVector<std::pair<GradeCurve, CurvePoints>>& curves,
        const QString& label);
    /// Replaces the qualifier of node @p nodeId (0: selected).
    bool setQualifier(quint64 nodeId, const GradeQualifier& qualifier, const QString& label);
    /// Sets the LUT of node @p nodeId (0: selected; empty path removes it).
    bool setLut(quint64 nodeId, const QString& path);

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
    /// Replaces the polygon points of window @p windowId (0: selected).
    bool setWindowPoints(quint64 windowId, const QVector<QPointF>& points, const QString& label);
    /// Sets the enabled / invert flags of window @p windowId (0: selected).
    bool setWindowFlags(quint64 windowId, bool enabled, bool invert);

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
    /// Copies the grade of clip @p fromClipId to clips @p toClipIds (empty: the current clip),
    /// one undo step ("copy grade" between clips). Returns the number of clips changed.
    int copyGrade(quint64 fromClipId, const QVector<quint64>& toClipIds = {});

Q_SIGNALS:
    /// Another clip, the target or the playhead's clip changed.
    void clipChanged();
    /// The grade changed (an edit, undo / redo, another clip).
    void gradeChanged();
    void selectedNodeChanged(quint64 id);
    void selectedWindowChanged(quint64 id);
    void selectedClipsChanged();
    void bypassAllChanged(bool bypass);

private:
    void reload();
    /// The grade as it would be after making sure it has a node (for setParam on an empty
    /// grade); @p node gets the node to edit.
    ClipGrade editableGrade(quint64 wanted, quint64* node) const;
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
    ClipGrade m_grade;
    int m_gesture = 0;
    quint64 m_selectedWindow = 0;
    QVector<quint64> m_selectedClips;
    ClipGrade m_clipboard;
    bool m_hasClipboard = false;
};

} // namespace kk
