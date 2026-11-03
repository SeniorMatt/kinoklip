// SPDX-License-Identifier: GPL-3.0-or-later
// NodeEditor painting: background grid, links, tiles, notes and the gesture overlays.
// One pass straight from the Composition value; tiles and links outside the view are culled.
#include "ui/common/SchemeColors.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"

#include <QElapsedTimer>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Below this many pixels per grid unit the grid lines are drawn every kCoarseGrid units.
constexpr double kFineGridMinPx = 14.0;
constexpr int kCoarseGrid = 5;
/// Below this zoom tile names are not drawn (unreadable; saves text layout time).
constexpr double kMinTextZoom = 0.3;
/// Tile corner radius as a fraction of the tile height.
constexpr double kCornerFraction = 0.18;
/// Port radius as a fraction of the tile height (at least kMinPortPx).
constexpr double kPortFraction = 0.15;
constexpr double kMinPortPx = 3.0;

/// Link colour of an input role: background a warm yellow, foreground green, masks blue
/// (the colours a node compositor user expects, taken from the scheme's roles).
QColor roleColor(compadapter::LinkRole role)
{
    const SchemeColors& sc = schemeColors();
    switch (role) {
    case compadapter::LinkRole::Background:
        return sc.snap;
    case compadapter::LinkRole::Foreground:
        return sc.positive;
    case compadapter::LinkRole::Mask:
        return sc.link;
    }
    return sc.text;
}

compadapter::LinkRole roleOf(const compadapter::NodeTypeInfo* info, const QString& port)
{
    if (info) {
        const int i = info->inputIndex(port);
        if (i >= 0)
            return info->inputs[i].role;
    }
    return compadapter::LinkRole::Background;
}

/// A small padlock (locked nodes), drawn in @p r.
void paintLock(QPainter& p, const QRectF& r, const QColor& color)
{
    p.save();
    p.setPen(QPen(color, std::max(1.0, r.width() / 8)));
    p.setBrush(Qt::NoBrush);
    const QRectF body(r.left(), r.top() + r.height() * 0.45, r.width(), r.height() * 0.55);
    const QRectF shackle(r.left() + r.width() * 0.2, r.top(), r.width() * 0.6, r.height() * 0.8);
    p.drawArc(shackle, 0, 180 * 16);
    p.setBrush(color);
    p.drawRoundedRect(body, 1, 1);
    p.restore();
}

} // namespace

void NodeEditor::paintEvent(QPaintEvent* event)
{
    QElapsedTimer timer;
    timer.start();
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.fillRect(event->rect(), sc.view);
    if (!m_session || !m_session->clipId()) {
        p.setPen(sc.textDim);
        p.drawText(rect(), Qt::AlignCenter, tr("Select a clip on the Edit page"));
        m_lastPaintMs = timer.nsecsElapsed() / 1e6;
        return;
    }
    const QRectF visible = QRectF(event->rect());
    paintGrid(p, QRectF(toGrid(visible.topLeft()), toGrid(visible.bottomRight())));
    p.setRenderHint(QPainter::Antialiasing, true);

    const Composition& c = m_session->composition();
    for (const CompNote& note : c.notes)
        paintNote(p, note);
    paintLinks(p, visible);
    // One font for every tile name (the size follows the zoom), set once per paint.
    m_tileFont = font();
    m_tileFont.setPointSizeF(std::max(4.0, m_tileFont.pointSizeF() * std::clamp(m_zoom, 0.6, 1.4)));
    p.setFont(m_tileFont);
    const QFontMetricsF metrics(m_tileFont);
    m_tileMetrics = &metrics;
    const QRectF cull = visible.adjusted(-kUnitPx, -kUnitPx, kUnitPx, kUnitPx);
    for (const CompNode& n : c.nodes) {
        if (tileRect(n).intersects(cull))
            paintTile(p, n, compadapter::nodeType(n.type));
    }
    m_tileMetrics = nullptr;
    paintDragOverlay(p);
    m_lastPaintMs = timer.nsecsElapsed() / 1e6;
}

void NodeEditor::paintGrid(QPainter& p, const QRectF& gridRect) const
{
    const SchemeColors& sc = schemeColors();
    const double px = kUnitPx * m_zoom;
    const int step = px >= kFineGridMinPx ? 1 : kCoarseGrid;
    // Opaque, pre-mixed colour and whole-pixel lines: the raster engine fills them as spans
    // without blending (an alpha pen made the grid the most expensive part of a paint).
    p.setPen(QPen(mixColors(sc.view, sc.grid, 0.35), 1));
    const int x0 = int(std::floor(gridRect.left() / step)) * step;
    const int y0 = int(std::floor(gridRect.top() / step)) * step;
    m_gridLines.clear();
    for (int x = x0; x <= gridRect.right(); x += step) {
        const int wx = int(std::lround(fromGrid(QPointF(x, 0)).x()));
        m_gridLines.append(QLine(wx, 0, wx, height()));
    }
    for (int y = y0; y <= gridRect.bottom(); y += step) {
        const int wy = int(std::lround(fromGrid(QPointF(0, y)).y()));
        m_gridLines.append(QLine(0, wy, width(), wy));
    }
    p.drawLines(m_gridLines);
}

