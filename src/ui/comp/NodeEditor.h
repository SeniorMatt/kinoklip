// SPDX-License-Identifier: GPL-3.0-or-later
// NodeEditor — the Compositing page's node graph canvas: tiles, ports, links and notes, with
// pan / zoom, selection, moving, connecting and the node editor keys.
#pragma once

#include "ui/comp/CompAdapter.h"

#include "render/RenderTypes.h"

#include <QFont>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QFontMetricsF>
#include <QLine>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <future>
#include <vector>

class QLineEdit;
class QMenu;
class QPlainTextEdit;

namespace kk {

class ActionRegistry;
class CompSession;

/// Custom-painted node graph view of a CompSession.
///
/// **Why a QWidget and not QGraphicsView.** The graph is a value (Composition) owned by the
/// project and replaced on every edit and undo. A QGraphicsScene would mirror it in a second,
/// mutable item tree that has to be rebuilt or diffed after each change; painting straight from
/// the value needs no synchronisation, keeps one source of truth and is faster for this size
/// of scene: one pass that culls tiles outside the view, no per-item virtual dispatch or BSP
/// index upkeep. test_nodeeditor measures 200 nodes (paint time printed and bounded).
///
/// **Geometry.** Node positions are grid units (1 unit = one tile height = kUnitPx pixels at
/// zoom 1); position is the tile's top-left corner. The main image input sits in the middle
/// of the left edge, other inputs (foreground, masks) along the top edge, the output in the
/// middle of the right edge. Links are cubic curves coloured by the role of the input they
/// end in (background, foreground, mask).
///
/// **Gestures** (each ends in one CompSession command = one undo step): click / Ctrl / Shift
/// click select, drag on the background box-selects, drag a tile moves the selection (a single
/// unconnected node dropped on a link is inserted into it), drag from a port connects, drag a
/// connected input's end away disconnects (or reconnects elsewhere), middle-drag or Space-drag
/// pans, the wheel zooms around the cursor. Dragging a tile out of the widget starts a drag of
/// the node (mime::CompNode) for the viewers. Nodes library entries (mime::CompNodeType) can be
/// dropped onto the canvas or onto a link. Shift-dragging a connected tile onto a link
/// inserts it (it leaves its place); Shift-dragging on a link disconnects it.
///
/// **Keys** are the comp.* commands of the ActionRegistry (keymap profile). They are widget
/// shortcuts: while the editor has focus it claims their keys through QEvent::ShortcutOverride
/// and triggers the action, so Ctrl+C here copies nodes while elsewhere it copies clips.
///
/// Thread-safety: GUI thread only.
class NodeEditor : public QWidget
{
    Q_OBJECT
public:
    /// Pixels per grid unit at zoom 1 (= tile height).
    static constexpr double kUnitPx = 30.0;
    /// Tile width in grid units.
    static constexpr double kTileWidth = 3.4;
    /// Zoom limits.
    static constexpr double kMinZoom = 0.1;
    static constexpr double kMaxZoom = 4.0;

    /// What is under a point.
    struct Hit
    {
        enum class Kind {
            None,
            Tile,
            Input, ///< an input port (port = its id)
            Output, ///< the output port
            Link, ///< a link (node = the node whose input it ends in, port = that input)
            Note
        };
        Kind kind = Kind::None;
        quint64 node = 0; ///< node id (Tile, Input, Output, Link)
        QString port;
        quint64 note = 0; ///< note id (Note)
    };

    /// Creates an editor showing @p session (not owned); keys from @p actions (may be null).
    NodeEditor(CompSession* session, ActionRegistry* actions, QWidget* parent = nullptr);

    // ---- View ----

