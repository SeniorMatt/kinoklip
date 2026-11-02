// SPDX-License-Identifier: GPL-3.0-or-later
// ParamAccess implementation.
#include "ui/inspector/ParamAccess.h"

#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::paramaccess {

namespace {

/// Value an Int parameter can take (descriptor ranges are inclusive).
double normalise(const ParamDescriptor& desc, double value)
{
    if (desc.type == ParamType::Int)
        value = std::round(value);
    return std::clamp(value, desc.min, desc.max);
}

/// For linked zoom: the partner key of zoomX/zoomY, or nullptr if @p ref is not linked.
const QString* linkedZoomPartner(const ClipData& clip, const ParamRef& ref)
{
    if (ref.scope != ParamScope::Transform)
        return nullptr;
    if (ref.key != keys::ZoomX && ref.key != keys::ZoomY)
        return nullptr;
    if (!clip.transform.staticValue(keys::ZoomLinked, true).toBool())
        return nullptr;
    return ref.key == keys::ZoomX ? &keys::ZoomY : &keys::ZoomX;
}

template<typename Clip, typename Fx>
Fx* findEffect(Clip& clip, quint64 effectId)
{
    for (auto& fx : clip.effects) {
        if (fx.id == effectId)
            return &fx;
    }
    return nullptr;
}

} // namespace

std::optional<ParamScope> scopeForStorage(const QString& storage)
{
    if (storage == QLatin1String("transform"))
        return ParamScope::Transform;
    if (storage == QLatin1String("audio"))
        return ParamScope::Audio;
    if (storage == QLatin1String("title"))
        return ParamScope::Title;
    if (storage == QLatin1String("solidColor"))
        return ParamScope::SolidColor;
    return std::nullopt;
}

const EffectInstance* effect(const ClipData& clip, quint64 effectId)
{
    return findEffect<const ClipData, const EffectInstance>(clip, effectId);
}

EffectInstance* effect(ClipData& clip, quint64 effectId)
{
    return findEffect<ClipData, EffectInstance>(clip, effectId);
}

const TransitionData* transition(const ClipData& clip, ParamScope scope)
{
    const auto& t = scope == ParamScope::TransitionIn ? clip.transitionIn : clip.transitionOut;
    return t ? &*t : nullptr;
}

TransitionData* transition(ClipData& clip, ParamScope scope)
{
    auto& t = scope == ParamScope::TransitionIn ? clip.transitionIn : clip.transitionOut;
    return t ? &*t : nullptr;
}

const ParamSet* paramSet(const ClipData& clip, const ParamRef& ref)
{
    switch (ref.scope) {
    case ParamScope::Transform:
        return &clip.transform;
    case ParamScope::Audio:
        return &clip.audio;
    case ParamScope::Title:
        return &clip.title;
    case ParamScope::SolidColor:
        return nullptr;
    case ParamScope::Effect: {
        const EffectInstance* fx = effect(clip, ref.effectId);
        return fx ? &fx->params : nullptr;
    }
    case ParamScope::TransitionIn:
    case ParamScope::TransitionOut: {
        const TransitionData* t = transition(clip, ref.scope);
        return t ? &t->params : nullptr;
    }
    }
    return nullptr;
}

ParamSet* paramSet(ClipData& clip, const ParamRef& ref)
{
    // Same lookup as the const overload; the const_cast is safe because `clip` is mutable.
    return const_cast<ParamSet*>(paramSet(static_cast<const ClipData&>(clip), ref));
}

const AnimatedValue* animated(const ClipData& clip, const ParamRef& ref)
{
    const ParamSet* set = paramSet(clip, ref);
    if (!set)
        return nullptr;
    const auto it = set->values.constFind(ref.key);
    return it == set->values.cend() ? nullptr : &*it;
}

double scalar(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    if (ref.key == kTransitionDuration) {
        const TransitionData* t = transition(clip, ref.scope);
        return t ? double(t->duration) : desc.def;
    }
    const AnimatedValue* value = animated(clip, ref);
    return value ? value->valueAt(localFrame) : desc.def;
}

