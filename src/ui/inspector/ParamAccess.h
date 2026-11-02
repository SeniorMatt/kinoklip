// SPDX-License-Identifier: GPL-3.0-or-later
// ParamAccess — addresses one inspector parameter inside a ClipData and reads/writes it.
// This is the only place that knows where each descriptor's values are stored on a clip.
#pragma once

#include "core/EffectRegistry.h"
#include "core/TimelineData.h"

#include <QString>
#include <QVariant>

#include <cstdint>
#include <optional>

namespace kk {

/// Which parameter storage of a clip a parameter lives in.
enum class ParamScope {
    Transform, ///< ClipData::transform (Transform + Cropping groups)
    Audio, ///< ClipData::audio
    Title, ///< ClipData::title (Title clips)
    SolidColor, ///< ClipData::solidColor (Solid clips; the single "color" static)
    Effect, ///< ClipData::effects[i].params, i found by ParamRef::effectId
    TransitionIn, ///< ClipData::transitionIn->params (+ the synthetic duration key)
    TransitionOut, ///< ClipData::transitionOut->params (+ the synthetic duration key)
    Mask ///< ClipData::masks[i].params, i found by ParamRef::effectId (= MaskData::id);
         ///< the statics maskkeys::Mode / Invert map to MaskData::mode / invert, and the key
         ///< maskkeys::Shape to the animated path (animated() = its timing; toggleKey() and
         ///< setKeyInterp() edit shape keys; setScalar() ignores it)
};

/// Full address of one parameter: clip, storage, effect instance (Effect scope) and key.
struct ParamRef
{
    quint64 clipId = 0;
    ParamScope scope = ParamScope::Transform;
    quint64 effectId = 0; ///< EffectInstance::id (Effect scope) or MaskData::id (Mask), else 0
    QString key;

    friend bool operator==(const ParamRef&, const ParamRef&) = default;
};

namespace paramaccess {

/// Synthetic key of a transition's length (TransitionData::duration, in timeline frames).
/// It is not stored in the ParamSet; get/setScalar() route it to the duration field.
inline const QString kTransitionDuration = QStringLiteral("@duration");

/// Scope used for the descriptor storage name ("transform", "audio", "title", "solidColor").
/// Returns std::nullopt for descriptors with their own ParamSet (effects, transitions).
[[nodiscard]] std::optional<ParamScope> scopeForStorage(const QString& storage);

/// The ParamSet addressed by @p ref inside @p clip, or nullptr (missing effect/transition,
/// or ParamScope::SolidColor which has no ParamSet).
[[nodiscard]] const ParamSet* paramSet(const ClipData& clip, const ParamRef& ref);
/// Mutable overload of paramSet().
[[nodiscard]] ParamSet* paramSet(ClipData& clip, const ParamRef& ref);
/// The effect instance addressed by @p ref (Effect scope), or nullptr.
[[nodiscard]] const EffectInstance* effect(const ClipData& clip, quint64 effectId);
/// Mutable overload of effect().
[[nodiscard]] EffectInstance* effect(ClipData& clip, quint64 effectId);
/// The mask addressed by a Mask-scope ref (MaskData::id @p maskId), or nullptr.
[[nodiscard]] const MaskData* mask(const ClipData& clip, quint64 maskId);
/// Mutable overload of mask().
[[nodiscard]] MaskData* mask(ClipData& clip, quint64 maskId);
/// The transition of a TransitionIn/TransitionOut scope, or nullptr.
[[nodiscard]] const TransitionData* transition(const ClipData& clip, ParamScope scope);
/// Mutable overload of transition().
[[nodiscard]] TransitionData* transition(ClipData& clip, ParamScope scope);

/// The animated value of a scalar parameter, or nullptr if the set does not contain it yet
/// (it then shows the descriptor default). Never allocates.
[[nodiscard]] const AnimatedValue* animated(const ClipData& clip, const ParamRef& ref);

/// Value of scalar parameter @p ref at clip-local frame @p localFrame (descriptor default if
/// absent). O(log keys), no allocation.
[[nodiscard]] double scalar(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame);
/// Inspector-style scalar edit at @p localFrame: sets a key if the parameter is animated,
/// else its constant value. Honours linked zoom (Transform zoomX/zoomY with zoomLinked on).
void setScalar(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame,
    double value);
/// Adds a key at @p localFrame with the current value, or removes the key that is there.
/// Honours linked zoom like setScalar(). Returns true if a key now exists at @p localFrame.
bool toggleKey(
    ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame);
/// Interpolation of the key at @p localFrame (std::nullopt if there is no key there).
[[nodiscard]] std::optional<Interp> keyInterp(
    const ClipData& clip, const ParamRef& ref, int64_t localFrame);
/// Applies interpolation preset @p interp to the key at @p localFrame (spec 15.6).
/// Honours linked zoom like setScalar(). Returns false if there is no key at that frame.
bool setKeyInterp(ClipData& clip, const ParamRef& ref, int64_t localFrame, Interp interp);
/// Resets a parameter to its descriptor default (removes all keys of a scalar).
void reset(ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc);

/// Static parameter value (descriptor default if absent).
[[nodiscard]] QVariant staticValue(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc);
/// Sets a static parameter.
void setStatic(ClipData& clip, const ParamRef& ref, const QVariant& value);

/// True if the parameter differs from its descriptor default (at @p localFrame for scalars,
/// or because it has keys). Used to enable the reset button.
[[nodiscard]] bool isModified(
    const ClipData& clip, const ParamRef& ref, const ParamDescriptor& desc, int64_t localFrame);

} // namespace paramaccess

} // namespace kk