    /// Grid point shown at the widget centre.
    [[nodiscard]] QPointF centre() const { return m_centre; }
    /// Pixels per grid unit / kUnitPx.
    [[nodiscard]] double zoom() const { return m_zoom; }
    /// Sets the view (zoom clamped to kMinZoom..kMaxZoom).
    void setView(QPointF centre, double zoom);
    /// Zooms by @p factor keeping the grid point under @p anchor (widget pixels) in place.
    void zoomAt(QPointF anchor, double factor);
    /// Shows every node and note (Fit, Ctrl+F); zoom at most 1.
    void fitToView();
    /// Widget pixel -> grid units.
    [[nodiscard]] QPointF toGrid(QPointF widgetPos) const;
    /// Grid units -> widget pixel.
    [[nodiscard]] QPointF fromGrid(QPointF gridPos) const;
    /// Grid point under the mouse when it is inside the widget, else the view centre (paste
    /// and dropped nodes go there).
    [[nodiscard]] QPointF pastePosition() const;

    // ---- Geometry (widget pixels; public for tests and the viewers' drop logic) ----

    /// Tile rectangle of @p node.
    [[nodiscard]] QRectF tileRect(const CompNode& node) const;
    /// Centre of input @p port of @p node (null point if the type has no such port).
    [[nodiscard]] QPointF inputPos(const CompNode& node, const QString& port) const;
    /// Centre of the output port of @p node.
    [[nodiscard]] QPointF outputPos(const CompNode& node) const;
    /// Edge of input @p port of @p node, facing its source like in node compositors: 0 left,
    /// -1 top, 1 bottom. The main input is on the left unless its source sits to the right
    /// (then top or bottom, whichever faces it); side inputs (foreground, masks) are on the
    /// top unless their source sits clearly below the tile. Links then never loop over or
    /// behind the tile.
    [[nodiscard]] int inputSide(const CompNode& node, const QString& port) const;
    /// Topmost item at @p pos.
    [[nodiscard]] Hit hitTest(QPointF pos) const;

    // ---- Commands that need the canvas ----

    /// Opens the inline name field on node @p id (F2). Enter commits, Esc cancels.
    void startRename(quint64 id);
    /// The inline name field while renaming, else nullptr (tests).
    [[nodiscard]] QLineEdit* renameField() const { return m_rename; }
    /// Opens the inline text field of note @p id.
    void startNoteEdit(quint64 id);
    /// Context menu for a click at @p pos (built from the registry's comp.* commands).
    [[nodiscard]] QMenu* buildContextMenu(QPointF pos, QWidget* parent);

    /// Text of the hover tooltip of the tile at @p pos: name, type and the key settings (empty
    /// over anything else).
    [[nodiscard]] QString tooltipAt(QPointF pos) const;
    /// Opens the Find field (Ctrl+F while the editor has focus): typing selects the nodes whose
    /// name or type contains the text, Enter frames them, Esc closes it.
    void startFind();
    /// The Find field while open, else nullptr (tests).
    [[nodiscard]] QLineEdit* findField() const { return m_find; }
    /// Selects the nodes whose name or type contains @p text (case-insensitive) and frames
    /// them in the view. Returns how many matched.
    int findNodes(const QString& text);

    /// Renders the picture of node @p nodeId at @p pixels for the tile pictures (comp frame of
    /// the page); an invalid future = not available. Set by the page.
    using ThumbnailSource = std::function<std::future<RenderedFramePtr>(quint64, QSize)>;
    void setThumbnailSource(ThumbnailSource source);
    /// Tile picture of @p nodeId, or a null image (not rendered yet; tests).
    [[nodiscard]] QImage tilePicture(quint64 nodeId) const { return m_thumbs.value(nodeId); }
    /// Renders the pictures of the visible nodes that show one now (normally the throttle
    /// timer does this; tests call it directly). Returns the number requested.
    int refreshTilePictures();

    /// Duration of the last paintEvent() in milliseconds (performance test).
    [[nodiscard]] double lastPaintMs() const { return m_lastPaintMs; }

signals:
    /// The user changed the view (pan / zoom); the page stores it in the session.
    void viewChanged(QPointF centre, double zoom);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    enum class Drag {
        None,
        Pending, ///< pressed on a tile / note, not moved far enough yet
        Move,
        Box,
        Pan,
        Connect, ///< from an output to an input
        ConnectReverse, ///< from an (unconnected) input to an output
        Reconnect, ///< a connected input's end picked up
        CutLink ///< Shift pressed on a link: dragging away disconnects it
    };

