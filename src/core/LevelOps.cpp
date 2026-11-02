// SPDX-License-Identifier: GPL-3.0-or-later
// LevelOps — volume line and fade handle edits (see LevelOps.h).
#include "core/LevelOps.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::levelops {

namespace {

double clampDb(double db)
{
    return std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
}

} // namespace

QString volumePath()
{
    return keyframeops::memberPath(QStringLiteral("audio"), keys::VolumeDb);
}

double volumeDbAt(const ClipData& clip, double localFrame)
{
    const auto it = clip.audio.values.constFind(keys::VolumeDb);
    if (it == clip.audio.values.cend())
        return keys::defaultValue(keys::VolumeDb);
    return it->valueAtTime(localFrame);
}

bool offsetVolume(TimelineData& data, quint64 clipId, int64_t localFrame, double deltaDb)
{
    ClipData* clip = editops::clipById(data, clipId);
    if (!clip || deltaDb == 0.0)
        return false;
    AnimatedValue& v = clip->audio.animated(keys::VolumeDb, keys::defaultValue(keys::VolumeDb));
    if (v.keys.isEmpty()) {
        const double before = v.base;
        v.base = clampDb(v.base + deltaDb);
        return v.base != before;
    }
    // The keys that shape the line at localFrame: [first, last] index range.
    qsizetype first = 0;
    qsizetype last = 0;
    const qsizetype n = v.keys.size();
    if (localFrame <= v.keys.front().frame) {
        first = last = 0;
    } else if (localFrame >= v.keys.back().frame) {
        first = last = n - 1;
    } else {
        const auto it = std::upper_bound(v.keys.cbegin(), v.keys.cend(), localFrame,
            [](int64_t f, const Keyframe& k) { return f < k.frame; });
        last = qsizetype(it - v.keys.cbegin()); // first key after localFrame
        first = last - 1;
        if (v.keys[first].frame == localFrame)
            last = first; // exactly on a key: only that key
    }
    bool changed = false;
    for (qsizetype i = first; i <= last; ++i) {
        const double before = v.keys[i].value;
        v.keys[i].value = clampDb(before + deltaDb);
        changed |= v.keys[i].value != before;
    }
    return changed;
}

std::optional<KeyframeRef> addVolumeKey(TimelineData& data, quint64 clipId, int64_t localFrame)
{
    ClipData* clip = editops::clipById(data, clipId);
    if (!clip || clip->duration <= 0)
        return std::nullopt;
    localFrame = std::clamp<int64_t>(localFrame, 0, clip->duration - 1);
    const double db = volumeDbAt(*clip, double(localFrame));
    clip->audio.animated(keys::VolumeDb, keys::defaultValue(keys::VolumeDb)).setKey(localFrame, db);
    return KeyframeRef {clipId, volumePath(), localFrame};
}

std::optional<KeyframeRef> setVolumeKey(
    TimelineData& data, const KeyframeRef& ref, int64_t newFrame, double valueDb)
{
    const ClipData* clip = editops::clipById(data, ref.clipId);
    if (!clip)
        return std::nullopt;
    const AnimatedValue* v = keyframeops::findValue(*clip, ref.paramPath);
    if (!v || !v->hasKeyAt(ref.frame))
        return std::nullopt;
    KeyframeRef at = ref;
    const int64_t delta = keyframeops::clampMoveDelta(data, {ref}, newFrame - ref.frame);
    if (delta != 0) {
        const QVector<KeyframeRef> moved = keyframeops::moveKeys(data, {ref}, delta);
        if (moved.isEmpty())
            return std::nullopt;
        at = moved.front();
    }
    ClipData* c = editops::clipById(data, ref.clipId);
    AnimatedValue* mv = keyframeops::findValue(*c, at.paramPath);
    const qsizetype i = mv ? mv->indexOf(at.frame) : -1;
    if (i < 0)
        return std::nullopt;
    mv->keys[i].value = clampDb(valueDb);
    return at;
}

bool setFade(TimelineData& data, quint64 clipId, bool head, int64_t frames)
{
    ClipData* clip = editops::clipById(data, clipId);
    if (!clip)
        return false;
    const int64_t other = head ? clip->fadeOutFrames : clip->fadeInFrames;
    frames = std::clamp<int64_t>(frames, 0, std::max<int64_t>(0, clip->duration - other));
    int64_t& field = head ? clip->fadeInFrames : clip->fadeOutFrames;
    if (field == frames)
        return false;
    field = frames;
    return true;
}

} // namespace kk::levelops
