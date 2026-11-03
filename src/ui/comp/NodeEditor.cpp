// SPDX-License-Identifier: GPL-3.0-or-later
// NodeEditor implementation: view maths, hit testing, mouse / key gestures, drag and drop.
// Painting is in NodeEditorPaint.cpp.
#include "ui/comp/NodeEditor.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/DragMime.h"
#include "ui/comp/CompSession.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace kk {

namespace {

/// Port hit radius in pixels (at least; grows with the zoom).
constexpr double kPortHitPx = 7.0;
/// Distance (pixels) within which a click hits a link.
constexpr double kLinkHitPx = 6.0;
/// Samples per link curve for hit testing (cheap, exact enough at 6 px tolerance).
constexpr int kCurveSamples = 20;
/// Wheel zoom: factor per 120 eighths of a degree (one notch).
constexpr double kWheelZoomStep = 1.2;
/// Margin around the graph after Fit, in grid units.
constexpr double kFitMargin = 1.0;

QPointF bezier(QPointF a, QPointF b, QPointF c, QPointF d, double t)
{
    const double u = 1.0 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

double distanceToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    const QPointF q = a + t * ab;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

} // namespace

NodeEditor::NodeEditor(CompSession* session, ActionRegistry* actions, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("NodeEditor"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 120);
    connect(session, &CompSession::compositionChanged, this, qOverload<>(&QWidget::update));
    connect(session, &CompSession::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(session, &CompSession::viewersChanged, this, qOverload<>(&QWidget::update));
    connect(session, &CompSession::clipChanged, this, [this] {
        commitRename(false);
        commitNoteEdit(false);
        const Composition& c = m_session->composition();
        if (c.nodes.isEmpty())
            return;
        if (c.viewZoom > 0 && (c.viewCentre != QPointF() || c.viewZoom != 1.0))
            setView(c.viewCentre, c.viewZoom);
        else
            fitToView();
    });
}

// ---- View ----

void NodeEditor::setView(QPointF centre, double zoom)
{
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (centre == m_centre && zoom == m_zoom)
        return;
    m_centre = centre;
    m_zoom = zoom;
    if (m_session)
        m_session->setViewState(m_centre, m_zoom);
    emit viewChanged(m_centre, m_zoom);
    update();
}

void NodeEditor::zoomAt(QPointF anchor, double factor)
{
    const QPointF before = toGrid(anchor);
    const double zoom = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
    // Keep `before` under the anchor: centre = before - (anchor - widgetCentre) / newScale.
    const QPointF offset = (anchor - QRectF(rect()).center()) / (kUnitPx * zoom);
    setView(before - offset, zoom);
}

void NodeEditor::fitToView()
{
    if (!m_session)
        return;
    const Composition& c = m_session->composition();
    QRectF bounds;
    for (const CompNode& n : c.nodes)
        bounds |= QRectF(n.position, QSizeF(kTileWidth, 1.0));
    for (const CompNote& n : c.notes)
        bounds |= QRectF(n.position, n.size);
    if (bounds.isNull()) {
        setView(QPointF(), 1.0);
        return;
    }
    bounds.adjust(-kFitMargin, -kFitMargin, kFitMargin, kFitMargin);
    const double zx = width() / (bounds.width() * kUnitPx);
    const double zy = height() / (bounds.height() * kUnitPx);
    setView(bounds.center(), std::min({zx, zy, 1.0}));
}

QPointF NodeEditor::toGrid(QPointF widgetPos) const
{
    return m_centre + (widgetPos - QRectF(rect()).center()) / (kUnitPx * m_zoom);
}

QPointF NodeEditor::fromGrid(QPointF gridPos) const
{
    return QRectF(rect()).center() + (gridPos - m_centre) * (kUnitPx * m_zoom);
}

QPointF NodeEditor::pastePosition() const
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    return rect().contains(local) ? toGrid(local) : m_centre;
}

// ---- Geometry ----

bool NodeEditor::isMoving(quint64 nodeId) const
{
    return m_drag == Drag::Move && m_session && m_session->selectedNodes().contains(nodeId);
}

QPointF NodeEditor::livePosition(const CompNode& node) const
{
    return isMoving(node.id) ? node.position + m_moveDelta : node.position;
}

QRectF NodeEditor::tileRect(const CompNode& node) const
{
    const double s = kUnitPx * m_zoom;
    return QRectF(fromGrid(livePosition(node)), QSizeF(kTileWidth * s, s));
}

bool NodeEditor::isTopPort(const compadapter::NodeTypeInfo& info, const QString& port)
{
    return port != info.mainInput();
}

QPointF NodeEditor::inputPos(const CompNode& node, const QString& port) const
{
    const compadapter::NodeTypeInfo* info = compadapter::nodeType(node.type);
    if (!info || info->inputIndex(port) < 0)
        return {};
    const QRectF r = tileRect(node);
    const int side = inputSide(node, port);
    if (side == 0)
        return QPointF(r.left(), r.center().y());
    // Top / bottom edge: the inputs on that edge spread evenly, ordered by where their sources
    // sit (left to right, so links do not cross); free inputs after them in descriptor order.
    const auto sourceX = [this, &node](const QString& id) {
        const NodeLink link = node.inputs.value(id);
        const CompNode* source = m_session && link.isConnected()
            ? m_session->composition().node(link.nodeId)
            : nullptr;
        return source ? outputPos(*source).x() : std::numeric_limits<double>::max();
    };
    QVector<std::pair<double, QString>> onEdge;
    for (const compadapter::PortInfo& p : info->inputs) {
        if (inputSide(node, p.id) == side)
            onEdge.append({sourceX(p.id), p.id});
    }
    std::stable_sort(onEdge.begin(), onEdge.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    int index = 0;
    for (int i = 0; i < onEdge.size(); ++i) {
        if (onEdge[i].second == port)
            index = i;
    }
    const int count = int(onEdge.size());
    return QPointF(
        r.left() + r.width() * (index + 1) / (count + 1), side > 0 ? r.bottom() : r.top());
}

int NodeEditor::inputSide(const CompNode& node, const QString& port) const
{
    const compadapter::NodeTypeInfo* info = compadapter::nodeType(node.type);
    if (!info || info->inputIndex(port) < 0)
        return 0;
    const bool main = !isTopPort(*info, port);
    const NodeLink link = node.inputs.value(port);
    const CompNode* source
        = m_session && link.isConnected() ? m_session->composition().node(link.nodeId) : nullptr;
    if (!source || source->id == node.id)
        return main ? 0 : -1;
    const QRectF r = tileRect(node);
    const QPointF s = outputPos(*source);
    if (main) {
        // The usual left-to-right flow keeps the left edge; a source above or below and to
        // the right comes in across the top / bottom instead of looping back.
        if (s.x() <= r.left() + r.width() * 0.25)
            return 0;
        return s.y() <= r.center().y() ? -1 : 1;
    }
    return s.y() > r.bottom() + r.height() * 0.5 ? 1 : -1;
}

QPointF NodeEditor::outputPos(const CompNode& node) const
{
    const QRectF r = tileRect(node);
    return QPointF(r.right(), r.center().y());
}

void NodeEditor::linkCurve(QPointF from, QPointF to, int side, QPointF& c1, QPointF& c2)
{
    const double dx = std::max(30.0, std::abs(to.x() - from.x()) * 0.5);
    c1 = from + QPointF(dx, 0);
    if (side != 0) {
        // Across the top / bottom edge; a short lead-in when the source is about level, so
        // a neighbour's link does not loop high over the tile.
        const double dy = std::max(16.0, std::abs(to.y() - from.y()) * 0.5);
        c2 = to + QPointF(0, side * dy);
    } else {
        c2 = to - QPointF(dx, 0);
    }
}

NodeEditor::Hit NodeEditor::linkAt(QPointF pos, quint64 exclude) const
{
    Hit best;
    if (!m_session)
        return best;
    const Composition& c = m_session->composition();
    double bestDist = kLinkHitPx;
    for (const CompNode& to : c.nodes) {
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(to.type);
        if (!info)
            continue;
        for (auto it = to.inputs.cbegin(); it != to.inputs.cend(); ++it) {
            const CompNode* from = c.node(it->nodeId);
            if (!from || (exclude && (to.id == exclude || from->id == exclude)))
                continue;
            const QPointF a = outputPos(*from), d = inputPos(to, it.key());
            QRectF box = QRectF(a, d).normalized().adjusted(-60, -60, 60, 60);
            if (!box.contains(pos))
                continue; // cheap reject before sampling the curve
            QPointF b, cc;
            linkCurve(a, d, inputSide(to, it.key()), b, cc);
            QPointF prev = a;
            for (int i = 1; i <= kCurveSamples; ++i) {
                const QPointF q = bezier(a, b, cc, d, double(i) / kCurveSamples);
                const double dist = distanceToSegment(pos, prev, q);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = Hit {Hit::Kind::Link, to.id, it.key(), 0};
                }
                prev = q;
            }
        }
    }
    return best;
}

NodeEditor::Hit NodeEditor::hitTest(QPointF pos) const
{
    if (!m_session)
        return {};
    const Composition& c = m_session->composition();
    const double portR = std::max(kPortHitPx, 0.25 * kUnitPx * m_zoom);
    // Tiles drawn last are on top: search backwards.
    for (auto it = c.nodes.crbegin(); it != c.nodes.crend(); ++it) {
        const CompNode& n = *it;
        const QRectF r = tileRect(n);
        if (!r.adjusted(-portR, -portR, portR, portR).contains(pos))
            continue;
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(n.type);
        if (info) {
            for (const compadapter::PortInfo& p : info->inputs) {
                const QPointF d = inputPos(n, p.id) - pos;
                if (std::hypot(d.x(), d.y()) <= portR)
                    return Hit {Hit::Kind::Input, n.id, p.id, 0};
            }
            if (info->outputCount > 0) {
                const QPointF d = outputPos(n) - pos;
                if (std::hypot(d.x(), d.y()) <= portR)
                    return Hit {Hit::Kind::Output, n.id, {}, 0};
            }
        }
        if (r.contains(pos))
            return Hit {Hit::Kind::Tile, n.id, {}, 0};
    }
    for (auto it = c.notes.crbegin(); it != c.notes.crend(); ++it) {
        const double s = kUnitPx * m_zoom;
        const QPointF p = m_drag == Drag::Move && m_session->selectedNotes().contains(it->id)
            ? it->position + m_moveDelta
            : it->position;
        if (QRectF(fromGrid(p), it->size * s).contains(pos))
            return Hit {Hit::Kind::Note, 0, {}, it->id};
    }
    return linkAt(pos);
}

NodeEditor::Hit NodeEditor::insertTarget() const
{
    if (m_drag != Drag::Move || !m_session || m_session->selectedNodes().size() != 1
        || !m_session->selectedNotes().isEmpty())
        return {};
    const Composition& c = m_session->composition();
    const CompNode* n = c.node(m_session->selectedNodes().first());
    const compadapter::NodeTypeInfo* info = n ? compadapter::nodeType(n->type) : nullptr;
    if (!info || info->outputCount == 0 || info->mainInput().isEmpty()
        || n->inputs.value(info->mainInput()).isConnected())
        return {};
    for (const CompNode& other : c.nodes)
        for (const NodeLink& l : other.inputs)
            if (l.nodeId == n->id)
                return {}; // its output is in use
    return linkAt(tileRect(*n).center(), n->id);
}

// ---- Keys ----

QString NodeEditor::actionForKey(const QKeyEvent* event) const
{
    if (!m_actions || event->key() == 0 || event->key() == Qt::Key_unknown)
        return {};
    // Number pad digits count as the digits (View in Left / Right Viewer).
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const QKeySequence seq(QKeyCombination(mods, Qt::Key(event->key())));
    for (const QString& id : m_actions->menuEntries(QStringLiteral("comp"))) {
        if (!m_actions->contains(id))
            continue;
        const QAction* a = m_actions->action(id);
        if (!a->isEnabled())
            continue;
        for (const QKeySequence& k : a->shortcuts())
            if (k == seq)
                return id;
    }
    return {};
}

bool NodeEditor::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Space pans while held (and plays / stops on a plain tap, see keyReleaseEvent()).
        if ((key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier)
            || (key->key() == Qt::Key_Escape && m_drag != Drag::None)
            || !actionForKey(key).isEmpty()) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void NodeEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        if (!event->isAutoRepeat()) {
            m_spaceHeld = true;
            m_spacePanned = false;
            setCursor(Qt::OpenHandCursor);
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_drag != Drag::None) {
            m_moveDelta = {};
            setDrag(Drag::None);
            update();
        }
        commitRename(false);
        commitNoteEdit(false);
        return;
    }
    const QString id = actionForKey(event);
    if (!id.isEmpty()) {
        m_actions->action(id)->trigger();
        return;
    }
    QWidget::keyPressEvent(event);
}