void NodeEditor::paintLinks(QPainter& p, const QRectF& visible) const
{
    const Composition& c = m_session->composition();
    const Hit insert = insertTarget();
    const double width = std::clamp(2.0 * m_zoom, 1.0, 3.0);
    for (const CompNode& to : c.nodes) {
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(to.type);
        for (auto it = to.inputs.cbegin(); it != to.inputs.cend(); ++it) {
            if (m_drag == Drag::Reconnect && to.id == m_pressNode && it.key() == m_pressPort)
                continue; // picked up: drawn by the overlay
            const CompNode* from = c.node(it->nodeId);
            if (!from)
                continue;
            const QPointF a = outputPos(*from), d = inputPos(to, it.key());
            if (!QRectF(a, d).normalized().adjusted(-60, -60, 60, 60).intersects(visible))
                continue;
            QPointF b, cc;
            linkCurve(a, d, info && isTopPort(*info, it.key()), b, cc);
            QPainterPath path(a);
            path.cubicTo(b, cc, d);
            const bool highlighted = (insert.node == to.id && insert.port == it.key())
                || (m_dropLink.node == to.id && m_dropLink.port == it.key());
            QColor color = roleColor(roleOf(info, it.key()));
            if (!from->enabled)
                color = withAlpha(color, 140);
            p.setPen(QPen(highlighted ? schemeColors().highlight : color,
                highlighted ? width * 2 : width, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }
}

void NodeEditor::paintTile(
    QPainter& p, const CompNode& node, const compadapter::NodeTypeInfo* info) const
{
    const SchemeColors& sc = schemeColors();
    const QRectF r = tileRect(node);
    const double h = r.height();
    const bool selected = m_session->selectedNodes().contains(node.id);
    const bool current = m_session->currentNode() == node.id;
    const bool io
        = node.type == QLatin1String("media_in") || node.type == QLatin1String("media_out");

    QColor fill = mixColors(sc.button, sc.window, 0.25);
    if (io)
        fill = mixColors(fill, sc.videoClip, 0.35);
    if (current)
        fill = mixColors(fill, sc.highlight, 0.25);
    if (!node.enabled)
        fill = mixColors(fill, sc.view, 0.55);
    const double radius = h * kCornerFraction;
    if (node.enabled) {
        // Border as a filled outer shape under the fill: two fills are much cheaper than an
        // antialiased stroke (measured: about a third of the tile cost with 200 nodes).
        const double bw = selected ? 2.0 : 1.0;
        p.setPen(Qt::NoPen);
        p.setBrush(selected ? sc.selection : sc.line);
        p.drawRoundedRect(r, radius, radius);
        p.setBrush(fill);
        p.drawRoundedRect(r.adjusted(bw, bw, -bw, -bw), radius - bw / 2, radius - bw / 2);
    } else {
        QPen border(selected ? sc.selection : sc.line, selected ? 2.0 : 1.0, Qt::DashLine);
        p.setPen(border);
        p.setBrush(fill);
        p.drawRoundedRect(r, radius, radius);
    }
    if (!node.enabled) {
        // Pass-through: a stroke across the tile, like a bypassed effect.
        p.setPen(QPen(sc.textDisabled, 1.0));
        p.drawLine(r.bottomLeft() + QPointF(radius, -2), r.topRight() + QPointF(-radius, 2));
    }

    if (m_zoom >= kMinTextZoom && m_tileMetrics) {
        // Point-based drawText (no rectangle layout); elide only names that do not fit.
        p.setPen(node.enabled ? sc.textOn(fill) : sc.textDisabled);
        const double room = r.width() - h * 0.6;
        double w = m_tileMetrics->horizontalAdvance(node.name);
        QString text = node.name;
        if (w > room) {
            text = m_tileMetrics->elidedText(node.name, Qt::ElideRight, room);
            w = m_tileMetrics->horizontalAdvance(text);
        }
        p.drawText(QPointF(r.center().x() - w / 2,
                       r.center().y() + (m_tileMetrics->ascent() - m_tileMetrics->descent()) / 2),
            text);
    }
    if (node.locked) {
        const double s = h * 0.32;
        paintLock(p, QRectF(r.right() - s - h * 0.15, r.top() + h * 0.12, s * 0.8, s), sc.textDim);
    }

    // Viewer markers under the tile: left dot = left viewer, right dot = right viewer (only
    // for the nodes shown, so a large graph does not pay for 2 x n empty markers).
    const double dot = std::max(2.0, h * 0.1);
    for (int v = 0; v < 2; ++v) {
        if (m_session->viewerNode(v) != node.id)
            continue;
        const QPointF centre(r.left() + h * 0.3 + v * dot * 3, r.bottom() + dot * 1.8);
        p.setPen(Qt::NoPen);
        p.setBrush(sc.highlight);
        p.drawEllipse(centre, dot, dot);
    }

    if (!info)
        return;
    const double portR = std::max(kMinPortPx, h * kPortFraction);
    const double ring = std::min(1.5, portR / 2);
    p.setPen(Qt::NoPen);
    for (const compadapter::PortInfo& port : info->inputs) {
        const QPointF c = inputPos(node, port.id);
        const bool connected = node.inputs.value(port.id).isConnected();
        // Filled shapes only (outer in the port colour, inner hole for a free port).
        p.setBrush(roleColor(port.role));
        if (port.kind == PortKind::Mask) {
            // Masks: a diamond, so the port kind reads without colour too.
            auto diamond = [&c](double s) {
                return QPolygonF(
                    {c + QPointF(0, -s), c + QPointF(s, 0), c + QPointF(0, s), c + QPointF(-s, 0)});
            };
            p.drawPolygon(diamond(portR));
            if (!connected) {
                p.setBrush(sc.view);
                p.drawPolygon(diamond(portR - ring * 1.4));
            }
        } else {
            p.drawEllipse(c, portR, portR);
            if (!connected) {
                p.setBrush(sc.view);
                p.drawEllipse(c, portR - ring, portR - ring);
            }
        }
    }
    if (info->outputCount > 0) {
        p.setBrush(sc.text);
        p.drawEllipse(outputPos(node), portR, portR);
    }
}

void NodeEditor::paintNote(QPainter& p, const CompNote& note) const
{
    const SchemeColors& sc = schemeColors();
    const double s = kUnitPx * m_zoom;
    const QPointF pos = m_drag == Drag::Move && m_session->selectedNotes().contains(note.id)
        ? note.position + m_moveDelta
        : note.position;
    const QRectF r(fromGrid(pos), note.size * s);
    const bool selected = m_session->selectedNotes().contains(note.id);
    const QColor base = note.color.isValid() ? note.color : sc.neutral;
    p.setPen(QPen(selected ? sc.selection : withAlpha(base, 180), selected ? 2.0 : 1.0));
    p.setBrush(withAlpha(mixColors(sc.view, base, 0.25), 230));
    p.drawRoundedRect(r, 3, 3);
    if (m_zoom >= kMinTextZoom) {
        p.setPen(sc.text);
        p.drawText(r.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            note.text.isEmpty() ? tr("Note") : note.text);
    }
}

void NodeEditor::paintDragOverlay(QPainter& p) const
{
    const SchemeColors& sc = schemeColors();
    const Composition& c = m_session->composition();
    switch (m_drag) {
    case Drag::Box: {
        const QRectF box = QRectF(m_pressPos, m_mousePos).normalized();
        p.setPen(QPen(sc.highlight, 1.0));
        p.setBrush(withAlpha(sc.highlight, 40));
        p.drawRect(box);
        return;
    }
    case Drag::Connect:
    case Drag::ConnectReverse:
    case Drag::Reconnect: {
        QPointF from, to;
        bool top = false;
        QColor color = sc.text;
        if (m_drag == Drag::Connect) {
            const CompNode* n = c.node(m_pressNode);
            if (!n)
                return;
            from = outputPos(*n);
            to = m_mousePos;
        } else {
            const CompNode* target = c.node(m_pressNode);
            const compadapter::NodeTypeInfo* info
                = target ? compadapter::nodeType(target->type) : nullptr;
            color = roleColor(roleOf(info, m_pressPort));
            if (m_drag == Drag::ConnectReverse) {
                if (!target)
                    return;
                from = m_mousePos;
                to = inputPos(*target, m_pressPort);
                top = info && isTopPort(*info, m_pressPort);
            } else {
                const CompNode* source = c.node(m_linkSource);
                if (!source)
                    return;
                from = outputPos(*source);
                to = m_mousePos;
            }
        }
        QPointF b, cc;
        linkCurve(from, to, top, b, cc);
        QPainterPath path(from);
        path.cubicTo(b, cc, to);
        p.setPen(QPen(color, 2.0, Qt::DashLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        return;
    }
    default:
        return;
    }
}

} // namespace kk
