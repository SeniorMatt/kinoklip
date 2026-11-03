// SPDX-License-Identifier: GPL-3.0-or-later
// CompSession implementation: graph commands as copy-mutate-store, one undo step each.
#include "ui/comp/CompSession.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/comp/CompAdapter.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <optional>

namespace kk {

namespace {

/// Where pasted / duplicated nodes go relative to the originals (grid units).
const QPointF kDuplicateOffset(1.0, 1.5);

/// The in-process node clipboard (the nodes and their internal links).
Composition& nodeClipboard()
{
    static Composition clip;
    return clip;
}

QPointF snapped(QPointF p)
{
    return QPointF(std::round(p.x()), std::round(p.y()));
}

} // namespace

CompSession::CompSession(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    // Edits, undo / redo and the stand-in store all end in Timeline::changed().
    connect(ctx.project->timeline(), &Timeline::changed, this, &CompSession::reload);
    connect(ctx.project, &Project::projectReset, this, [this] { openClip(0); });
}

quint64 CompSession::clipForPage() const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    if (m_ctx.selection) {
        for (quint64 id : m_ctx.selection->clips()) {
            const auto loc = editops::findClip(data, id);
            if (loc && loc->track.type == TrackType::Video)
                return id;
        }
    }
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    for (auto t = data.videoTracks.rbegin(); t != data.videoTracks.rend(); ++t) {
        if (const ClipData* c = t->clipAt(playhead))
            return c->id;
    }
    return 0;
}

void CompSession::openClip(quint64 clipId)
{
    if (clipId && !editops::clipById(m_ctx.project->timeline()->data(), clipId))
        clipId = 0;
    const bool other = clipId != m_clipId;
    m_clipId = clipId;
    if (other) {
        m_selNodes.clear();
        m_selNotes.clear();
        m_current = 0;
    }
    reload();
    if (other) {
        const std::optional<Composition> stored
            = clipId ? compadapter::clipComposition(*m_ctx.project, clipId) : std::nullopt;
        m_viewer[0] = stored ? stored->leftViewerNode : 0;
        m_viewer[1] = stored ? stored->rightViewerNode : 0;
        m_viewCentre = m_comp.viewCentre;
        m_viewZoom = m_comp.viewZoom;
        if (m_clipId && m_comp.mediaOutId)
            setSelection({m_comp.mediaOutId});
        emit clipChanged();
        emit viewersChanged();
        emit selectionChanged();
    }
}

const ClipData* CompSession::clip() const
{
    return m_clipId ? editops::clipById(m_ctx.project->timeline()->data(), m_clipId) : nullptr;
}

void CompSession::reload()
{
    if (m_clipId && !clip()) {
        // The clip went away (deleted, undo of its creation, another timeline).
        openClip(0);
        return;
    }
    std::optional<Composition> stored
        = m_clipId ? compadapter::clipComposition(*m_ctx.project, m_clipId) : std::nullopt;
    m_stored = stored.has_value();
    Composition next
        = m_clipId ? (stored ? *stored : compadapter::defaultComposition()) : Composition();
    if (next == m_comp)
        return;
    m_comp = std::move(next);
    pruneSelection();
    emit compositionChanged();
}

void CompSession::pruneSelection()
{
    const auto before = std::make_pair(m_selNodes, m_selNotes);
    const quint64 current = m_current;
    m_selNodes.erase(std::remove_if(m_selNodes.begin(), m_selNodes.end(),
                         [this](quint64 id) { return !m_comp.node(id); }),
        m_selNodes.end());
    m_selNotes.erase(std::remove_if(m_selNotes.begin(), m_selNotes.end(),
                         [this](quint64 id) {
                             return std::none_of(m_comp.notes.cbegin(), m_comp.notes.cend(),
                                 [id](const CompNote& n) { return n.id == id; });
                         }),
        m_selNotes.end());
    if (!m_selNodes.contains(m_current))
        m_current = m_selNodes.isEmpty() ? 0 : m_selNodes.last();
    for (quint64& v : m_viewer)
        if (v && !m_comp.node(v))
            v = 0;
    if (before != std::make_pair(m_selNodes, m_selNotes) || current != m_current)
        emit selectionChanged();
}