void NodeEditor::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        const bool tap = m_spaceHeld && !m_spacePanned && m_drag != Drag::Pan;
        m_spaceHeld = false;
        unsetCursor();
        if (m_drag == Drag::Pan)
            setDrag(Drag::None);
        // A tap without panning is the transport's Space (play / stop), as everywhere else.
        if (tap && m_actions && m_actions->contains(QStringLiteral("transport.play_pause")))
            m_actions->action(QStringLiteral("transport.play_pause"))->trigger();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void NodeEditor::focusOutEvent(QFocusEvent* event)
{
    m_spaceHeld = false;
    QWidget::focusOutEvent(event);
}

// ---- Mouse ----

void NodeEditor::setDrag(Drag drag)
{
    m_drag = drag;
    if (drag == Drag::None) {
        m_moveDelta = {};
        m_linkSource = 0;
    }
}

void NodeEditor::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    commitRename(true);
    commitNoteEdit(true);
    m_pressPos = m_mousePos = event->position();
    m_pressMods = event->modifiers();
    m_pressNode = m_pressNote = 0;
    m_pressPort.clear();
    if (!m_session || !m_session->clipId())
        return;
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_spaceHeld)) {
        setDrag(Drag::Pan);
        m_spacePanned = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const Hit hit = hitTest(m_pressPos);
    const bool toggle = m_pressMods & (Qt::ControlModifier | Qt::ShiftModifier);
    const Composition& comp = m_session->composition();
    switch (hit.kind) {
    case Hit::Kind::Output:
        m_pressNode = hit.node;
        setDrag(Drag::Connect);
        return;
    case Hit::Kind::Input: {
        m_pressNode = hit.node;
        m_pressPort = hit.port;
        const CompNode* n = comp.node(hit.node);
        const NodeLink link = n ? n->inputs.value(hit.port) : NodeLink {};
        if (link.isConnected()) {
            m_linkSource = link.nodeId;
            setDrag(Drag::Reconnect);
        } else {
            setDrag(Drag::ConnectReverse);
        }
        update();
        return;
    }
    case Hit::Kind::Tile: {
        m_pressNode = hit.node;
        QVector<quint64> nodes = m_session->selectedNodes();
        QVector<quint64> notes = m_session->selectedNotes();
        if (toggle) {
            if (nodes.contains(hit.node))
                nodes.removeAll(hit.node);
            else
                nodes.append(hit.node);
        } else if (!nodes.contains(hit.node)) {
            nodes = {hit.node};
            notes.clear();
        }
        m_session->setSelection(nodes, notes, hit.node);
        setDrag(nodes.contains(hit.node) ? Drag::Pending : Drag::None);
        return;
    }
    case Hit::Kind::Note: {
        m_pressNote = hit.note;
        QVector<quint64> nodes = m_session->selectedNodes();
        QVector<quint64> notes = m_session->selectedNotes();
        if (toggle) {
            if (notes.contains(hit.note))
                notes.removeAll(hit.note);
            else
                notes.append(hit.note);
        } else if (!notes.contains(hit.note)) {
            notes = {hit.note};
            nodes.clear();
        }
        m_session->setSelection(nodes, notes, m_session->currentNode());
        setDrag(notes.contains(hit.note) ? Drag::Pending : Drag::None);
        return;
    }
    case Hit::Kind::Link:
    case Hit::Kind::None:
        setDrag(Drag::Box);
        update();
        return;
    }
}

