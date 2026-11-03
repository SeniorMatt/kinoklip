// SPDX-License-Identifier: GPL-3.0-or-later
// TimeEffects implementation.
#include "render/TimeEffects.h"

#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::render::timefx {

namespace k = fxkeys;

bool isTimeEffect(const QString& type)
{
    return type == QLatin1String(fxid::Echo) || type == QLatin1String(fxid::Strobe);
}

int64_t strobeFrame(const ParamSet& params, int64_t localFrame)
{
    const int64_t hold = std::max<int64_t>(1, std::llround(params.value(k::Hold, localFrame, 3)));
    if (localFrame <= 0)
        return localFrame;
    return localFrame - localFrame % hold;
}

void echoFrames(const ParamSet& params, int64_t localFrame, std::vector<int64_t>* out)
{
    const int echoes = std::clamp(int(std::lround(params.value(k::Echoes, localFrame, 3))), 1, 4);
    const int64_t delay = std::max<int64_t>(1, std::llround(params.value(k::Delay, localFrame, 2)));
    for (int i = 1; i <= echoes; ++i) {
        const int64_t f = localFrame - i * delay;
        if (f < 0)
            break;
        out->push_back(f);
    }
}

std::vector<int64_t> clipFrames(const ClipData& clip, int64_t localFrame)
{
    std::vector<int64_t> frames {localFrame};
    // Walk the effects from the last to the first: each time effect maps the frames the
    // effects after it need to the frames it reads itself.
    for (auto it = clip.effects.crbegin(); it != clip.effects.crend(); ++it) {
        if (!it->enabled || !isTimeEffect(it->type))
            continue;
        std::vector<int64_t> next;
        for (const int64_t f : frames) {
            if (it->type == QLatin1String(fxid::Strobe)) {
                next.push_back(strobeFrame(it->params, f));
            } else {
                next.push_back(f);
                echoFrames(it->params, f, &next);
            }
        }
        frames.clear();
        for (const int64_t f : next) {
            if (std::find(frames.begin(), frames.end(), f) == frames.end()
                && int(frames.size()) < kMaxClipFrames)
                frames.push_back(f);
        }
    }
    // The output frame stays first (it is drawn anyway), the rest follow.
    auto self = std::find(frames.begin(), frames.end(), localFrame);
    if (self == frames.end())
        frames.insert(frames.begin(), localFrame);
    else
        std::rotate(frames.begin(), self, self + 1);
    return frames;
}

} // namespace kk::render::timefx