int64_t CompSession::compFrame() const
{
    const ClipData* c = clip();
    if (!c)
        return 0;
    const int64_t local = m_ctx.project->timeline()->playhead() - c->start;
    return std::clamp<int64_t>(local, 0, std::max<int64_t>(0, c->duration - 1));
}

int64_t CompSession::compFrameCount() const
{
    const ClipData* c = clip();
    return c ? c->duration : 0;
}

QSize CompSession::frameSize() const
{
    const TimelineData& d = m_ctx.project->timeline()->data();
    return QSize(d.width, d.height);
}

void CompSession::seekCompFrame(int64_t frame)
{
    if (const ClipData* c = clip())
        m_ctx.project->timeline()->setPlayhead(
            c->start + std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, c->duration - 1)));
}

void CompSession::stampViewState(Composition& comp) const
{
    comp.leftViewerNode = m_viewer[0];
    comp.rightViewerNode = m_viewer[1];
    comp.viewCentre = m_viewCentre;
    comp.viewZoom = m_viewZoom;
}

bool CompSession::edit(const QString& label, const std::function<bool(Composition&)>& mutate)
{
    if (!m_clipId)
        return false;
    Composition next = m_comp;
    if (!mutate(next) || next == m_comp)
        return false;
    stampViewState(next);
    return compadapter::setClipComposition(*m_ctx.project, m_clipId, next, label);
}

void CompSession::beginGesture(const QString& label)
{
    ++m_gestureDepth;
    compadapter::beginCompEdit(*m_ctx.project, label);
}

bool CompSession::modifyLive(const std::function<void(Composition&)>& mutate)
{
    if (!m_clipId)
        return false;
    Composition next = m_comp;
    mutate(next);
    if (next == m_comp)
        return false;
    stampViewState(next);
    return compadapter::modifyClipComposition(*m_ctx.project, m_clipId, next);
}

void CompSession::endGesture()
{
    if (m_gestureDepth <= 0)
        return;
    --m_gestureDepth;
    compadapter::endCompEdit(*m_ctx.project);
}

bool CompSession::resetComposition()
{
    if (!m_clipId || !m_stored)
        return false;
    return compadapter::setClipComposition(
        *m_ctx.project, m_clipId, std::nullopt, tr("Reset Composition"));
}

// ---- Node commands ----

quint64 CompSession::addNode(const QString& type)
{
    quint64 added = 0;
    const quint64 after = m_current;
    const QPointF centre = m_viewCentre;
    const compadapter::NodeTypeInfo* info = compadapter::nodeType(type);
    edit(tr("Add %1").arg(info ? info->name : type), [&](Composition& c) {
        c.viewCentre = centre;
        added = compadapter::addNodeAfter(c, type, after);
        return added != 0;
    });
    if (added)
        setSelection({added}, {}, added);
    return added;
}

quint64 CompSession::addNodeAt(
    const QString& type, QPointF position, quint64 linkTo, const QString& linkPort)
{
    quint64 added = 0;
    const compadapter::NodeTypeInfo* info = compadapter::nodeType(type);
    edit(tr("Add %1").arg(info ? info->name : type), [&](Composition& c) {
        added = compadapter::addNode(c, type, m_snap ? snapped(position) : position);
        if (added && linkTo)
            compadapter::insertIntoLink(c, added, linkTo, linkPort);
        return added != 0;
    });
    if (added)
        setSelection({added}, {}, added);
    return added;
}