void NodeEditor::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const QPointF last = m_mousePos;
    m_mousePos = pos;
    const double scale = kUnitPx * m_zoom;
    switch (m_drag) {
    case Drag::None:
        return;
    case Drag::Pan:
        setView(m_centre - (pos - last) / scale, m_zoom);
        return;
    case Drag::Pending:
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        setDrag(Drag::Move);
        [[fallthrough]];
    case Drag::Move:
        // Out of the editor with a node: hand it to the viewers as a drag (spec 42b).
        if (m_pressNode && !rect().contains(pos.toPoint())) {
            const quint64 node = m_pressNode;
            setDrag(Drag::None);
            update();
            startNodeDrag(node);
            return;
        }
        m_moveDelta = (pos - m_pressPos) / scale;
        update();
        return;
    default:
        update();
        return;
    }
}

void NodeEditor::mouseReleaseEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    m_mousePos = pos;
    if (!m_session)
        return;
    switch (m_drag) {
    case Drag::Pan:
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
        if (!m_spaceHeld)
            unsetCursor();
        break;
    case Drag::Pending:
        // A plain click on a node of a multiple selection selects only that node.
        if (!(m_pressMods & (Qt::ControlModifier | Qt::ShiftModifier))) {
            if (m_pressNode)
                m_session->setSelection({m_pressNode}, {}, m_pressNode);
            else if (m_pressNote)
                m_session->setSelection({}, {m_pressNote});
        }
        break;
    case Drag::Move: {
        const Hit target = insertTarget();
        const QPointF delta = m_moveDelta;
        setDrag(Drag::None); // paint the stored positions from here on
        if (!delta.isNull())
            m_session->moveSelection(delta, target.node, target.port);
        break;
    }
    case Drag::Box:
        finishBox(m_pressMods);
        break;
    case Drag::Connect:
    case Drag::ConnectReverse:
    case Drag::Reconnect:
        finishConnect(pos);
        break;
    case Drag::None:
        break;
    }
    setDrag(Drag::None);
    update();
}

void NodeEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Hit hit = hitTest(event->position());
    if (hit.kind == Hit::Kind::Note) {
        startNoteEdit(hit.note);
        return;
    }
    if (hit.kind == Hit::Kind::Tile) {
        startRename(hit.node);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void NodeEditor::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;
    zoomAt(event->position(), std::pow(kWheelZoomStep, delta / 120.0));
    event->accept();
}

void NodeEditor::finishBox(Qt::KeyboardModifiers mods)
{
    const QRectF box = QRectF(m_pressPos, m_mousePos).normalized();
    const bool add = mods & (Qt::ControlModifier | Qt::ShiftModifier);
    QVector<quint64> nodes = add ? m_session->selectedNodes() : QVector<quint64> {};
    QVector<quint64> notes = add ? m_session->selectedNotes() : QVector<quint64> {};
    const Composition& c = m_session->composition();
    const double s = kUnitPx * m_zoom;
    if (box.width() >= 2 || box.height() >= 2) {
        for (const CompNode& n : c.nodes)
            if (box.intersects(tileRect(n)) && !nodes.contains(n.id))
                nodes.append(n.id);
        for (const CompNote& n : c.notes)
            if (box.intersects(QRectF(fromGrid(n.position), n.size * s)) && !notes.contains(n.id))
                notes.append(n.id);
    }
    m_session->setSelection(nodes, notes, nodes.isEmpty() ? 0 : nodes.last());
}