void setScalar(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame,
    double value)
{
    value = normalise(desc, value);
    if (ref.key == kTransitionDuration) {
        if (TransitionData* t = transition(clip, ref.scope))
            t->duration
                = std::clamp<int64_t>(std::llround(value), 1, std::max<int64_t>(1, clip.duration));
        return;
    }
    ParamSet* set = paramSet(clip, ref);
    if (!set)
        return;
    set->animated(ref.key, desc.def).setValue(localFrame, value);
    if (const QString* partner = linkedZoomPartner(clip, ref))
        set->animated(*partner, desc.def).setValue(localFrame, value);
}

bool toggleKey(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    ParamSet* set = paramSet(clip, ref);
    if (!set || ref.key == kTransitionDuration)
        return false;
    AnimatedValue& value = set->animated(ref.key, desc.def);
    const bool remove = value.hasKeyAt(localFrame);
    auto apply = [&](AnimatedValue& v) {
        if (remove)
            v.removeKey(localFrame);
        else
            v.setKey(localFrame, v.valueAt(localFrame));
    };
    apply(value);
    if (const QString* partner = linkedZoomPartner(clip, ref)) {
        AnimatedValue& other = set->animated(*partner, desc.def);
        if (other.hasKeyAt(localFrame) == remove)
            apply(other);
    }
    return !remove;
}

std::optional<Interp> keyInterp(const ClipData& clip, const ParamRef& ref, int64_t localFrame)
{
    if (const AnimatedValue* value = animated(clip, ref)) {
        for (const Keyframe& k : value->keys) {
            if (k.frame == localFrame)
                return kk::keyInterp(k);
        }
    }
    return std::nullopt;
}

bool setKeyInterp(ClipData& clip, const ParamRef& ref, int64_t localFrame, Interp interp)
{
    ParamSet* set = paramSet(clip, ref);
    if (!set)
        return false;
    auto apply = [&](const QString& key) {
        const auto it = set->values.find(key);
        if (it == set->values.end())
            return false;
        for (Keyframe& k : it->keys) {
            if (k.frame == localFrame) {
                applyInterp(k, interp);
                return true;
            }
        }
        return false;
    };
    const bool changed = apply(ref.key);
    if (changed) {
        if (const QString* partner = linkedZoomPartner(clip, ref))
            apply(*partner);
    }
    return changed;
}

void reset(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc)
{
    if (desc.isScalar()) {
        if (ref.key == kTransitionDuration) {
            setScalar(clip, ref, desc, 0, desc.def);
            return;
        }
        if (ParamSet* set = paramSet(clip, ref)) {
            set->values.insert(ref.key, AnimatedValue(desc.def));
            if (const QString* partner = linkedZoomPartner(clip, ref))
                set->values.insert(*partner, AnimatedValue(desc.def));
        }
        return;
    }
    setStatic(clip, ref, desc.defStatic);
}

QVariant staticValue(const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc)
{
    if (ref.scope == ParamScope::SolidColor)
        return clip.solidColor.isValid() ? QVariant(clip.solidColor) : desc.defStatic;
    const ParamSet* set = paramSet(clip, ref);
    return set ? set->staticValue(ref.key, desc.defStatic) : desc.defStatic;
}

void setStatic(ClipData& clip, const ParamRef& ref, const QVariant& value)
{
    if (ref.scope == ParamScope::SolidColor) {
        clip.solidColor = value.value<QColor>();
        return;
    }
    if (ParamSet* set = paramSet(clip, ref))
        set->setStatic(ref.key, value);
}

bool isModified(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame)
{
    if (desc.isScalar()) {
        if (ref.key != kTransitionDuration) {
            const AnimatedValue* value = animated(clip, ref);
            if (value && value->isAnimated())
                return true;
        }
        return std::abs(scalar(clip, ref, desc, localFrame) - desc.def) > 1e-9;
    }
    return staticValue(clip, ref, desc) != desc.defStatic;
}

} // namespace kk::paramaccess