    /// The comp.* action bound to the key of @p event, or empty.
    [[nodiscard]] QString actionForKey(const QKeyEvent* event) const;
    /// Node of @p id with the live move offset applied (painting during a move).
    [[nodiscard]] QPointF livePosition(const CompNode& node) const;
    [[nodiscard]] bool isMoving(quint64 nodeId) const;
    /// The link (node + port it ends in) near @p pos, ignoring links touching @p exclude.
    [[nodiscard]] Hit linkAt(QPointF pos, quint64 exclude = 0) const;
    /// The link the moving node would be inserted into (Move drag of one unconnected node).
    [[nodiscard]] Hit insertTarget() const;
    /// Points of the link curve from @p from (an output) to @p to (an input on edge @p side of
    /// its tile: 0 left, -1 top, 1 bottom); the curve leaves the output to the right and enters
    /// the input across its edge.
    static void linkCurve(QPointF from, QPointF to, int side, QPointF& c1, QPointF& c2);
    /// True if input @p port of a node of type @p info is a side input (not the main input);
    /// see inputSide() for the edge it is drawn on.
    [[nodiscard]] static bool isTopPort(const compadapter::NodeTypeInfo& info, const QString& port);
    /// Ends a connect gesture at @p pos.
    void finishConnect(QPointF pos);
    void finishBox(Qt::KeyboardModifiers mods);
    void startNodeDrag(quint64 nodeId);
    void setDrag(Drag drag);
    void commitRename(bool accept);
    void commitNoteEdit(bool accept);
    void closeFind();
    /// Replace With / Set Color / Tile Picture entries of a node's context menu.
    void addNodeMenuItems(QMenu* menu, const CompNode& node);
    void scheduleTilePictures();
    void collectTilePictures();

    // ---- Painting (NodeEditorPaint.cpp) ----
    void paintGrid(QPainter& p, const QRectF& gridRect) const;
    void paintLinks(QPainter& p, const QRectF& visible) const;
    void paintTile(QPainter& p, const CompNode& node, const compadapter::NodeTypeInfo* info) const;
    void paintNote(QPainter& p, const CompNote& note) const;
    void paintDragOverlay(QPainter& p) const;

    QPointer<CompSession> m_session;
    ActionRegistry* m_actions = nullptr;
    QPointF m_centre;
    double m_zoom = 1.0;

    Drag m_drag = Drag::None;
    QPointF m_pressPos; ///< widget pixels
    QPointF m_mousePos;
    QPointF m_moveDelta; ///< grid units, live during Move
    quint64 m_pressNode = 0;
    quint64 m_pressNote = 0;
    QString m_pressPort;
    quint64 m_linkSource = 0; ///< Reconnect: node feeding the picked-up link
    Qt::KeyboardModifiers m_pressMods;
    bool m_spaceHeld = false;
    bool m_spacePanned = false;
    Hit m_dropLink; ///< library drag: link under the cursor

    bool m_moveShift = false; ///< Shift held during the Move drag (insert a connected node)
    bool m_pendingToggle = false; ///< Shift pressed on a selected tile: a click deselects it
    QLineEdit* m_find = nullptr;
    ThumbnailSource m_thumbSource;
    QHash<quint64, QImage> m_thumbs;
    struct PendingPicture
    {
        quint64 node = 0;
        std::future<RenderedFramePtr> frame;
    };
    std::vector<PendingPicture> m_pendingPictures;
    QTimer m_thumbTimer; ///< throttle: at most one round of renders per interval
    QTimer m_thumbPoll;  ///< collects finished renders
    QLineEdit* m_rename = nullptr;
    quint64 m_renameNode = 0;
    QPlainTextEdit* m_noteEdit = nullptr;
    quint64 m_editNote = 0;
    double m_lastPaintMs = 0.0;
    // Paint scratch (reused between paints: no allocation in steady state).
    mutable QVector<QLine> m_gridLines;
    mutable QFont m_tileFont;
    mutable const QFontMetricsF* m_tileMetrics = nullptr; ///< valid during paintEvent() only
};

} // namespace kk
