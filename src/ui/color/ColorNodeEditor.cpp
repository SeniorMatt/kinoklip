// SPDX-License-Identifier: GPL-3.0-or-later
// ColorNodeEditor — painting, hit tests and gestures of the Color page node graph.
#include "ui/color/ColorNodeEditor.h"

#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr double kDragStartPx = 4.0;
constexpr double kEndGap = 70.0; ///< editor units between the nodes and the source / output ends
constexpr double kLabelHeight = 16.0;

double distance(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QAction* registryAction(ActionRegistry* actions, const char* id)
{
    const QString key = QLatin1String(id);
    return actions && actions->contains(key) ? actions->action(key) : nullptr;
}

} // namespace

ColorNodeEditor::ColorNodeEditor(ColorSession* session, ActionRegistry* actions, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("ColorNodeEditor"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(160, 120);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorNodeEditor::onGradeChanged);
        connect(m_session, &ColorSession::clipChanged, this, &ColorNodeEditor::onGradeChanged);
        connect(m_session, &ColorSession::selectedNodeChanged, this, qOverload<>(&QWidget::update));
    }
}

void ColorNodeEditor::onGradeChanged()
{
    // Another clip (or target): show its whole graph again unless the user moved the view.
    // Nodes added or removed: fit again (unless the user set the view); a moved node keeps it.
    const quint64 clip = m_session ? m_session->clipId() : 0;
    QVector<quint64> ids;
    if (m_session) {
        for (const GradeNode& n : m_session->grade().nodes)
            ids.append(n.id);
    }
    if (clip != m_lastClip) {
        m_lastClip = clip;
        m_viewSet = false;
    }
    if (ids != m_lastIds) {
        m_lastIds = ids;
        if (!m_viewSet)
            fitToView();
    }
    update();
}

void ColorNodeEditor::setThumbnails(const QHash<quint64, QImage>& thumbnails)
{
    m_thumbs = thumbnails;
    update();
}

void ColorNodeEditor::setView(QPointF origin, double zoom)
{
    m_origin = origin;
    m_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    update();
}

QSizeF ColorNodeEditor::tileSize(const GradeNode& node)
{
    return node.isMixer() ? QSizeF(kMixerSize, kMixerSize) : kTileSize;
}

QRectF ColorNodeEditor::nodesBounds() const
{
    const ClipGrade* g = m_session ? &m_session->grade() : nullptr;
    if (!g || g->nodes.isEmpty())
        return QRectF(QPointF(0, 0), kTileSize);
    QRectF r;
    for (const GradeNode& n : g->nodes) {
        const QRectF t(n.pos, tileSize(n));
        r = r.isNull() ? t : r.united(t);
    }
    return r;
}

void ColorNodeEditor::fitToView()
{
    const QRectF b = nodesBounds().adjusted(-kEndGap - 44, -30, kEndGap + 44, 24);
    if (width() <= 0 || height() <= 0)
        return;
    m_zoom = std::clamp(
        std::min({width() / b.width(), height() / b.height(), 1.0}), kMinZoom, kMaxZoom);
    m_origin = b.center() - QPointF(width(), height()) / (2 * m_zoom);
    update();
}

void ColorNodeEditor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_viewSet)
        fitToView();
}

QPointF ColorNodeEditor::toWidget(QPointF e) const
{
    return (e - m_origin) * m_zoom;
}

QPointF ColorNodeEditor::toEditor(QPointF w) const
{
    return w / m_zoom + m_origin;
}

QPointF ColorNodeEditor::livePos(const GradeNode& node) const
{
    if (m_drag == Drag::Move && node.id == m_pressNode)
        return node.pos + m_moveDelta;
    return node.pos;
}

QRectF ColorNodeEditor::tileRect(const GradeNode& node) const
{
    const QSizeF s = tileSize(node);
    return QRectF(toWidget(livePos(node)), s * m_zoom);
}

QPointF ColorNodeEditor::inputPos(const GradeNode& node, int index) const
{
    const QRectF r = tileRect(node);
    if (!node.isMixer())
        return QPointF(r.left(), r.center().y());
    const int portCount = int(node.inputs.size()) + 1; // + the free port
    return QPointF(r.left(), r.top() + r.height() * (index + 1) / (portCount + 1));
}

QPointF ColorNodeEditor::outputPos(const GradeNode& node) const
{
    const QRectF r = tileRect(node);
    return QPointF(r.right(), r.center().y());
}

