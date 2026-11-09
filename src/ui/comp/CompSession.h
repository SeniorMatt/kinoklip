// SPDX-License-Identifier: GPL-3.0-or-later
// CompSession — the composition open on the Compositing page: which clip, its graph, the node
// selection, what each viewer shows, and every editing command of the node editor.
#pragma once

#include "core/Composition.h"

#include <QObject>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <functional>

namespace kk {

struct AppContext;
struct ClipData;

/// State and commands of the Compositing page, shared by its node editor, viewers, inspector
/// and transport (none of them edit the project directly).
///
/// - **Clip.** openClip() shows a clip's composition. A clip without one shows the default
///   graph (MediaIn1 -> MediaOut1) without storing it: the composition is created by the first
///   edit, so merely viewing changes nothing (spec 42b).
/// - **Edits.** Every command is one undo step through compadapter::setClipComposition();
///   gestures (inspector slider drags) use beginGesture() / modifyLive() / endGesture().
///   The page's view state (viewer nodes, node editor centre and zoom) is written into the
///   composition with the next edit, never as a step of its own.
/// - **Selection** of nodes and notes is UI state (not undoable), pruned on every change.
///
/// Commands are plain methods with explicit arguments (docs/archive/plan.md 5.9b): the widgets only map
/// mouse and keys to them.
///
/// Thread-safety: GUI thread only.
class CompSession : public QObject
{
    Q_OBJECT
public:
    /// Creates a session with no clip open.
    explicit CompSession(AppContext& ctx, QObject* parent = nullptr);

    // ---- Clip ----

    /// The clip the page should open now: the first selected clip on a video track, else the
    /// clip under the playhead on the top-most video track that has one; 0 = none.
    [[nodiscard]] quint64 clipForPage() const;
    /// Opens clip @p clipId of the active timeline (0 closes). Selection: the MediaOut node.
    void openClip(quint64 clipId);
    /// The open clip (0 = none).
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// The open clip's current state, or nullptr.
    [[nodiscard]] const ClipData* clip() const;
    /// The graph shown (the stored composition, or the unsaved default).
    [[nodiscard]] const Composition& composition() const { return m_comp; }
    /// True if the clip has a stored composition (false: the default is only shown).
    [[nodiscard]] bool isStored() const { return m_stored; }
    /// Comp frame of the playhead (clip-local, clamped into the clip; 0 without a clip).
    [[nodiscard]] int64_t compFrame() const;
    /// Number of comp frames (the clip's duration; 0 without a clip).
    [[nodiscard]] int64_t compFrameCount() const;
    /// Pixel size of the frame (the timeline resolution).
    [[nodiscard]] QSize frameSize() const;
    /// Moves the playhead to comp frame @p frame (clamped).
    void seekCompFrame(int64_t frame);

    // ---- Editing ----

    /// Applies @p mutate to a copy of the graph; if it returns true and the graph changed,
    /// stores the result as one undo step labelled @p label. Returns true if stored.
    bool edit(const QString& label, const std::function<bool(Composition&)>& mutate);
    /// Starts a gesture (one undo step until endGesture()).
    void beginGesture(const QString& label);
    /// Applies @p mutate live inside the gesture (views update, no step yet).
    bool modifyLive(const std::function<void(Composition&)>& mutate);
    /// Ends the gesture opened by beginGesture().
    void endGesture();
    /// Removes the clip's composition ("Reset Composition", one undo step).
    bool resetComposition();

    // ---- Node commands (each one undo step; false when nothing happened) ----

