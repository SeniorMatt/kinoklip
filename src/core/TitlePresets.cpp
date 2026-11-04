// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TitlePresets.h"

#include "core/EffectRegistry.h"
#include "core/TimelineData.h"

namespace kk::titles {

const EffectDescriptor* descriptorOf(const ClipData& clip)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    if (const EffectDescriptor* d = registry.find(clip.generator);
        d && d->category == EffectCategory::Title)
        return d;
    return registry.find(Text);
}

} // namespace kk::titles
