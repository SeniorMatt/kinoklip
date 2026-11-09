// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/TrackStore.h"

#include "core/NodeRegistry.h"

namespace kk::tracking {

bool storeTrack(CompNode& node, const std::vector<TrackSample>& samples, int64_t referenceFrame)
{
    if (samples.empty())
        return false;
    AnimatedValue& x = node.params.animated(nodekeys::CenterX, 0.5);
    AnimatedValue& y = node.params.animated(nodekeys::CenterY, 0.5);
    if (!x.isAnimated() && !y.isAnimated())
        node.params.setBase(nodekeys::RefFrame, double(referenceFrame));
    for (const TrackSample& s : samples) {
        x.setKey(s.frame, s.u, Interp::Linear);
        y.setKey(s.frame, s.v, Interp::Linear);
    }
    return true;
}

void clearTrack(CompNode& node, int64_t frame)
{
    const double u = node.params.value(nodekeys::CenterX, frame, 0.5);
    const double v = node.params.value(nodekeys::CenterY, frame, 0.5);
    node.params.animated(nodekeys::CenterX, 0.5) = AnimatedValue(u);
    node.params.animated(nodekeys::CenterY, 0.5) = AnimatedValue(v);
}

} // namespace kk::tracking
