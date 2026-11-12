// SPDX-License-Identifier: GPL-3.0-or-later
// VimNodeOps — what a node graph (the Compositing page's NodeEditor, the Color page's
// ColorNodeEditor) lets the Vim layer do from the keyboard (spec section 83, 5.8). The editor
// widget has the property kkVimContext = "nodes" and the property kNodesProperty pointing at its
// VimNodeOps; VimNodeHost (VimNodeHost.h) turns h j k l w b and the other keys into these calls.
#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

#include <utility>

namespace kk {

/// One node as the keyboard sees it: its id, its tile in widget coordinates and its name.
struct VimNodeInfo
{
    quint64 id = 0;
    QRectF rect;
    QString name;
};

class VimNodeOps
{
public:
    virtual ~VimNodeOps() = default;

    /// Property (QObject*) of the editor widget that points at its VimNodeOps object.
    static constexpr char kNodesProperty[] = "kkVimNodes";

    /// Every node (not notes) with its tile.
    [[nodiscard]] virtual QVector<VimNodeInfo> vimNodes() const = 0;
    /// The links as (source node, target node) pairs, in signal flow direction.
    [[nodiscard]] virtual QVector<std::pair<quint64, quint64>> vimLinks() const { return {}; }
    /// The current node (0: none) and the selected nodes.
    [[nodiscard]] virtual quint64 vimCurrent() const = 0;
    [[nodiscard]] virtual QVector<quint64> vimSelected() const = 0;
    /// Selects only @p id and makes it current; with @p toggle adds it to / removes it from the
    /// selection instead.
    virtual void vimSelect(quint64 id, bool toggle) = 0;
    virtual void vimSelectAll() { }
    /// Brings the tile of @p id into view.
    virtual void vimReveal(quint64 id) { Q_UNUSED(id); }
    /// Connects the output of @p from to a free (else the main) input of @p to.
    virtual bool vimConnect(quint64 from, quint64 to)
    {
        Q_UNUSED(from);
        Q_UNUSED(to);
        return false;
    }
    /// Removes the main input of @p to (@p all: every input).
    virtual bool vimDisconnect(quint64 to, bool all)
    {
        Q_UNUSED(to);
        Q_UNUSED(all);
        return false;
    }
    /// A named command of the editor: "delete", "copy", "paste", "rename", "bypass", "wipe",
    /// "fit", "add", "add_after", "add_before", "add_parallel", "add_layer". False when the
    /// editor does not have it.
    virtual bool vimCommand(const QString& verb)
    {
        Q_UNUSED(verb);
        return false;
    }
};

} // namespace kk
