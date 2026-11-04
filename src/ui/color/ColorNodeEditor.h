// SPDX-License-Identifier: GPL-3.0-or-later
// ColorNodeEditor — the Color page's node graph: corrector tiles with a number and a thumbnail,
// Parallel / Layer mixers, the source and output ends, links dragged between ports.
#pragma once

#include "core/Grade.h"

#include <QHash>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QWidget>

class QMenu;

namespace kk {

class ActionRegistry;
class ColorSession;

/// Custom-painted node graph of the open grade (ColorSession::grade()).
///
/// Painted straight from the grade value like the Compositing page's NodeEditor (no scene to
/// keep in sync; the reasons are in ui/comp/NodeEditor.h). It is a separate widget because a
/// grade is a different graph (numbered correctors with one input, mixers with a list of
/// inputs, no named ports or notes) and the Compositing page's editor is built around
/// CompSession's model; the conventions are shared: pan with the middle button, zoom with the
/// wheel around the cursor, links as horizontal cubic curves.
///
/// **Geometry.** GradeNode::pos is the tile's top-left corner in editor units (pixels at zoom
/// 1; gradeops places nodes 140 x 90 apart). A corrector tile is kTileSize: the node number
/// (and label) on top, a thumbnail of the node's output below; its input is on the middle of
/// the left edge, its output on the right. A mixer is a small square with one input port per
/// input plus a free one (drop a link there to add an input). The source picture enters at
/// a port left of every node, the grade output leaves at a port right of every node.
///
/// **Gestures** (each one undo step through ColorSession): click selects; drag a tile moves
/// it (GradeNode::pos); drag from an output (or the source) to an input, a tile or the output
/// end connects; drag a connected input's end to another input reconnects, into empty space
/// disconnects; right-click opens the node menu (add serial / parallel / layer, enable,
/// reset, delete). The keys (Alt+S, Ctrl+D, Delete, ...) are the page's color.* commands.
///
/// Thread-safety: GUI thread only.
class ColorNodeEditor : public QWidget
{
    Q_OBJECT
public:
    static constexpr QSizeF kTileSize {100.0, 70.0};
    static constexpr double kMixerSize = 40.0;
    static constexpr double kPortRadius = 5.0;
    static constexpr double kMinZoom = 0.25;
    static constexpr double kMaxZoom = 3.0;

    struct Hit
    {
        enum class Kind {
            None,
            Tile,
            Input, ///< input port @p index of @p node (index == input count: the free port)
            Output, ///< output port of @p node
            Source, ///< the source picture's port
            GradeOutput ///< the grade output's port
        };
        Kind kind = Kind::None;
        quint64 node = 0;
        int index = 0;
    };

    ColorNodeEditor(ColorSession* session, ActionRegistry* actions, QWidget* parent = nullptr);

    /// Thumbnails per node id (the page renders them); missing ones are drawn as a dark box.
    void setThumbnails(const QHash<quint64, QImage>& thumbnails);
    [[nodiscard]] const QHash<quint64, QImage>& thumbnails() const { return m_thumbs; }

    // ---- View ----
    [[nodiscard]] double zoom() const { return m_zoom; }
    /// Editor point at the widget's top-left.
    [[nodiscard]] QPointF origin() const { return m_origin; }
    void setView(QPointF origin, double zoom);
    /// Shows every node (zoom at most 1).
    void fitToView();
    [[nodiscard]] QPointF toWidget(QPointF editor) const;
    [[nodiscard]] QPointF toEditor(QPointF widget) const;

    // ---- Geometry (widget pixels; public for tests) ----
    [[nodiscard]] QRectF tileRect(const GradeNode& node) const;
    [[nodiscard]] QPointF inputPos(const GradeNode& node, int index) const;
    [[nodiscard]] QPointF outputPos(const GradeNode& node) const;
    [[nodiscard]] QPointF sourcePos() const;
    [[nodiscard]] QPointF gradeOutputPos() const;
    [[nodiscard]] Hit hitTest(QPointF pos) const;

    /// The right-click menu for a click at @p pos (tests call it directly).
    [[nodiscard]] QMenu* buildContextMenu(QPointF pos, QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Drag {
        None,
        Pending,
        Move,
        Pan,
        Connect, ///< from an output (m_fromNode, 0 = source) to an input
        Reconnect ///< a connected input's end picked up (m_pickNode / m_pickIndex)
    };

    /// Editor-space bounds of every tile (empty grade: a default box).
    [[nodiscard]] QRectF nodesBounds() const;
    /// Tile size of @p node in editor units.
    [[nodiscard]] static QSizeF tileSize(const GradeNode& node);
    /// Position of @p node including the live move offset.
    [[nodiscard]] QPointF livePos(const GradeNode& node) const;
    void finishConnect(QPointF pos);
    void paintTile(QPainter& p, const ClipGrade& g, int index) const;
    void paintLink(QPainter& p, QPointF from, QPointF to, const QColor& colour) const;
    void onGradeChanged();

    QPointer<ColorSession> m_session;
    ActionRegistry* m_actions = nullptr;
    QHash<quint64, QImage> m_thumbs;
    QPointF m_origin {-40, -60};
    double m_zoom = 1.0;
    bool m_viewSet = false; ///< the user panned / zoomed: no automatic fit
    quint64 m_lastClip = ~quint64(0);
    QVector<quint64> m_lastIds; ///< node ids at the last fit

    Drag m_drag = Drag::None;
    QPointF m_pressPos;
    QPointF m_mousePos;
    QPointF m_moveDelta; ///< editor units
    quint64 m_pressNode = 0;
    quint64 m_fromNode = 0;
    quint64 m_pickNode = 0;
    int m_pickIndex = 0;
};

} // namespace kk
