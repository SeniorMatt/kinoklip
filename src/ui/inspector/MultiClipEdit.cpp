// SPDX-License-Identifier: GPL-3.0-or-later
// MultiClipEdit — peers of the shown clip, parameter matching and change propagation.
#include "ui/inspector/MultiClipEdit.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/TimelineData.h"

namespace kk::multiclip {

namespace {

/// Position of instance @p id among @p clip's effects of the same type (-1 = not found).
int ordinalOf(const ClipData& clip, quint64 id, QString* type)
{
    int n = 0;
    const EffectInstance* target = paramaccess::effect(clip, id);
    if (!target)
        return -1;
    for (const EffectInstance& e : clip.effects) {
        if (e.id == id) {
            *type = e.type;
            return n;
        }
        if (e.type == target->type)
            ++n;
    }
    return -1;
}

/// The @p ordinal-th effect of type @p type on @p clip, or nullptr (const and mutable).
template<typename Clip>
auto* nthOfType(Clip& clip, const QString& type, int ordinal)
{
    for (auto& e : clip.effects)
        if (e.type == type && ordinal-- == 0)
            return &e;
    return static_cast<decltype(&clip.effects[0])>(nullptr);
}

/// Copies the entries of a QMap that differ between @p before and @p after onto @p peer.
template<typename Map>
bool copyChangedEntries(const Map& before, const Map& after, Map& peer)
{
    bool changed = false;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto old = before.constFind(it.key());
        if (old != before.cend() && *old == it.value())
            continue;
        const auto cur = peer.constFind(it.key());
        if (cur != peer.cend() && *cur == it.value())
            continue;
        peer.insert(it.key(), it.value());
        changed = true;
    }
    // Removed entries (a reset back to the default).
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (!after.contains(it.key()) && peer.remove(it.key()) > 0)
            changed = true;
    }
    return changed;
}

bool copyChanged(const ParamSet& before, const ParamSet& after, ParamSet& peer)
{
    if (before == after)
        return false;
    const bool values = copyChangedEntries(before.values, after.values, peer.values);
    const bool statics = copyChangedEntries(before.statics, after.statics, peer.statics);
    return values || statics;
}

/// Titles of different presets share their parameters, so they edit together.
bool sameKind(const ClipData& a, const ClipData& b)
{
    return a.kind == b.kind && (a.kind == ClipKind::Title || a.generator == b.generator);
}

} // namespace

QVector<quint64> peers(const TimelineData& data, const QVector<quint64>& selection, quint64 shownId)
{
    QVector<quint64> out;
    if (!selection.contains(shownId))
        return out;
    const auto shownLoc = editops::findClip(data, shownId);
    const ClipData* shown = editops::clipById(data, shownId);
    if (!shownLoc || !shown)
        return out;
    for (quint64 id : selection) {
        if (id == shownId)
            continue;
        const auto loc = editops::findClip(data, id);
        const ClipData* c = editops::clipById(data, id);
        // Transform, cropping, composite and audio sets exist on every clip, so clips of any
        // kind on the same kind of track are peers; sets that belong to one kind (titles,
        // solids, effects) are copied only between clips of that kind (propagate()).
        if (!loc || !c || loc->track.type != shownLoc->track.type)
            continue;
        const TrackData* track = data.track(loc->track);
        if (!track || track->locked || out.contains(id))
            continue;
        out.append(id);
    }
    return out;
}

std::optional<ParamRef> peerRef(const ClipData& shown, const ClipData& peer, const ParamRef& ref)
{
    ParamRef out = ref;
    out.clipId = peer.id;
    switch (ref.scope) {
    case ParamScope::Title:
    case ParamScope::SolidColor:
    case ParamScope::Effect:
        if (!sameKind(shown, peer))
            return std::nullopt;
        break;
    default:
        break;
    }
    switch (ref.scope) {
    case ParamScope::Title:
        // A caption's text is its own (spec 28.19): never matched, never "Mixed".
        if (shown.kind == ClipKind::Subtitle && ref.key == keys::Text)
            return std::nullopt;
        return out;
    case ParamScope::Transform:
    case ParamScope::Audio:
    case ParamScope::SolidColor:
        return out;
    case ParamScope::Effect: {
        QString type;
        const int ordinal = ordinalOf(shown, ref.effectId, &type);
        if (ordinal < 0)
            return std::nullopt;
        const EffectInstance* match = nthOfType(peer, type, ordinal);
        if (!match)
            return std::nullopt;
        out.effectId = match->id;
        return out;
    }
    case ParamScope::TransitionIn:
    case ParamScope::TransitionOut:
    case ParamScope::Mask:
    case ParamScope::CompNode:
    case ParamScope::TitleLayer: // layers are per clip: never matched between clips
        return std::nullopt;
    }
    return std::nullopt;
}

bool propagate(const ClipData& before, const ClipData& after, ClipData& peer)
{
    bool changed = copyChanged(before.transform, after.transform, peer.transform);
    changed = copyChanged(before.audio, after.audio, peer.audio) || changed;
    if (!sameKind(peer, after))
        return changed;
    {
        if (after.kind == ClipKind::Subtitle) {
            // Typing a caption's words changes that caption only (spec 28.19); its style
            // settings still reach every selected caption.
            ParamSet b = before.title;
            ParamSet a = after.title;
            for (ParamSet* set : {&b, &a}) {
                set->values.remove(keys::Text);
                set->statics.remove(keys::Text);
            }
            changed = copyChanged(b, a, peer.title) || changed;
        } else {
            changed = copyChanged(before.title, after.title, peer.title) || changed;
        }
    }
    if (before.solidColor != after.solidColor && peer.kind == ClipKind::Solid
        && peer.solidColor != after.solidColor) {
        peer.solidColor = after.solidColor;
        changed = true;
    }
    for (const EffectInstance& e : after.effects) {
        const EffectInstance* old = paramaccess::effect(before, e.id);
        if (!old || *old == e)
            continue; // a new instance (structure edit) or unchanged
        QString type;
        const int ordinal = ordinalOf(after, e.id, &type);
        EffectInstance* match = ordinal >= 0 ? nthOfType(peer, type, ordinal) : nullptr;
        if (!match)
            continue;
        if (old->enabled != e.enabled && match->enabled != e.enabled) {
            match->enabled = e.enabled;
            changed = true;
        }
        changed = copyChanged(old->params, e.params, match->params) || changed;
    }
    return changed;
}

} // namespace kk::multiclip
