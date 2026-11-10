// SPDX-License-Identifier: GPL-3.0-or-later
// TimeEffects implementation.
#include "render/TimeEffects.h"

#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::render::timefx {

namespace k = fxkeys;

namespace {

constexpr const char* kReaders[] = {fxid::Echo, fxid::Strobe, fxid::MotionTrails, fxid::Smear,
    fxid::FrameReplacer, fxid::TemporalMotionBlur, fxid::Deflicker, fxid::ColorStabilizer,
    fxid::DirtRemoval, fxid::DustBuster};

int intParam(const ParamSet& p, const QString& key, int64_t frame, int fallback, int lo, int hi)
{
    return std::clamp(int(std::lround(p.value(key, frame, fallback))), lo, hi);
}

} // namespace

bool isTimeEffect(const QString& type)
{
    return std::any_of(std::begin(kReaders), std::end(kReaders),
        [&type](const char* id) { return type == QLatin1String(id); });
}

void framesRead(const QString& type, const ParamSet& p, int64_t f, std::vector<int64_t>* out)
{
    if (type == QLatin1String(fxid::Echo)) {
        echoFrames(p, f, out);
    } else if (type == QLatin1String(fxid::MotionTrails)) {
        if (p.value(k::Opacity, f, 100) <= 0.0)
            return;
        const int n = intParam(p, k::Trails, f, 4, 1, 5);
        const int spacing = intParam(p, k::Delay, f, 1, 1, 4);
        for (int i = 1; i <= n; ++i)
            out->push_back(f - int64_t(i) * spacing);
    } else if (type == QLatin1String(fxid::Smear)) {
        if (p.value(k::Amount, f, 100) <= 0.0)
            return;
        const int n = intParam(p, k::Length, f, 2, 1, 2);
        const QString dir = p.statics.value(k::SmearDirection, fxchoice::Both).toString();
        for (int i = 1; i <= n; ++i) {
            if (dir != fxchoice::Ahead)
                out->push_back(f - i);
            if (dir != fxchoice::Behind)
                out->push_back(f + i);
        }
    } else if (type == QLatin1String(fxid::FrameReplacer)) {
        if (p.value(k::Amount, f, 100) <= 0.0)
            return;
        const QString source = p.statics.value(k::FrameSource, fxchoice::Previous).toString();
        if (source != fxchoice::Next)
            out->push_back(f - 1);
        if (source != fxchoice::Previous)
            out->push_back(f + 1);
    } else if (type == QLatin1String(fxid::TemporalMotionBlur)) {
        if (p.value(k::ShutterAngle, f, 180) <= 0.0)
            return;
        out->push_back(f - 1);
        out->push_back(f + 1);
    } else if (type == QLatin1String(fxid::Deflicker)) {
        if (p.value(k::Strength, f, 100) <= 0.0)
            return;
        const int w = intParam(p, k::FrameWindow, f, 2, 1, 2);
        for (int i = 1; i <= w; ++i) {
            out->push_back(f - i);
            out->push_back(f + i);
        }
    } else if (type == QLatin1String(fxid::DirtRemoval)
        || type == QLatin1String(fxid::DustBuster)) {
        if (p.value(k::Amount, f, 100) <= 0.0)
            return;
        const int w = intParam(p, k::FrameWindow, f, 1, 1, 2);
        for (int i = 1; i <= w; ++i) {
            out->push_back(f - i);
            out->push_back(f + i);
        }
    } else if (type == QLatin1String(fxid::ColorStabilizer)) {
        if (p.value(k::Strength, f, 100) <= 0.0)
            return;
        const int64_t ref = std::max<int64_t>(0, std::llround(p.value(k::ReferenceFrame, f, 0)));
        if (ref != f)
            out->push_back(ref);
    }
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
                framesRead(it->type, it->params, f, &next);
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
