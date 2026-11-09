// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/TrackLink.h"

#include "compositing/tracking/Homography.h"
#include "core/NodeRegistry.h"

#include <algorithm>
#include <set>

namespace kk::tracking {

bool isTrackSource(const CompNode& node)
{
    return node.type == nodetypes::Tracker || node.type == nodetypes::PlanarTracker;
}

std::vector<int64_t> trackFrames(const CompNode& source)
{
    std::set<int64_t> frames;
    auto addKeys = [&](const QString& key) {
        const auto it = source.params.values.constFind(key);
        if (it == source.params.values.constEnd())
            return;
        for (const Keyframe& k : it->keys)
            frames.insert(k.frame);
    };
    if (source.type == nodetypes::Tracker) {
        addKeys(nodekeys::CenterX);
        addKeys(nodekeys::CenterY);
    } else if (source.type == nodetypes::PlanarTracker) {
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            addKeys(nodekeys::planarCorner(i, false));
            addKeys(nodekeys::planarCorner(i, true));
        }
    }
    return {frames.begin(), frames.end()};
}

std::vector<quint64> trackSources(const Composition& comp)
{
    std::vector<quint64> ids;
    for (const CompNode& n : comp.nodes) {
        if (isTrackSource(n) && !trackFrames(n).empty())
            ids.push_back(n.id);
    }
    return ids;
}

bool canFollowTrack(const CompNode& node)
{
    if (isTrackSource(node))
        return false;
    const NodeDescriptor* d = NodeRegistry::instance().find(node.type);
    return d && d->param(nodekeys::CenterX) && d->param(nodekeys::CenterY);
}

QPointF trackCentre(const CompNode& source, int64_t frame)
{
    const ParamSet& p = source.params;
    if (source.type == nodetypes::PlanarTracker) {
        // Normalised y is up; the diagonal crossing does not care about the orientation.
        Quad q;
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            q[size_t(i)] = {p.value(nodekeys::planarCorner(i, false), frame, 0.5),
                p.value(nodekeys::planarCorner(i, true), frame, 0.5)};
        }
        const Vec2 c = quadCentre(q);
        return {c.x, c.y};
    }
    return {p.value(nodekeys::CenterX, frame, 0.5), p.value(nodekeys::CenterY, frame, 0.5)};
}

bool linkToTrack(CompNode& target, const CompNode& source, int64_t offsetFrame)
{
    if (target.locked || source.locked || !canFollowTrack(target) || !isTrackSource(source))
        return false;
    const std::vector<int64_t> frames = trackFrames(source);
    if (frames.empty())
        return false;
    const QPointF here = trackCentre(source, offsetFrame);
    const double offsetX = target.params.value(nodekeys::CenterX, offsetFrame, 0.5) - here.x();
    const double offsetY = target.params.value(nodekeys::CenterY, offsetFrame, 0.5) - here.y();
    AnimatedValue x, y;
    for (const int64_t f : frames) {
        const QPointF c = trackCentre(source, f);
        x.setKey(f, c.x() + offsetX, Interp::Linear);
        y.setKey(f, c.y() + offsetY, Interp::Linear);
    }
    target.params.animated(nodekeys::CenterX, 0.5) = x;
    target.params.animated(nodekeys::CenterY, 0.5) = y;
    return true;
}

} // namespace kk::tracking
