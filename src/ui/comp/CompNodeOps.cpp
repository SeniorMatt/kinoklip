// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/comp/CompNodeOps.h"

#include "ui/common/ActionRegistry.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"

#include <QAction>

namespace kk {

CompNodeOps::CompNodeOps(NodeEditor* editor, CompSession* session, ActionRegistry* actions)
    : QObject(editor)
    , m_editor(editor)
    , m_session(session)
    , m_actions(actions)
{
    editor->setProperty("kkVimContext", QStringLiteral("nodes"));
    editor->setProperty(kNodesProperty, QVariant::fromValue<QObject*>(this));
    if (editor->focusPolicy() == Qt::NoFocus)
        editor->setFocusPolicy(Qt::StrongFocus);
}

QVector<VimNodeInfo> CompNodeOps::vimNodes() const
{
    QVector<VimNodeInfo> out;
    if (!m_editor || !m_session)
        return out;
    for (const CompNode& n : m_session->composition().nodes)
        out.append({n.id, m_editor->tileRect(n), n.name});
    return out;
}

QVector<std::pair<quint64, quint64>> CompNodeOps::vimLinks() const
{
    QVector<std::pair<quint64, quint64>> out;
    if (!m_session)
        return out;
    for (const CompNode& n : m_session->composition().nodes) {
        for (const NodeLink& link : n.inputs) {
            if (link.isConnected())
                out.append({link.nodeId, n.id});
        }
    }
    return out;
}

quint64 CompNodeOps::vimCurrent() const
{
    return m_session ? m_session->currentNode() : 0;
}

QVector<quint64> CompNodeOps::vimSelected() const
{
    return m_session ? m_session->selectedNodes() : QVector<quint64> {};
}

void CompNodeOps::vimSelect(quint64 id, bool toggle)
{
    if (!m_session)
        return;
    if (!toggle) {
        m_session->setSelection({id}, {}, id);
        return;
    }
    QVector<quint64> now = m_session->selectedNodes();
    if (now.contains(id))
        now.removeAll(id);
    else
        now.append(id);
    m_session->setSelection(now, m_session->selectedNotes(), now.contains(id) ? id : 0);
}

void CompNodeOps::vimSelectAll()
{
    if (m_session)
        m_session->selectAll();
}

void CompNodeOps::vimReveal(quint64 id)
{
    if (!m_editor || !m_session)
        return;
    const CompNode* node = m_session->composition().node(id);
    if (!node)
        return;
    const QRectF tile = m_editor->tileRect(*node);
    if (m_editor->rect().adjusted(8, 8, -8, -8).contains(tile.toRect()))
        return;
    m_editor->setView(m_editor->toGrid(tile.center()) - QPointF(0, 0), m_editor->zoom());
    emit m_editor->viewChanged(m_editor->centre(), m_editor->zoom());
}

bool CompNodeOps::vimConnect(quint64 from, quint64 to)
{
    if (!m_session)
        return false;
    const Composition& comp = m_session->composition();
    const CompNode* source = comp.node(from);
    const CompNode* target = comp.node(to);
    const compadapter::NodeTypeInfo* sourceType
        = source ? compadapter::nodeType(source->type) : nullptr;
    const compadapter::NodeTypeInfo* targetType
        = target ? compadapter::nodeType(target->type) : nullptr;
    if (!sourceType || !targetType || sourceType->outputCount == 0 || targetType->inputs.isEmpty())
        return false;
    // A free input of the same kind, the main one first; else the main input is replaced.
    QString port;
    const QString main = targetType->mainInput();
    if (!main.isEmpty() && !target->inputs.value(main).isConnected())
        port = main;
    for (const compadapter::PortInfo& p : targetType->inputs) {
        if (port.isEmpty() && p.kind == sourceType->outputKind
            && !target->inputs.value(p.id).isConnected())
            port = p.id;
    }
    if (port.isEmpty())
        port = main.isEmpty() ? targetType->inputs.first().id : main;
    return m_session->connectNodes(from, to, port);
}

bool CompNodeOps::vimDisconnect(quint64 to, bool all)
{
    if (!m_session)
        return false;
    const CompNode* node = m_session->composition().node(to);
    const compadapter::NodeTypeInfo* type = node ? compadapter::nodeType(node->type) : nullptr;
    if (!type)
        return false;
    bool any = false;
    const QString main = type->mainInput();
    for (const compadapter::PortInfo& p : type->inputs) {
        if (!node->inputs.value(p.id).isConnected())
            continue;
        if (all || p.id == main || type->inputs.size() == 1 || (main.isEmpty() && !any))
            any = m_session->disconnectInput(to, p.id) || any;
        if (!all && any)
            break;
    }
    return any;
}

bool CompNodeOps::vimCommand(const QString& verb)
{
    if (!m_session || !m_editor)
        return false;
    if (verb == QLatin1String("delete"))
        return m_session->deleteSelection();
    if (verb == QLatin1String("copy")) {
        m_session->copySelection();
        return true;
    }
    if (verb == QLatin1String("paste"))
        return m_session->paste(m_editor->pastePosition());
    if (verb == QLatin1String("rename")) {
        if (!m_session->currentNode())
            return false;
        m_editor->startRename(m_session->currentNode());
        return true;
    }
    if (verb == QLatin1String("bypass"))
        return m_session->togglePassThrough();
    if (verb == QLatin1String("fit")) {
        m_editor->fitToView();
        return true;
    }
    if (verb == QLatin1String("add") || verb == QLatin1String("add_after")
        || verb == QLatin1String("add_before")) {
        // The node library list ("Add Node..."): the page connects the new node after the
        // selected one.
        if (!m_actions || !m_actions->contains(QStringLiteral("comp.select_node")))
            return false;
        m_actions->action(QStringLiteral("comp.select_node"))->trigger();
        return true;
    }
    return false;
}

} // namespace kk
