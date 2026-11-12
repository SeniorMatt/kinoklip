// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimNodeHost.h"

#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>

namespace kk {

VimNodeHost::VimNodeHost(ActionRegistry* actions)
    : VimPanelHost(actions)
{ }

void VimNodeHost::focusChanged(QWidget* focus)
{
    for (QWidget* w = focus; w; w = w->parentWidget()) {
        auto* object = w->property(VimNodeOps::kNodesProperty).value<QObject*>();
        if (auto* ops = dynamic_cast<VimNodeOps*>(object)) {
            m_editor = w;
            m_ops = ops;
            return;
        }
    }
}

quint64 VimNodeHost::neighbour(const QVector<VimNodeInfo>& nodes, quint64 from, Dir dir)
{
    const VimNodeInfo* origin = nullptr;
    for (const VimNodeInfo& n : nodes) {
        if (n.id == from)
            origin = &n;
    }
    if (!origin)
        return nodes.isEmpty() ? 0 : nodes.first().id;
    const QPointF c = origin->rect.center();
    quint64 best = 0;
    double bestScore = 1e18;
    // Nodes inside the 60 degree cone round the direction first; if there are none, any node
    // that lies that way.
    for (const bool cone : {true, false}) {
        for (const VimNodeInfo& n : nodes) {
            if (n.id == from)
                continue;
            const QPointF d = n.rect.center() - c;
            const bool horizontal = dir == Dir::Left || dir == Dir::Right;
            const double along = horizontal ? (dir == Dir::Right ? d.x() : -d.x())
                                            : (dir == Dir::Down ? d.y() : -d.y());
            const double across = std::abs(horizontal ? d.y() : d.x());
            if (along <= 1.0)
                continue;
            if (cone && across > along * 1.7)
                continue;
            const double score = along + 2.0 * across;
            if (score < bestScore) {
                bestScore = score;
                best = n.id;
            }
        }
        if (best)
            return best;
    }
    return 0;
}

QVector<quint64> VimNodeHost::signalOrder(
    const QVector<VimNodeInfo>& nodes, const QVector<std::pair<quint64, quint64>>& links)
{
    // Depth = the longest path from a node without inputs (cycles are cut at 64 steps).
    std::map<quint64, int> depth;
    for (const VimNodeInfo& n : nodes)
        depth[n.id] = 0;
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false;
        for (const auto& [from, to] : links) {
            if (!depth.count(from) || !depth.count(to))
                continue;
            if (depth[to] < depth[from] + 1) {
                depth[to] = depth[from] + 1;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    QVector<VimNodeInfo> sorted = nodes;
    std::stable_sort(
        sorted.begin(), sorted.end(), [&depth](const VimNodeInfo& a, const VimNodeInfo& b) {
            if (depth[a.id] != depth[b.id])
                return depth[a.id] < depth[b.id];
            return a.rect.center().y() < b.rect.center().y();
        });
    QVector<quint64> ids;
    for (const VimNodeInfo& n : std::as_const(sorted))
        ids.append(n.id);
    return ids;
}

bool VimNodeHost::go(VimNodeOps& ops, Dir dir, int count)
{
    const QVector<VimNodeInfo> nodes = ops.vimNodes();
    quint64 at = ops.vimCurrent();
    bool moved = false;
    for (int i = 0; i < count; ++i) {
        const quint64 next = neighbour(nodes, at, dir);
        if (!next || next == at)
            break;
        at = next;
        moved = true;
    }
    if (!moved && ops.vimCurrent() == 0 && at) {
        moved = true; // nothing was selected: the first node
    }
    if (moved) {
        ops.vimSelect(at, false);
        ops.vimReveal(at);
    }
    return moved;
}

bool VimNodeHost::order(VimNodeOps& ops, int steps)
{
    const QVector<quint64> ids = signalOrder(ops.vimNodes(), ops.vimLinks());
    if (ids.isEmpty())
        return false;
    const int at = int(ids.indexOf(ops.vimCurrent()));
    const int to
        = std::clamp(at < 0 ? (steps > 0 ? steps - 1 : 0) : at + steps, 0, int(ids.size()) - 1);
    if (to == at)
        return false;
    ops.vimSelect(ids[to], false);
    ops.vimReveal(ids[to]);
    return true;
}

bool VimNodeHost::hostCommand(const QString& name, int count, bool hasCount)
{
    if (!name.startsWith(QLatin1String("node_")))
        return false;
    if (!m_ops) {
        return true;
    }
    VimNodeOps& ops = *m_ops;
    const QString cmd = name.mid(5);
    const int n = std::max(1, count);
    bool ok = true;
    if (cmd == QLatin1String("left")) {
        ok = go(ops, Dir::Left, n);
    } else if (cmd == QLatin1String("right")) {
        ok = go(ops, Dir::Right, n);
    } else if (cmd == QLatin1String("up")) {
        ok = go(ops, Dir::Up, n);
    } else if (cmd == QLatin1String("down")) {
        ok = go(ops, Dir::Down, n);
    } else if (cmd == QLatin1String("next")) {
        ok = order(ops, n);
    } else if (cmd == QLatin1String("prev")) {
        ok = order(ops, -n);
    } else if (cmd == QLatin1String("first") || cmd == QLatin1String("last")) {
        QVector<quint64> ids = signalOrder(ops.vimNodes(), ops.vimLinks());
        if (hasCount && !ids.isEmpty()) {
            const quint64 id = ids[std::clamp(count - 1, 0, int(ids.size()) - 1)];
            ops.vimSelect(id, false);
            ops.vimReveal(id);
        } else if (ids.isEmpty()) {
            ok = false;
        } else {
            const quint64 id = cmd == QLatin1String("first") ? ids.first() : ids.last();
            ops.vimSelect(id, false);
            ops.vimReveal(id);
        }
    } else if (cmd == QLatin1String("select_toggle")) {
        ok = ops.vimCurrent() != 0;
        if (ok)
            ops.vimSelect(ops.vimCurrent(), true);
    } else if (cmd == QLatin1String("select_all")) {
        ops.vimSelectAll();
    } else if (cmd == QLatin1String("connect")) {
        const quint64 here = ops.vimCurrent();
        if (!here) {
            ok = false;
        } else if (!m_connectFrom) {
            m_connectFrom = here;
            showMessage(QObject::tr("Connect: move to the node to connect it to, then gc"));
        } else if (m_connectFrom == here) {
            m_connectFrom = 0;
            showMessage(QObject::tr("Connect cancelled"));
        } else {
            ok = ops.vimConnect(m_connectFrom, here);
            m_connectFrom = 0;
        }
    } else if (cmd == QLatin1String("disconnect") || cmd == QLatin1String("disconnect_all")) {
        ok = ops.vimCurrent()
            && ops.vimDisconnect(ops.vimCurrent(), cmd == QLatin1String("disconnect_all"));
    } else {
        // delete copy paste rename bypass wipe fit add add_after add_before add_parallel add_layer
        ok = ops.vimCommand(cmd);
    }
    if (!ok)
        showMessage(QObject::tr("Cannot do that here"));
    return true;
}

} // namespace kk