QPointF ColorNodeEditor::sourcePos() const
{
    const QRectF b = nodesBounds();
    return toWidget(QPointF(b.left() - kEndGap, b.center().y()));
}

QPointF ColorNodeEditor::gradeOutputPos() const
{
    const QRectF b = nodesBounds();
    return toWidget(QPointF(b.right() + kEndGap, b.center().y()));
}

ColorNodeEditor::Hit ColorNodeEditor::hitTest(QPointF pos) const
{
    Hit hit;
    if (!m_session)
        return hit;
    const double portPick = std::max(kPortRadius * m_zoom, 4.0) + 3.0;
    const ClipGrade& g = m_session->grade();
    // Ports first (they sit on tile edges), topmost (last painted) node first.
    for (int i = int(g.nodes.size()) - 1; i >= 0; --i) {
        const GradeNode& n = g.nodes[i];
        if (distance(pos, outputPos(n)) <= portPick)
            return {Hit::Kind::Output, n.id, 0};
        const int ports = n.isMixer() ? int(n.inputs.size()) + 1 : 1;
        for (int k = 0; k < ports; ++k) {
            if (distance(pos, inputPos(n, k)) <= portPick)
                return {Hit::Kind::Input, n.id, k};
        }
    }
    for (int i = int(g.nodes.size()) - 1; i >= 0; --i) {
        if (tileRect(g.nodes[i]).contains(pos))
            return {Hit::Kind::Tile, g.nodes[i].id, 0};
    }
    if (distance(pos, sourcePos()) <= portPick + 4)
        return {Hit::Kind::Source, 0, 0};
    if (distance(pos, gradeOutputPos()) <= portPick + 4)
        return {Hit::Kind::GradeOutput, 0, 0};
    return hit;
}

// ---- Painting ----

void ColorNodeEditor::paintLink(QPainter& p, QPointF a, QPointF b, const QColor& colour) const
{
    const double dx = std::max(30.0 * m_zoom, std::abs(b.x() - a.x()) * 0.5);
    QPainterPath path(a);
    path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
    p.setPen(QPen(colour, std::max(1.2, 1.6 * m_zoom)));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void ColorNodeEditor::paintTile(QPainter& p, const ClipGrade& g, int index) const
{
    const GradeNode& n = g.nodes[index];
    const QRectF r = tileRect(n);
    const QPalette& pal = palette();
    const bool selected = m_session && n.id == m_session->selectedNode();
    const bool reached = [&] {
        for (const int i : gradeops::evaluationOrder(g)) {
            if (i == index)
                return true;
        }
        return false;
    }();
    QColor body = pal.color(QPalette::Button);
    QColor text = pal.color(QPalette::ButtonText);
    if (!n.enabled || !reached) {
        body = body.darker(130);
        text.setAlphaF(0.5f);
    }
    const double radius = 4.0 * m_zoom;
    p.setPen(QPen(selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Dark),
        selected ? 2.0 : 1.0));
    p.setBrush(body);
    p.drawRoundedRect(r, radius, radius);

    QFont f = font();
    f.setPointSizeF(std::max(5.0, f.pointSizeF() * std::min(1.0, m_zoom) * 0.9));
    p.setFont(f);
    if (n.isMixer()) {
        // A mixer: its kind as a letter (P / L) in the middle.
        p.setPen(text);
        QFont big = f;
        big.setBold(true);
        p.setFont(big);
        p.drawText(r, Qt::AlignCenter,
            n.type == GradeNodeType::Parallel ? tr("P", "Parallel mixer") : tr("L", "Layer mixer"));
        p.setFont(f);
    } else {
        const QRectF label(
            r.left() + 4 * m_zoom, r.top(), r.width() - 8 * m_zoom, kLabelHeight * m_zoom);
        p.setPen(text);
        QString name = gradeNodeName(g, index);
        if (!n.label.isEmpty() && name != n.label)
            name = n.label;
        else if (n.label.isEmpty())
            name = QString::number(index + 1).rightJustified(2, QLatin1Char('0'));
        p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, name);
        // Markers of what the node uses: qualifier, windows, LUT.
        QStringList marks;
        if (n.qualifier.enabled)
            marks << tr("Q", "qualifier marker");
        if (!n.windows.isEmpty())
            marks << tr("W", "window marker");
        if (!n.lutPath.isEmpty())
            marks << tr("LUT");
        if (!marks.isEmpty())
            p.drawText(label, Qt::AlignRight | Qt::AlignVCenter, marks.join(QLatin1Char(' ')));
        const QRectF thumb = QRectF(r.left() + 6 * m_zoom, r.top() + kLabelHeight * m_zoom,
            r.width() - 12 * m_zoom, r.height() - (kLabelHeight + 6) * m_zoom);
        const QImage img = m_thumbs.value(n.id);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(16, 16, 18));
        p.drawRect(thumb);
        if (!img.isNull()) {
            QSizeF s = QSizeF(img.size()).scaled(thumb.size(), Qt::KeepAspectRatio);
            const QRectF t(thumb.center() - QPointF(s.width(), s.height()) / 2, s);
            p.drawImage(t, img);
        }
        if (!n.enabled) {
            // Bypassed: a diagonal over the thumbnail.
            p.setPen(QPen(QColor(230, 90, 70), 1.5));
            p.drawLine(thumb.topRight(), thumb.bottomLeft());
        }
    }
    // Ports.
    const double pr = std::max(2.5, kPortRadius * m_zoom);
    p.setPen(QPen(pal.color(QPalette::Dark), 1));
    const int ports = n.isMixer() ? int(n.inputs.size()) + 1 : 1;
    for (int k = 0; k < ports; ++k) {
        const bool spare = n.isMixer() && k == int(n.inputs.size());
        p.setBrush(spare ? pal.color(QPalette::Mid) : QColor(80, 190, 110));
        p.drawEllipse(inputPos(n, k), pr, pr);
    }
    p.setBrush(QColor(80, 190, 110));
    p.drawEllipse(outputPos(n), pr, pr);
}

void ColorNodeEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base).darker(115));
    // Dot grid every 20 editor units.
    {
        const double step = 20.0 * m_zoom;
        if (step >= 6.0) {
            QColor dot = pal.color(QPalette::Text);
            dot.setAlphaF(0.12f);
            p.setPen(QPen(dot, 1.0));
            const QPointF o = toWidget(QPointF(0, 0));
            const double x0 = std::fmod(o.x(), step);
            const double y0 = std::fmod(o.y(), step);
            for (double x = x0; x < width(); x += step) {
                for (double y = y0; y < height(); y += step)
                    p.drawPoint(QPointF(x, y));
            }
        }
    }
    if (!m_session || !m_session->hasGrade()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("No clip to grade"));
        return;
    }
    const ClipGrade& g = m_session->grade();
    const QColor linkColour(120, 200, 140);
    QColor dimLink = pal.color(QPalette::Text);
    dimLink.setAlphaF(0.35f);
    // Links.
    for (const GradeNode& n : g.nodes) {
        for (int k = 0; k < n.inputs.size(); ++k) {
            if (m_drag == Drag::Reconnect && n.id == m_pickNode && k == m_pickIndex)
                continue;
            const quint64 src = n.inputs[k];
            const GradeNode* from = src ? g.node(src) : nullptr;
            const QPointF a = from ? outputPos(*from) : sourcePos();
            paintLink(p, a, inputPos(n, k), linkColour);
        }
    }
    if (g.output && !(m_drag == Drag::Reconnect && m_pickNode == 0)) {
        if (const GradeNode* out = g.node(g.output))
            paintLink(p, outputPos(*out), gradeOutputPos(), linkColour);
    } else if (g.isEmpty()) {
        paintLink(p, sourcePos(), gradeOutputPos(), dimLink);
    }
    // Source and output ends.
    const double pr = std::max(3.0, kPortRadius * m_zoom * 1.2);
    p.setPen(QPen(pal.color(QPalette::Dark), 1));
    p.setBrush(QColor(80, 190, 110));
    p.drawEllipse(sourcePos(), pr, pr);
    p.drawEllipse(gradeOutputPos(), pr, pr);
    p.setPen(pal.color(QPalette::Text));
    QFont f = font();
    f.setPointSizeF(std::max(5.0, f.pointSizeF() * 0.85));
    p.setFont(f);
    p.drawText(
        QRectF(sourcePos() - QPointF(40, 26), QSizeF(80, 16)), Qt::AlignCenter, tr("Source"));
    p.drawText(
        QRectF(gradeOutputPos() - QPointF(40, 26), QSizeF(80, 16)), Qt::AlignCenter, tr("Output"));
    // Tiles.
    for (int i = 0; i < g.nodes.size(); ++i)
        paintTile(p, g, i);
    // Live link of a connect gesture.
    if (m_drag == Drag::Connect) {
        const GradeNode* from = m_fromNode ? g.node(m_fromNode) : nullptr;
        paintLink(
            p, from ? outputPos(*from) : sourcePos(), m_mousePos, pal.color(QPalette::Highlight));
    } else if (m_drag == Drag::Reconnect) {
        quint64 src = 0;
        if (m_pickNode == 0)
            src = g.output;
        else if (const GradeNode* n = g.node(m_pickNode); n && m_pickIndex < n->inputs.size())
            src = n->inputs[m_pickIndex];
        const GradeNode* from = src ? g.node(src) : nullptr;
        paintLink(
            p, from ? outputPos(*from) : sourcePos(), m_mousePos, pal.color(QPalette::Highlight));
    }
    if (g.isEmpty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect().adjusted(0, 0, 0, -10), Qt::AlignHCenter | Qt::AlignBottom,
            tr("Ungraded: Alt+S adds a node"));
    }
}