quint64 CompSession::addNote(QPointF position, const QString& text)
{
    quint64 id = 0;
    edit(tr("Add Note"), [&](Composition& c) {
        CompNote note;
        note.id = id = c.allocateId();
        note.position = position;
        note.size = QSizeF(4.0, 2.0);
        note.text = text;
        c.notes.append(note);
        return true;
    });
    if (id)
        setSelection({}, {id});
    return id;
}

bool CompSession::setNoteText(quint64 id, const QString& text)
{
    return edit(tr("Edit Note"), [&](Composition& c) {
        for (CompNote& n : c.notes) {
            if (n.id == id) {
                n.text = text;
                return true;
            }
        }
        return false;
    });
}

bool CompSession::connectNodes(quint64 fromId, quint64 toId, const QString& port)
{
    return edit(tr("Connect"),
        [&](Composition& c) { return compadapter::connectNodes(c, fromId, 0, toId, port); });
}

bool CompSession::disconnectInput(quint64 toId, const QString& port)
{
    return edit(tr("Disconnect"),
        [&](Composition& c) { return compadapter::disconnectInput(c, toId, port); });
}

bool CompSession::moveSelection(QPointF delta, quint64 insertTo, const QString& insertPort)
{
    const QVector<quint64> nodes = m_selNodes;
    const QVector<quint64> notes = m_selNotes;
    const bool snap = m_snap;
    const bool inserting = insertTo && nodes.size() == 1 && notes.isEmpty();
    return edit(inserting ? tr("Insert Node") : tr("Move Nodes"), [&](Composition& c) {
        for (CompNode& n : c.nodes) {
            if (nodes.contains(n.id))
                n.position = snap ? snapped(n.position + delta) : n.position + delta;
        }
        for (CompNote& n : c.notes) {
            if (notes.contains(n.id))
                n.position = snap ? snapped(n.position + delta) : n.position + delta;
        }
        if (inserting)
            compadapter::insertIntoLink(c, nodes.first(), insertTo, insertPort);
        return true;
    });
}

bool CompSession::deleteSelection()
{
    const QSet<quint64> nodes(m_selNodes.cbegin(), m_selNodes.cend());
    const QSet<quint64> notes(m_selNotes.cbegin(), m_selNotes.cend());
    return edit(tr("Delete Nodes"),
        [&](Composition& c) { return compadapter::removeItems(c, nodes, notes); });
}

void CompSession::copySelection()
{
    const QSet<quint64> nodes(m_selNodes.cbegin(), m_selNodes.cend());
    Composition fragment = compadapter::extract(m_comp, nodes);
    if (!fragment.nodes.isEmpty())
        nodeClipboard() = std::move(fragment);
}

bool CompSession::cutSelection()
{
    copySelection();
    return edit(tr("Cut Nodes"), [this](Composition& c) {
        return compadapter::removeItems(c, QSet<quint64>(m_selNodes.cbegin(), m_selNodes.cend()),
            QSet<quint64>(m_selNotes.cbegin(), m_selNotes.cend()));
    });
}

bool CompSession::canPaste()
{
    return !nodeClipboard().nodes.isEmpty();
}

bool CompSession::paste(QPointF position)
{
    const Composition& fragment = nodeClipboard();
    if (fragment.nodes.isEmpty())
        return false;
    QPointF topLeft = fragment.nodes.first().position;
    for (const CompNode& n : fragment.nodes)
        topLeft
            = QPointF(std::min(topLeft.x(), n.position.x()), std::min(topLeft.y(), n.position.y()));
    QVector<quint64> added;
    edit(tr("Paste Nodes"), [&](Composition& c) {
        added = compadapter::paste(c, fragment, position - topLeft);
        return !added.isEmpty();
    });
    if (added.isEmpty())
        return false;
    setSelection(added, {}, added.last());
    return true;
}

