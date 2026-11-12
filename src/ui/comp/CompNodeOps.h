// SPDX-License-Identifier: GPL-3.0-or-later
// CompNodeOps — the Compositing page's NodeEditor for the Vim layer (VimNodeOps.h).
#pragma once

#include "ui/common/VimNodeOps.h"

#include <QObject>
#include <QPointer>

namespace kk {

class ActionRegistry;
class CompSession;
class NodeEditor;

class CompNodeOps final : public QObject, public VimNodeOps
{
    Q_OBJECT
public:
    /// Installs itself on @p editor (property kNodesProperty and the key context "nodes").
    CompNodeOps(NodeEditor* editor, CompSession* session, ActionRegistry* actions);

    [[nodiscard]] QVector<VimNodeInfo> vimNodes() const override;
    [[nodiscard]] QVector<std::pair<quint64, quint64>> vimLinks() const override;
    [[nodiscard]] quint64 vimCurrent() const override;
    [[nodiscard]] QVector<quint64> vimSelected() const override;
    void vimSelect(quint64 id, bool toggle) override;
    void vimSelectAll() override;
    void vimReveal(quint64 id) override;
    bool vimConnect(quint64 from, quint64 to) override;
    bool vimDisconnect(quint64 to, bool all) override;
    bool vimCommand(const QString& verb) override;

private:
    QPointer<NodeEditor> m_editor;
    QPointer<CompSession> m_session;
    ActionRegistry* m_actions;
};

} // namespace kk