// ---- Gestures ----

void ColorNodeEditor::mousePressEvent(QMouseEvent* event)
{
    m_pressPos = m_mousePos = event->position();
    if (event->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        return;
    }
    if (event->button() != Qt::LeftButton || !m_session)
        return;
    const Hit hit = hitTest(m_pressPos);
    const ClipGrade& g = m_session->grade();
    switch (hit.kind) {
    case Hit::Kind::Tile:
        m_session->selectNode(hit.node);
        m_pressNode = hit.node;
        m_moveDelta = {};
        m_drag = Drag::Pending;
        break;
    case Hit::Kind::Output:
        m_fromNode = hit.node;
        m_drag = Drag::Connect;
        break;
    case Hit::Kind::Source:
        m_fromNode = 0;
        m_drag = Drag::Connect;
        break;
    case Hit::Kind::Input: {
        const GradeNode* n = g.node(hit.node);
        if (n && hit.index < n->inputs.size()) {
            m_pickNode = hit.node;
            m_pickIndex = hit.index;
            m_drag = Drag::Reconnect;
        }
        break;
    }
    case Hit::Kind::GradeOutput:
        if (g.output) {
            m_pickNode = 0;
            m_pickIndex = 0;
            m_drag = Drag::Reconnect;
        }
        break;
    case Hit::Kind::None:
        break;
    }
    update();
}

void ColorNodeEditor::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const QPointF last = m_mousePos;
    m_mousePos = pos;
    switch (m_drag) {
    case Drag::Pan:
        m_origin -= (pos - last) / m_zoom;
        m_viewSet = true;
        update();
        break;
    case Drag::Pending:
        if (distance(pos, m_pressPos) >= kDragStartPx)
            m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move:
        if (m_drag == Drag::Move) {
            m_moveDelta = (pos - m_pressPos) / m_zoom;
            update();
        }
        break;
    case Drag::Connect:
    case Drag::Reconnect:
        update();
        break;
    case Drag::None:
        break;
    }
}

void ColorNodeEditor::finishConnect(QPointF pos)
{
    if (!m_session)
        return;
    const Hit hit = hitTest(pos);
    ClipGrade g = m_session->grade();
    bool changed = false;
    // Where the link should now start (the picked-up link keeps its source).
    quint64 from = m_fromNode;
    if (m_drag == Drag::Reconnect) {
        if (m_pickNode == 0)
            from = g.output;
        else if (const GradeNode* n = g.node(m_pickNode); n && m_pickIndex < n->inputs.size())
            from = n->inputs[m_pickIndex];
    }
    auto connectTo = [&](quint64 to, int index) {
        if (gradeops::connect(g, from, to, index))
            changed = true;
    };
    if (m_drag == Drag::Reconnect && m_pickNode == 0) {
        // The output end picked up: drop it on a node (its output) to make that the output.
        if (hit.kind == Hit::Kind::Tile || hit.kind == Hit::Kind::Output) {
            if (gradeops::connect(g, hit.node, 0, 0))
                changed = true;
        } else if (hit.kind != Hit::Kind::GradeOutput) {
            g.output = 0; // dropped into empty space: the grade is disconnected
            changed = true;
        }
    } else {
        switch (hit.kind) {
        case Hit::Kind::Input:
            if (!(m_drag == Drag::Reconnect && hit.node == m_pickNode && hit.index == m_pickIndex))
                connectTo(hit.node, hit.index);
            break;
        case Hit::Kind::Tile: {
            const GradeNode* n = g.node(hit.node);
            if (n && hit.node != from)
                connectTo(hit.node, n->isMixer() ? int(n->inputs.size()) : 0);
            break;
        }
        case Hit::Kind::GradeOutput:
            if (from && m_drag == Drag::Connect)
                connectTo(0, 0);
            break;
        default:
            break;
        }
        if (m_drag == Drag::Reconnect
            && !(hit.kind == Hit::Kind::Input && hit.node == m_pickNode
                && hit.index == m_pickIndex)) {
            // The picked-up end leaves its old input (a corrector's falls back to the source).
            if (const GradeNode* n = g.node(m_pickNode); n && m_pickIndex < n->inputs.size()) {
                if (gradeops::disconnect(g, m_pickNode, m_pickIndex))
                    changed = true;
            }
        }
    }
    if (changed && g != m_session->grade())
        m_session->commit(g, m_drag == Drag::Connect ? tr("Connect Nodes") : tr("Reconnect Nodes"));
}

void ColorNodeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = m_drag;
    if (drag == Drag::Pan && event->button() == Qt::MiddleButton) {
        m_drag = Drag::None;
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (drag == Drag::Move && m_session && !m_moveDelta.isNull()) {
        if (const GradeNode* n = m_session->grade().node(m_pressNode)) {
            const QPointF target = n->pos + m_moveDelta;
            m_drag = Drag::None;
            m_session->moveNodes({{m_pressNode, target}});
        }
    } else if (drag == Drag::Connect || drag == Drag::Reconnect) {
        finishConnect(event->position());
    }
    m_drag = Drag::None;
    m_moveDelta = {};
    m_pressNode = 0;
    update();
}

void ColorNodeEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click on the background: show every node (like Fit).
    if (hitTest(event->position()).kind == Hit::Kind::None) {
        m_viewSet = false;
        fitToView();
    }
}

void ColorNodeEditor::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0)
        return;
    const QPointF anchor = event->position();
    const QPointF before = toEditor(anchor);
    m_zoom = std::clamp(m_zoom * std::pow(1.15, steps), kMinZoom, kMaxZoom);
    m_origin = before - anchor / m_zoom;
    m_viewSet = true;
    update();
    event->accept();
}

QMenu* ColorNodeEditor::buildContextMenu(QPointF pos, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    const Hit hit = hitTest(pos);
    if (m_session
        && (hit.kind == Hit::Kind::Tile || hit.kind == Hit::Kind::Output
            || hit.kind == Hit::Kind::Input))
        m_session->selectNode(hit.node);
    const bool onNode = hit.node != 0;
    auto add = [&](const char* id, const QString& fallback, auto&& fn) {
        QAction* a = nullptr;
        if (QAction* reg = registryAction(m_actions, id)) {
            a = menu->addAction(reg->text());
            a->setShortcut(reg->shortcut());
            connect(a, &QAction::triggered, reg, &QAction::trigger);
        } else {
            a = menu->addAction(fallback);
            connect(a, &QAction::triggered, this, fn);
        }
        a->setObjectName(QLatin1String(id));
        return a;
    };
    QPointer<ColorSession> s = m_session;
    add("color.add_serial", tr("Add Serial Node"), [s] {
        if (s)
            s->addSerialNode();
    });
    add("color.add_serial_before", tr("Add Serial Node Before"), [s] {
        if (s)
            s->addSerialNodeBefore();
    });
    add("color.add_parallel", tr("Add Parallel Node"), [s] {
        if (s)
            s->addParallelNode();
    });
    add("color.add_layer", tr("Add Layer Node"), [s] {
        if (s)
            s->addLayerNode();
    });
    menu->addSeparator();
    add("color.bypass_node", tr("Enable / Disable Node"), [s] {
        if (s)
            s->toggleNodeBypass();
    })->setEnabled(onNode);
    add("color.reset_node", tr("Reset Node Grade"), [s] {
        if (s)
            s->resetNode();
    })->setEnabled(onNode);
    add("color.delete_node", tr("Delete Node"), [s] {
        if (s)
            s->removeNode();
    })->setEnabled(onNode);
    menu->addSeparator();
    add("color.reset_grade", tr("Reset Clip Grade"), [s] {
        if (s)
            s->resetGrade();
    })->setEnabled(m_session && !m_session->grade().isEmpty());
    QAction* fit = menu->addAction(tr("Fit to View"));
    fit->setObjectName(QStringLiteral("color.fit_nodes"));
    connect(fit, &QAction::triggered, this, [this] {
        m_viewSet = false;
        fitToView();
    });
    return menu;
}

void ColorNodeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = buildContextMenu(event->pos(), this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(event->globalPos());
}

} // namespace kk