void NodeEditor::finishConnect(QPointF pos)
{
    const Hit hit = hitTest(pos);
    const Composition& c = m_session->composition();
    // The input of a tile that a link dropped on it should take: the main input when free,
    // else the first free one, else the main input.
    // The input of a tile that a link dropped on it should take: for a mask source the first
    // free mask input; else the main input when free, else the first free one, else the main.
    const quint64 sourceId = m_drag == Drag::Reconnect ? m_linkSource : m_pressNode;
    const CompNode* source = c.node(sourceId);
    const compadapter::NodeTypeInfo* sourceInfo
        = source ? compadapter::nodeType(source->type) : nullptr;
    const bool maskSource = sourceInfo && sourceInfo->outputKind == PortKind::Mask;
    auto inputForTile = [&c, maskSource](quint64 nodeId) -> QString {
        const CompNode* n = c.node(nodeId);
        const compadapter::NodeTypeInfo* info = n ? compadapter::nodeType(n->type) : nullptr;
        if (!info || info->inputs.isEmpty())
            return {};
        if (maskSource) {
            for (const compadapter::PortInfo& p : info->inputs)
                if (p.kind == PortKind::Mask && !n->inputs.value(p.id).isConnected())
                    return p.id;
        }
        const QString main = info->mainInput();
        if (!main.isEmpty() && !n->inputs.value(main).isConnected())
            return main;
        for (const compadapter::PortInfo& p : info->inputs)
            if (!n->inputs.value(p.id).isConnected())
                return p.id;
        return main.isEmpty() ? info->inputs.first().id : main;
    };
    switch (m_drag) {
    case Drag::Connect: {
        quint64 to = 0;
        QString port;
        if (hit.kind == Hit::Kind::Input) {
            to = hit.node;
            port = hit.port;
        } else if (hit.kind == Hit::Kind::Tile) {
            to = hit.node;
            port = inputForTile(to);
        }
        if (to && !port.isEmpty())
            m_session->connectNodes(m_pressNode, to, port);
        return;
    }
    case Drag::ConnectReverse:
        if ((hit.kind == Hit::Kind::Output || hit.kind == Hit::Kind::Tile)
            && hit.node != m_pressNode)
            m_session->connectNodes(hit.node, m_pressNode, m_pressPort);
        return;
    case Drag::Reconnect: {
        const quint64 oldTo = m_pressNode;
        const QString oldPort = m_pressPort;
        const quint64 source = m_linkSource;
        quint64 to = 0;
        QString port;
        if (hit.kind == Hit::Kind::Input) {
            to = hit.node;
            port = hit.port;
        } else if (hit.kind == Hit::Kind::Tile && hit.node != oldTo) {
            to = hit.node;
            port = inputForTile(to);
        }
        if (to == oldTo && port == oldPort)
            return; // dropped back where it was
        if (to && !port.isEmpty()) {
            m_session->edit(tr("Connect"), [&](Composition& g) {
                compadapter::disconnectInput(g, oldTo, oldPort);
                return compadapter::connectNodes(g, source, 0, to, port);
            });
        } else {
            m_session->disconnectInput(oldTo, oldPort);
        }
        return;
    }
    default:
        return;
    }
}