bool CompSession::duplicateSelection()
{
    const QSet<quint64> ids(m_selNodes.cbegin(), m_selNodes.cend());
    const Composition fragment = compadapter::extract(m_comp, ids);
    if (fragment.nodes.isEmpty())
        return false;
    QVector<quint64> added;
    edit(tr("Duplicate Nodes"), [&](Composition& c) {
        added = compadapter::paste(c, fragment, kDuplicateOffset);
        return !added.isEmpty();
    });
    if (added.isEmpty())
        return false;
    setSelection(added, {}, added.last());
    return true;
}

bool CompSession::renameNode(quint64 id, const QString& name)
{
    return edit(
        tr("Rename Node"), [&](Composition& c) { return compadapter::renameNode(c, id, name); });
}

bool CompSession::togglePassThrough()
{
    const QVector<quint64> ids = m_selNodes;
    if (ids.isEmpty())
        return false;
    bool anyEnabled = false;
    for (quint64 id : ids)
        if (const CompNode* n = m_comp.node(id); n && n->enabled)
            anyEnabled = true;
    return edit(anyEnabled ? tr("Pass Through On") : tr("Pass Through Off"), [&](Composition& c) {
        for (quint64 id : ids)
            if (CompNode* n = c.node(id); n && n->id != c.mediaOutId)
                n->enabled = !anyEnabled;
        return true;
    });
}

bool CompSession::setNodeEnabled(quint64 id, bool enabled)
{
    return edit(enabled ? tr("Pass Through Off") : tr("Pass Through On"), [&](Composition& c) {
        CompNode* n = c.node(id);
        if (!n || n->id == c.mediaOutId)
            return false;
        n->enabled = enabled;
        return true;
    });
}

bool CompSession::toggleLock()
{
    const QVector<quint64> ids = m_selNodes;
    bool anyUnlocked = false;
    for (quint64 id : ids)
        if (const CompNode* n = m_comp.node(id); n && !n->locked)
            anyUnlocked = true;
    return edit(anyUnlocked ? tr("Lock Node") : tr("Unlock Node"), [&](Composition& c) {
        for (quint64 id : ids)
            if (CompNode* n = c.node(id))
                n->locked = anyUnlocked;
        return !ids.isEmpty();
    });
}

bool CompSession::swapInputs()
{
    const quint64 id = m_current;
    return edit(
        tr("Swap Inputs"), [&](Composition& c) { return compadapter::swapMainInputs(c, id); });
}

// ---- Selection ----

void CompSession::setSelection(
    const QVector<quint64>& nodes, const QVector<quint64>& notes, quint64 current)
{
    QVector<quint64> n;
    for (quint64 id : nodes)
        if (m_comp.node(id) && !n.contains(id))
            n.append(id);
    if (!n.contains(current))
        current = n.isEmpty() ? 0 : n.last();
    if (n == m_selNodes && notes == m_selNotes && current == m_current)
        return;
    m_selNodes = n;
    m_selNotes = notes;
    m_current = current;
    emit selectionChanged();
}

void CompSession::selectAll()
{
    QVector<quint64> nodes, notes;
    for (const CompNode& n : m_comp.nodes)
        nodes.append(n.id);
    for (const CompNote& n : m_comp.notes)
        notes.append(n.id);
    setSelection(nodes, notes, m_current);
}

// ---- Viewers ----

quint64 CompSession::viewerNode(int index) const
{
    if (index < 0 || index > 1)
        return 0;
    if (index == 1 && m_viewer[1] == 0)
        return m_comp.mediaOutId;
    return m_viewer[index];
}

void CompSession::setViewerNode(int index, quint64 nodeId)
{
    if (index < 0 || index > 1 || (nodeId && !m_comp.node(nodeId)))
        return;
    if (index == 1 && nodeId == m_comp.mediaOutId)
        nodeId = 0; // the default
    if (m_viewer[index] == nodeId)
        return;
    m_viewer[index] = nodeId;
    emit viewersChanged();
}

void CompSession::setViewState(QPointF centre, double zoom)
{
    m_viewCentre = centre;
    m_viewZoom = zoom;
}

} // namespace kk
