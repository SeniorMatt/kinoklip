// SPDX-License-Identifier: GPL-3.0-or-later
// Composition — the node graph of the Compositing page (docs/compositing-page.md).
// Value types only; the node catalogue is in core/NodeRegistry.h, evaluation in src/render.
#pragma once

#include "core/Animation.h"
#include "core/Mask.h"

#include <QColor>
#include <QMap>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace kk {

/// Where an input of a node takes its image from: output @p outputIndex of node @p nodeId.
struct NodeLink
{
    quint64 nodeId = 0; ///< 0 = not connected
    int outputIndex = 0;

    [[nodiscard]] bool isConnected() const { return nodeId != 0; }
    friend bool operator==(const NodeLink&, const NodeLink&) = default;
};

/// One node of a composition. `type` is a NodeRegistry descriptor id ("media_in", "merge",
/// "transform", or "fx:<EffectRegistry id>" for every Edit page video effect used as a node).
struct CompNode
{
    quint64 id = 0; ///< unique within the composition (Composition::allocateId())
    QString type;
    QString name; ///< shown on the tile, unique within the composition ("Merge1", "Blur2")
    QPointF position; ///< node editor position in grid units (1 unit = one tile height)
    bool enabled = true; ///< false = pass-through: the main input goes straight to the output
    bool locked = false; ///< parameters cannot be edited
    QColor color; ///< tile colour set by the user (Set Color); invalid = the default look
    bool thumbnail = false; ///< show the node's picture on its tile (node editor)
    ParamSet params; ///< keys from the descriptor's parameters; keyframes in comp frames
    /// Inputs by port id from the descriptor ("background", "foreground", "mask", "input").
    /// Ports without an entry are unconnected.
    QMap<QString, NodeLink> inputs;
    /// MediaIn only: MediaItem::id of the picture it reads; empty = the clip that owns the
    /// composition (its source at the clip's time, with the clip's speed and retime).
    QString mediaId;
    /// Polygon mask node only: the closed point path (core/Mask.h AnimatedPath, shape
    /// keyframes in comp frames). Points are mask-local frame pixels (timeline pixels, origin
    /// at the node's centre, y down), like MaskData::path.
    AnimatedPath path;

    friend bool operator==(const CompNode&, const CompNode&) = default;
};

/// A note or group frame in the node editor (no effect on the picture).
struct CompNote
{
    quint64 id = 0;
    QPointF position;
    QSizeF size;
    QString text;
    QColor color;

    friend bool operator==(const CompNote&, const CompNote&) = default;
};

/// The node graph of one clip. A clip with a composition shows the composition's output
/// (the image arriving at its MediaOut node) instead of its source picture; the Edit page
/// effects, masks, transform, opacity and compositing of the clip then apply on top of it.
/// Frames are clip-local ("comp frames": 0 = the clip's first frame), like every keyframe.
struct Composition
{
    QVector<CompNode> nodes;
    QVector<CompNote> notes;
    quint64 mediaOutId = 0; ///< the node whose image is the result (type "media_out")
    quint64 nextId = 1; ///< next id handed out by allocateId()
    /// Node editor view state (saved so reopening shows the same place).
    QPointF viewCentre;
    double viewZoom = 1.0;
    quint64 leftViewerNode = 0; ///< node shown in the left viewer (0 = none)
    quint64 rightViewerNode = 0; ///< node shown in the right viewer (0 = MediaOut)

    /// A new id, unique within this composition.
    quint64 allocateId() { return nextId++; }
    /// Node with id @p id, or nullptr.
    [[nodiscard]] const CompNode* node(quint64 id) const
    {
        for (const CompNode& n : nodes) {
            if (n.id == id) {
                return &n;
            }
        }
        return nullptr;
    }
    /// Mutable node with id @p id, or nullptr.
    [[nodiscard]] CompNode* node(quint64 id)
    {
        for (CompNode& n : nodes) {
            if (n.id == id) {
                return &n;
            }
        }
        return nullptr;
    }

    friend bool operator==(const Composition&, const Composition&) = default;
};

} // namespace kk