void NodeEditor::startNodeDrag(quint64 nodeId)
{
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(mime::CompNode), QByteArray::number(nodeId));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
}

// ---- Context menu ----

QMenu* NodeEditor::buildContextMenu(QPointF pos, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("NodeEditorMenu"));
    if (!m_session || !m_session->clipId() || !m_actions)
        return menu;
    auto add = [this, menu](const char* id) {
        if (m_actions->contains(QLatin1String(id)))
            menu->addAction(m_actions->action(QLatin1String(id)));
    };
    const Hit hit = hitTest(pos);
    if (hit.kind == Hit::Kind::Tile || hit.kind == Hit::Kind::Input
        || hit.kind == Hit::Kind::Output) {
        if (!m_session->selectedNodes().contains(hit.node))
            m_session->setSelection({hit.node}, {}, hit.node);
        const CompNode* n = m_session->composition().node(hit.node);
        if (n && m_actions->contains(QStringLiteral("comp.pass_through"))) {
            m_actions->action(QStringLiteral("comp.pass_through"))->setChecked(!n->enabled);
            m_actions->action(QStringLiteral("comp.lock"))->setChecked(n->locked);
        }
        add("comp.view_left");
        add("comp.view_right");
        menu->addSeparator();
        add("comp.rename");
        add("comp.pass_through");
        add("comp.lock");
        add("comp.swap_inputs");
        menu->addSeparator();
        add("comp.cut");
        add("comp.copy");
        add("comp.duplicate");
        add("comp.delete");
        return menu;
    }
    if (hit.kind == Hit::Kind::Note) {
        menu->addAction(tr("Edit Note"), this, [this, id = hit.note] { startNoteEdit(id); });
        if (!m_session->selectedNotes().contains(hit.note))
            m_session->setSelection({}, {hit.note});
        add("comp.delete");
        return menu;
    }
    // Background: add any node here.
    QMenu* addMenu = menu->addMenu(tr("Add Node"));
    addMenu->setObjectName(QStringLiteral("AddNodeMenu"));
    const QPointF at = toGrid(pos) - QPointF(kTileWidth / 2, 0.5);
    for (const QString& category : compadapter::categories()) {
        QMenu* sub = addMenu->addMenu(category);
        for (const compadapter::NodeTypeInfo& info : compadapter::nodeTypes()) {
            if (info.category != category)
                continue;
            const QString type = info.type;
            sub->addAction(info.name, this, [this, type, at] { m_session->addNodeAt(type, at); });
        }
    }
    add("comp.select_node");
    add("comp.add_note");
    menu->addSeparator();
    add("comp.paste");
    add("comp.select_all");
    menu->addSeparator();
    add("comp.fit");
    add("comp.snap_grid");
    return menu;
}

void NodeEditor::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = buildContextMenu(event->pos(), this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(event->globalPos());
}

// ---- Drag and drop (Nodes library) ----

