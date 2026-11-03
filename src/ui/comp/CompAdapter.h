// SPDX-License-Identifier: GPL-3.0-or-later
// CompAdapter — the Compositing page UI's only door to the composition core: the node
// catalogue, the pure graph operations and the undoable "replace a clip's composition" call.
// Everything in src/ui/comp and the page goes through these functions, so switching from a
// stand-in to core/NodeRegistry + core/CompOps touches this file pair only.
#pragma once

#include "core/Composition.h"
#include "core/EffectRegistry.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace kk {

class Project;

namespace compadapter {

/// What flows through a port.
enum class PortKind {
    Image,
    Mask
};

/// Colour role of a link in the node editor (by the input port it ends in).
enum class LinkRole {
    Background, ///< main image input (background of Merge, input of everything else)
    Foreground, ///< Merge / Dissolve second image
    Mask ///< mask / effect mask inputs
};

/// One input port of a node type.
struct PortInfo
{
    QString id; ///< key in CompNode::inputs ("background", "foreground", "mask", "input")
    QString label; ///< translated tooltip label
    PortKind kind = PortKind::Image;
    LinkRole role = LinkRole::Background;
    bool required = false;
};

/// One node type of the catalogue (the UI's view of a NodeRegistry descriptor).
struct NodeTypeInfo
{
    QString type; ///< CompNode::type ("merge", "fx:gaussian_blur", ...)
    QString name; ///< translated display name ("Merge")
    QString category; ///< translated library folder ("Composite", "Effects", ...)
    QString baseName; ///< prefix of new node names ("Merge" -> Merge1, Merge2)
    QVector<PortInfo> inputs;
    int outputCount = 1; ///< 0 for MediaOut
    QVector<ParamDescriptor> params;
    bool singleton = false; ///< only one per composition (MediaOut)

    /// Index of input port @p id, or -1.
    [[nodiscard]] int inputIndex(const QString& id) const;
    /// The main image input (where "add after the selected node" connects), or empty.
    [[nodiscard]] QString mainInput() const;
};

// ---- Catalogue ----

/// Every node type, in library order (categories grouped).
[[nodiscard]] const QVector<NodeTypeInfo>& nodeTypes();
/// Node type @p type, or nullptr.
[[nodiscard]] const NodeTypeInfo* nodeType(const QString& type);
/// Library folders in display order.
[[nodiscard]] QStringList categories();
/// The node types of the node editor's tool bar, in order (Background, Text, Merge, ...).
[[nodiscard]] QStringList toolbarTypes();

/// The on-viewer controls a node type has (CompViewerOverlay).
enum class ViewerControl {
    None,
    Transform, ///< centre, size, angle of the picture (Transform; Merge's foreground)
    Rectangle, ///< centre, width, height, angle of a mask
    Ellipse, ///< same keys as Rectangle, drawn as an ellipse
    Polygon ///< points of a polygon mask
};

/// Parameter keys behind the on-viewer controls of a node type. Positions are normalised to
/// the frame (0..1, y down); sizes relative to the frame; angles in degrees.
struct ViewerControlKeys
{
    ViewerControl kind = ViewerControl::None;
    QString centerX, centerY, size, width, height, angle;
    QString points; ///< Polygon: static "x,y x,y ..." (normalised)
};

/// The on-viewer controls of node type @p type.
[[nodiscard]] ViewerControlKeys viewerControls(const QString& type);

// ---- Pure graph operations (no project, no undo) ----

/// A new composition: MediaIn1 -> MediaOut1.
[[nodiscard]] Composition defaultComposition();
/// Adds a node of @p type at @p position (grid units) with the descriptor defaults and a
/// unique name. Returns its id, or 0 for an unknown type or a second singleton.
quint64 addNode(Composition& comp, const QString& type, QPointF position);
/// Adds a node of @p type "after" node @p afterId: its main input takes @p afterId's output
/// and the inputs that read @p afterId now read the new node. @p afterId 0: unconnected.
/// The node is placed right of @p afterId. Returns the new id or 0.
quint64 addNodeAfter(Composition& comp, const QString& type, quint64 afterId);
/// Connects output @p outputIndex of @p fromId to input @p port of @p toId. Refused (false)
/// for unknown nodes / ports, a self link or a link that would create a cycle.
bool connectNodes(
    Composition& comp, quint64 fromId, int outputIndex, quint64 toId, const QString& port);
/// Disconnects input @p port of @p toId. Returns false if it was not connected.
bool disconnectInput(Composition& comp, quint64 toId, const QString& port);
/// Inserts node @p nodeId (unconnected main input and output) into the link ending in input
/// @p port of @p toId. Returns false if not possible.
bool insertIntoLink(Composition& comp, quint64 nodeId, quint64 toId, const QString& port);
/// Removes nodes @p ids (MediaOut stays) and notes @p noteIds; links to removed nodes go away.
/// Returns true if anything was removed.
bool removeItems(Composition& comp, const QSet<quint64>& ids, const QSet<quint64>& noteIds = {});
/// Renames node @p id to @p name (made unique). Returns false for an empty name or no change.
bool renameNode(Composition& comp, quint64 id, const QString& name);
/// A self-contained copy of nodes @p ids (links between them kept, others dropped).
[[nodiscard]] Composition extract(const Composition& comp, const QSet<quint64>& ids);
/// Pastes @p fragment (from extract()) into @p comp moved by @p offset (grid units): new ids,
/// unique names, no MediaOut. Returns the new node ids.
QVector<quint64> paste(Composition& comp, const Composition& fragment, QPointF offset);
/// Swaps the background and foreground inputs of a Merge / Dissolve node.
bool swapMainInputs(Composition& comp, quint64 id);
/// True if node @p id's output reaches MediaOut (it contributes to the result).
[[nodiscard]] bool reachesOutput(const Composition& comp, quint64 id);
/// A unique name for a new node with base @p base ("Merge" -> "Merge3").
[[nodiscard]] QString uniqueName(const Composition& comp, const QString& base);

// ---- Project access ----

/// The composition of clip @p clipId of the active timeline, or std::nullopt (none yet).
[[nodiscard]] std::optional<Composition> clipComposition(const Project& project, quint64 clipId);
/// True if clip @p clipId of the active timeline has a composition.
[[nodiscard]] bool hasComposition(const Project& project, quint64 clipId);
/// Replaces the composition of clip @p clipId of the active timeline (std::nullopt removes it)
/// as one undo step labelled @p label. Returns false if the clip does not exist or nothing
/// changed (no step then).
bool setClipComposition(Project& project, quint64 clipId, const std::optional<Composition>& comp,
    const QString& label);
/// Removes the compositions of clips @p clipIds (those that have one) as one undo step
/// "Reset Composition". Returns the number of clips changed.
int resetCompositions(Project& project, const QVector<quint64>& clipIds);
/// Opens a gesture (a slider drag): the modifyClipComposition() calls until endCompEdit()
/// form one undo step labelled @p label. Nesting-safe like Project::beginEdit().
void beginCompEdit(Project& project, const QString& label);
/// Closes the gesture opened by beginCompEdit() (pushes the step if anything changed).
void endCompEdit(Project& project);
/// Live change inside an open gesture, notifying the views. Same return as
/// setClipComposition().
bool modifyClipComposition(Project& project, quint64 clipId, const Composition& comp);
// Every change above ends in Timeline::changed() of the active timeline: views refresh from
// that signal (and from Project::projectReset / undo, which emit it too).

} // namespace compadapter

} // namespace kk
