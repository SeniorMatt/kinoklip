// SPDX-License-Identifier: GPL-3.0-or-later
// MixLevels — merging the readings of several mix calls.
#include "render/MixLevels.h"

#include <algorithm>

namespace kk {

namespace {
template<int N>
void mergeList(LevelList<N>& into, const LevelList<N>& from)
{
    for (int i = 0; i < from.count; ++i) {
        const ChannelLevel& f = from.items[size_t(i)];
        ChannelLevel* e = nullptr;
        for (int k = 0; k < into.count; ++k) {
            if (into.items[size_t(k)].id == f.id)
                e = &into.items[size_t(k)];
        }
        if (!e) {
            if (ChannelLevel* added = into.add())
                *added = f;
            continue;
        }
        e->peak[0] = std::max(e->peak[0], f.peak[0]);
        e->peak[1] = std::max(e->peak[1], f.peak[1]);
        e->gainReductionDb = f.gainReductionDb; // the later reading
    }
}
} // namespace

void MixLevels::merge(const MixLevels& other)
{
    mergeList(tracks, other.tracks);
    mergeList(buses, other.buses);
    output[0] = std::max(output[0], other.output[0]);
    output[1] = std::max(output[1], other.output[1]);
}

} // namespace kk
