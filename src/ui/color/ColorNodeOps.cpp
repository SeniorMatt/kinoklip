// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/color/ColorNodeOps.h"

#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"

#include <QAction>

namespace kk {

ColorNodeOps::ColorNodeOps(ColorNodeEditor* editor, ColorSession* session, ActionRegistry* actions)
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

QVector<VimNodeInfo> ColorNodeOps::vimNodes() const
{
    QVector<VimNodeInfo> out;
    if (!m_editor || !m_session)
        return out;
    for (const GradeNode& n : m_session->grade().nodes)
        out.append({n.id, m_editor->tileRect(n), n.label});
    return out;
}

QVector<std::pair<quint64, quint64>> ColorNodeOps::vimLinks() const
{
    QVector<std::pair<quint64, quint64>> out;
    if (!m_session)
        return out;
    for (const GradeNode& n : m_session->grade().nodes) {
        for (const quint64 from : n.inputs) {
            if (from != 0)
                out.append({from, n.id});
        }
    }
    return out;
}

quint64 ColorNodeOps::vimCurrent() const
{
    return m_session ? m_session->selectedNode() : 0;
}

QVector<quint64> ColorNodeOps::vimSelected() const
{
    const quint64 id = vimCurrent();
    return id ? QVector<quint64> {id} : QVector<quint64> {};
}

void ColorNodeOps::vimSelect(quint64 id, bool)
{
    if (m_session)
        m_session->selectNode(id); // the grade has one selected node
}

void ColorNodeOps::vimReveal(quint64 id)
{
    if (!m_editor || !m_session)
        return;
    const GradeNode* node = m_session->grade().node(id);
    if (!node)
        return;
    const QRectF tile = m_editor->tileRect(*node);
    if (m_editor->rect().adjusted(8, 8, -8, -8).contains(tile.toRect()))
        return;
    const QPointF centre = m_editor->toEditor(tile.center());
    const QPointF half
        = m_editor->toEditor(QPointF(m_editor->width() / 2.0, m_editor->height() / 2.0))
        - m_editor->toEditor(QPointF(0, 0));
    m_editor->setView(centre - half, m_editor->zoom());
}

bool ColorNodeOps::vimConnect(quint64 from, quint64 to)
{
    if (!m_session)
        return false;
    const GradeNode* target = m_session->grade().node(to);
    if (!target)
        return false;
    // A corrector has one input (replaced); a mixer gets one more.
    const int index = target->isMixer() ? int(target->inputs.size()) : 0;
    return m_session->connectNodes(from, to, index);
}

bool ColorNodeOps::vimDisconnect(quint64 to, bool all)
{
    if (!m_session)
        return false;
    const GradeNode* target = m_session->grade().node(to);
    if (!target)
        return false;
    bool any = false;
    // The last input first, so the indexes below stay right.
    for (int i = int(target->inputs.size()) - 1; i >= 0; --i) {
        if (target->inputs[i] == 0 && !target->isMixer())
            continue; // already fed by the source picture
        any = m_session->disconnectInput(to, i) || any;
        if (!all)
            break;
    }
    return any;
}

bool ColorNodeOps::trigger(const char* actionId) const
{
    const QString id = QLatin1String(actionId);
    if (!m_actions || !m_actions->contains(id) || !m_actions->action(id)->isEnabled())
        return false;
    m_actions->action(id)->trigger();
    return true;
}

bool ColorNodeOps::vimCommand(const QString& verb)
{
    if (!m_session || !m_editor)
        return false;
    if (verb == QLatin1String("delete"))
        return m_session->removeNode();
    if (verb == QLatin1String("bypass"))
        return m_session->toggleNodeBypass();
    if (verb == QLatin1String("fit")) {
        m_editor->fitToView();
        return true;
    }
    if (verb == QLatin1String("add") || verb == QLatin1String("add_after"))
        return m_session->addSerialNode() != 0;
    if (verb == QLatin1String("add_before"))
        return m_session->addSerialNodeBefore() != 0;
    if (verb == QLatin1String("add_parallel"))
        return m_session->addParallelNode() != 0;
    if (verb == QLatin1String("add_layer"))
        return m_session->addLayerNode() != 0;
    if (verb == QLatin1String("wipe"))
        return trigger("color.wipe");
    if (verb == QLatin1String("copy"))
        return trigger("color.copy_grade");
    if (verb == QLatin1String("paste"))
        return trigger("color.paste_grade");
    return false; // rename: nodes are numbered
}

} // namespace kk