    /// Adds a node of @p type after the current node (connected into its output), or
    /// unconnected at the node editor's centre without a selection. Selects it.
    quint64 addNode(const QString& type);
    /// Adds a node of @p type at @p position (grid units); with @p linkTo non-zero it is
    /// inserted into the link ending in input @p linkPort of @p linkTo. Selects it.
    quint64 addNodeAt(
        const QString& type, QPointF position, quint64 linkTo = 0, const QString& linkPort = {});
    /// Adds one unconnected MediaIn node per media pool item of @p mediaIds (those that
    /// canReadIntoMediaIn()), the first at @p position (grid units), the others below it: the
    /// items dropped on the node editor (spec 42b.30). One undo step; selects them, the last
    /// one current. Returns the new node ids.
    QVector<quint64> addMediaIns(const QStringList& mediaIds, QPointF position);
    /// Makes MediaIn node @p id read media pool item @p mediaId (empty: the clip's own
    /// picture). One undo step; false for other nodes, locked nodes or no change.
    bool setMediaInSource(quint64 id, const QString& mediaId);
    /// True if media pool item @p mediaId can be read by a MediaIn node: a media file with a
    /// picture (video or still), not a timeline, subtitle or audio-only item.
    [[nodiscard]] bool canReadIntoMediaIn(const QString& mediaId) const;
    /// Adds a note at @p position (grid units). Returns its id.
    quint64 addNote(QPointF position, const QString& text = {});
    /// Changes the text of note @p id.
    bool setNoteText(quint64 id, const QString& text);
    /// Connects output 0 of @p fromId to input @p port of @p toId.
    bool connectNodes(quint64 fromId, quint64 toId, const QString& port);
    /// Disconnects input @p port of @p toId.
    bool disconnectInput(quint64 toId, const QString& port);
    /// Moves the selected nodes and notes by @p delta (grid units), then, when exactly one
    /// unconnected node moved and @p insertTo is set, inserts it into that link: one step.
    bool moveSelection(QPointF delta, quint64 insertTo = 0, const QString& insertPort = {});
    /// Deletes the selected nodes (MediaOut stays) and notes.
    bool deleteSelection();
    /// Copies the selected nodes to the node clipboard.
    void copySelection();
    /// Copies, then deletes the selection.
    bool cutSelection();
    /// True if the node clipboard holds nodes.
    [[nodiscard]] static bool canPaste();
    /// Pastes the node clipboard at @p position (grid units; top-left of the pasted group)
    /// and selects the pasted nodes.
    bool paste(QPointF position);
    /// Duplicates the selection next to it.
    bool duplicateSelection();
    /// Renames node @p id.
    bool renameNode(quint64 id, const QString& name);
    /// Toggles pass-through of the selected nodes (all on if any is off).
    bool togglePassThrough();
    /// Sets pass-through (enabled = !passThrough) of node @p id.
    bool setNodeEnabled(quint64 id, bool enabled);
    /// Toggles the lock of the selected nodes.
    bool toggleLock();
    /// Swaps background and foreground of the current node (Merge / Dissolve).
    bool swapInputs();

    // ---- Selection (UI state) ----

    [[nodiscard]] const QVector<quint64>& selectedNodes() const { return m_selNodes; }
    [[nodiscard]] const QVector<quint64>& selectedNotes() const { return m_selNotes; }
    /// The node the inspector shows (last clicked of the selection; 0 = none).
    [[nodiscard]] quint64 currentNode() const { return m_current; }
    /// Replaces the selection. @p current must be in @p nodes (else the last one is used).
    void setSelection(
        const QVector<quint64>& nodes, const QVector<quint64>& notes = {}, quint64 current = 0);
    /// Selects every node.
    void selectAll();

    // ---- Viewers and view state (UI state, saved with the next edit) ----

    /// Node shown in viewer @p index (0 left, 1 right). The right viewer defaults to MediaOut;
    /// 0 = empty.
    [[nodiscard]] quint64 viewerNode(int index) const;
    /// Shows node @p nodeId in viewer @p index (0 clears).
    void setViewerNode(int index, quint64 nodeId);
    /// Remembers the node editor's view (saved in the composition with the next edit).
    void setViewState(QPointF centre, double zoom);

    /// Grid snapping of moved nodes (node editor option).
    [[nodiscard]] bool snapToGrid() const { return m_snap; }
    void setSnapToGrid(bool on) { m_snap = on; }

signals:
    /// Another clip was opened (or the open one was closed / removed).
    void clipChanged();
    /// The graph changed (edit, undo, live gesture, another clip).
    void compositionChanged();
    /// The node / note selection or the current node changed.
    void selectionChanged();
    /// What a viewer shows changed.
    void viewersChanged();

private:
    void reload();
    void pruneSelection();
    /// Copies the UI view state into @p comp before it is stored.
    void stampViewState(Composition& comp) const;

    AppContext& m_ctx;
    quint64 m_clipId = 0;
    Composition m_comp;
    bool m_stored = false;
    bool m_snap = false;
    QVector<quint64> m_selNodes, m_selNotes;
    quint64 m_current = 0;
    quint64 m_viewer[2] = {0, 0};
    QPointF m_viewCentre;
    double m_viewZoom = 1.0;
    int m_gestureDepth = 0;
};

} // namespace kk