void NodeEditor::dragEnterEvent(QDragEnterEvent* event)
{
    if (m_session && m_session->clipId()
        && event->mimeData()->hasFormat(QLatin1String(mime::CompNodeType)))
        event->acceptProposedAction();
}

void NodeEditor::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event->mimeData()->hasFormat(QLatin1String(mime::CompNodeType)))
        return;
    m_dropLink = linkAt(event->position());
    event->acceptProposedAction();
    update();
}

void NodeEditor::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_dropLink = {};
    update();
    QWidget::dragLeaveEvent(event);
}

void NodeEditor::dropEvent(QDropEvent* event)
{
    const QString type
        = QString::fromUtf8(event->mimeData()->data(QLatin1String(mime::CompNodeType)));
    const Hit link = linkAt(event->position());
    m_dropLink = {};
    if (!m_session || type.isEmpty())
        return;
    m_session->addNodeAt(
        type, toGrid(event->position()) - QPointF(kTileWidth / 2, 0.5), link.node, link.port);
    event->acceptProposedAction();
    setFocus();
    update();
}

// ---- Inline editors ----

void NodeEditor::startRename(quint64 id)
{
    commitRename(false);
    const CompNode* n = m_session ? m_session->composition().node(id) : nullptr;
    if (!n)
        return;
    m_renameNode = id;
    m_rename = new QLineEdit(n->name, this);
    m_rename->setObjectName(QStringLiteral("NodeRenameField"));
    m_rename->setFrame(true);
    m_rename->setAlignment(Qt::AlignCenter);
    QRect r = tileRect(*n).toAlignedRect();
    r.setHeight(std::max(r.height(), m_rename->sizeHint().height()));
    r.setWidth(std::max(r.width(), 120));
    m_rename->setGeometry(r);
    m_rename->selectAll();
    m_rename->show();
    m_rename->setFocus();
    m_rename->installEventFilter(this);
    connect(m_rename, &QLineEdit::returnPressed, this, [this] { commitRename(true); });
}

void NodeEditor::commitRename(bool accept)
{
    if (!m_rename)
        return;
    QLineEdit* field = std::exchange(m_rename, nullptr);
    const QString name = field->text();
    const quint64 id = std::exchange(m_renameNode, 0);
    field->removeEventFilter(this);
    if (field->hasFocus())
        setFocus(); // keep the keys in the editor (not when focus already went elsewhere)
    field->hide();
    field->deleteLater();
    if (accept && m_session)
        m_session->renameNode(id, name);
}

void NodeEditor::startNoteEdit(quint64 id)
{
    commitNoteEdit(false);
    if (!m_session)
        return;
    const Composition& c = m_session->composition();
    const auto it = std::find_if(
        c.notes.cbegin(), c.notes.cend(), [id](const CompNote& n) { return n.id == id; });
    if (it == c.notes.cend())
        return;
    m_editNote = id;
    m_noteEdit = new QPlainTextEdit(it->text, this);
    m_noteEdit->setObjectName(QStringLiteral("NoteEditField"));
    m_noteEdit->setGeometry(
        QRectF(fromGrid(it->position), it->size * (kUnitPx * m_zoom)).toAlignedRect());
    m_noteEdit->show();
    m_noteEdit->setFocus();
    m_noteEdit->installEventFilter(this);
}

void NodeEditor::commitNoteEdit(bool accept)
{
    if (!m_noteEdit)
        return;
    QPlainTextEdit* field = std::exchange(m_noteEdit, nullptr);
    const QString text = field->toPlainText();
    const quint64 id = std::exchange(m_editNote, 0);
    field->removeEventFilter(this);
    field->hide();
    field->deleteLater();
    if (accept && m_session)
        m_session->setNoteText(id, text);
}

bool NodeEditor::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_rename || watched == m_noteEdit) && event) {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                if (watched == m_rename)
                    commitRename(false);
                else
                    commitNoteEdit(false);
                setFocus();
                return true;
            }
            if (watched == m_noteEdit
                && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && (key->modifiers() & Qt::ControlModifier)) {
                commitNoteEdit(true);
                setFocus();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            if (watched == m_rename)
                QMetaObject::invokeMethod(
                    this, [this] { commitRename(true); }, Qt::QueuedConnection);
            else
                QMetaObject::invokeMethod(
                    this, [this] { commitNoteEdit(true); }, Qt::QueuedConnection);
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
