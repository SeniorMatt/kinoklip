// SPDX-License-Identifier: GPL-3.0-or-later
// CompOps — pure operations on a Composition (the Compositing page node graph): default
// graph, adding / removing / connecting nodes, insert into a link, copy / paste, viewers.
// Every UI action builds the new Composition with these and commits it as one undo step
// through Project::setClipComposition().
#pragma once

#include "core/Composition.h"
#include "core/NodeRegistry.h"

#include <QPointF>
#include <QString>
#include <QVector>

#include <optional>

namespace kk::compops {

/// A new composition: MediaIn1 (the clip's own picture) -> MediaOut1, both viewers unset
/// (the right one shows MediaOut). Rendering it changes nothing (isPassThrough()).
[[nodiscard]] Composition defaultComposition();

/// True if @p comp renders exactly the clip's own picture: following MediaOut's input
/// (through disabled nodes, which pass their main input through) arrives at an enabled
/// MediaIn of the owning clip (no media id). The renderer then skips the evaluation, so a
/// default composition costs nothing. Nodes that do not reach MediaOut do not matter.
[[nodiscard]] bool isPassThrough(const Composition& comp);

/// A free node name "<base><n>" (n = 1, 2, ...), e.g. "Merge1", "Merge2".
[[nodiscard]] QString uniqueName(const Composition& comp, const QString& base);

/// A new node of type @p type (NodeRegistry id) with default parameters at @p position, a
/// unique name and a new id; nothing connected. std::nullopt for an unknown type.
[[nodiscard]] std::optional<CompNode> makeNode(
    Composition& comp, const QString& type, QPointF position);

/// Adds a node of type @p type at @p position. Returns its id, 0 for an unknown type.
quint64 addNode(Composition& comp, const QString& type, QPointF position);

/// What addNodeAfter() created.
struct AddResult
{
    quint64 node = 0; ///< the new node (0 = unknown type)
    quint64 merge = 0; ///< the Merge created to combine a source node, or 0
};

/// The toolbar / Select Node action: adds a node of @p type "after" node @p selected.
/// * A node with an image input: inserted between @p selected and everything it feeds.
/// * A source (Background, Text, Fast Noise, MediaIn): a Merge is added after @p selected
///   (background = selected) with the new node as its foreground.
/// * A mask: connected to the selected node's free "mask" port (else unconnected).
/// Without a (valid) selection the node is placed unconnected right of the rightmost node.
/// MediaOut as @p selected adds before it (into its input link).
AddResult addNodeAfter(Composition& comp, const QString& type, quint64 selected);

/// Removes the nodes @p ids (unknown ids and the MediaOut node are skipped). The chain stays
/// connected: an input that read a removed node now reads that node's main input (if its
/// kind fits). Viewer references to removed nodes are cleared. Returns the number removed.
int removeNodes(Composition& comp, const QVector<quint64>& ids);

/// Why connect() refused.
enum class ConnectError {
    None,
    UnknownNode, ///< @p from or @p to does not exist (or has an unknown type)
    UnknownPort, ///< @p to has no such input / @p from no such output
    WrongKind, ///< a mask output into an image input
    Cycle ///< the link would make @p from depend on itself
};

/// True if an output of kind @p output may feed an input of kind @p input: same kind, or an
/// image into a mask input (its alpha is the matte); a mask never feeds an image input.
[[nodiscard]] bool kindsCompatible(PortKind output, PortKind input);

/// True if @p node depends (through its inputs, transitively) on node @p on (or is it).
[[nodiscard]] bool dependsOn(const Composition& comp, quint64 node, quint64 on);

/// Connects output @p output of @p from to input @p port of @p to (replacing what was
/// connected there). Refuses cycles and wrong kinds.
ConnectError connect(Composition& comp, quint64 from, int output, quint64 to, const QString& port);

/// Disconnects input @p port of @p to. False if nothing was connected.
bool disconnect(Composition& comp, quint64 to, const QString& port);

/// Drops node @p node onto the link arriving at input @p port of @p to: the link's source
/// feeds @p node's main input and @p node feeds @p port. Refused (false) when the link is
/// not connected, @p node has no main input, the kinds do not fit or it would make a cycle.
bool insertIntoLink(Composition& comp, quint64 node, quint64 to, const QString& port);

/// Moves the nodes @p ids by @p delta (grid units).
void moveNodes(Composition& comp, const QVector<quint64>& ids, QPointF delta);

/// Renames @p node. Refused (false) for an empty name or one another node has.
bool renameNode(Composition& comp, quint64 node, const QString& name);

/// Enables @p node, or disables it (pass-through of its main input). False if unknown.
bool setEnabled(Composition& comp, quint64 node, bool enabled);

/// Copies the nodes @p ids (and the links between them; links from other nodes are
/// dropped). The MediaOut node is never copied.
[[nodiscard]] QVector<CompNode> copyNodes(const Composition& comp, const QVector<quint64>& ids);

/// Pastes @p nodes (from copyNodes() or the clipboard JSON) into @p comp, moved by
/// @p offset: new ids, unique names (a taken name gets the next free number), links between
/// pasted nodes remapped, links to nodes outside the pasted set dropped, unknown types and
/// MediaOut nodes skipped. Returns the new ids in the order of @p nodes.
QVector<quint64> pasteNodes(Composition& comp, const QVector<CompNode>& nodes, QPointF offset);

/// Sets the node shown in the left / right viewer (0 = none / MediaOut for the right one);
/// unknown ids become 0.
void setViewerNodes(Composition& comp, quint64 left, quint64 right);

/// The nodes @p target depends on (and @p target itself), each after every node it reads:
/// the evaluation order of a pull from @p target. Disabled nodes still list their main
/// input only (that is all they read). Empty for an unknown @p target.
[[nodiscard]] QVector<quint64> evaluationOrder(const Composition& comp, quint64 target);

/// The input link that disabled node @p node passes through (its main input), or nullptr.
[[nodiscard]] const NodeLink* mainInputLink(const CompNode& node);

} // namespace kk::compops
