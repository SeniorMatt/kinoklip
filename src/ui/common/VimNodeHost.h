// SPDX-License-Identifier: GPL-3.0-or-later
// VimNodeHost — the keys of context "nodes" (spec section 83, 5.8): select nodes with h j k l by
// where they are on the canvas (Natron and Nuke's node graphs are navigated by position too),
// w and b along the signal flow, add, delete, copy, bypass, rename, and connect with `gc` twice
// (source, then target). The cursor is the editor's own current node, so nothing is drawn.
#pragma once

#include "ui/common/VimNodeOps.h"
#include "ui/common/VimPanelHost.h"

#include <QPointer>

class QWidget;

namespace kk {

class VimNodeHost final : public VimPanelHost
{
public:
    explicit VimNodeHost(ActionRegistry* actions);

    void focusChanged(QWidget* focus) override;
    bool hostCommand(const QString& name, int count, bool hasCount) override;
    /// The node the `gc` connect gesture started from (0 when none).
    [[nodiscard]] quint64 connectSource() const { return m_connectFrom; }

    enum class Dir {
        Left,
        Down,
        Up,
        Right
    };
    /// The node nearest to @p from in direction @p dir by where the tiles are, or 0.
    [[nodiscard]] static quint64 neighbour(
        const QVector<VimNodeInfo>& nodes, quint64 from, Dir dir);
    /// The nodes in signal order: sources first, then by the longest path from a source, then
    /// top to bottom.
    [[nodiscard]] static QVector<quint64> signalOrder(
        const QVector<VimNodeInfo>& nodes, const QVector<std::pair<quint64, quint64>>& links);

private:
    bool go(VimNodeOps& ops, Dir dir, int count);
    bool order(VimNodeOps& ops, int steps);

    QPointer<QWidget> m_editor;
    VimNodeOps* m_ops = nullptr;
    quint64 m_connectFrom = 0;
};

} // namespace kk
