// SPDX-License-Identifier: GPL-3.0-or-later
// Keyframe animation value types: Interp, Keyframe, AnimatedValue and ParamSet.
#pragma once

#include <QMap>
#include <QString>
#include <QVariant>
#include <QVector>

#include <cstdint>
#include <optional>

namespace kk {

/// How the value travels from one keyframe to the next. The interpolation mode stored on keyframe
/// K governs the segment K → K+1 (the last key's mode is irrelevant).
enum class Interp {
    Linear, ///< constant speed
    EaseIn, ///< starts slowly, accelerates towards the next key (t²)
    EaseOut, ///< starts fast, decelerates into the next key (1 − (1 − t)²)
    EaseInOut, ///< slow at both ends (smoothstep 3t² − 2t³)
    Hold ///< keeps this key's value until the next key, then jumps
};

/// Applies the easing curve of @p interp to a normalised segment position @p t in [0, 1].
/// Returns the eased position in [0, 1] (Hold always returns 0).
[[nodiscard]] double easeCurve(Interp interp, double t);

/// One keyframe of an AnimatedValue.
struct Keyframe
{
    int64_t frame = 0; ///< clip-local frame (timeline frame − clip.start), see PLAN §5.1
    double value = 0.0;
    Interp interp = Interp::Linear; ///< curve of the segment that starts at this key

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// A keyframeable scalar parameter (position, opacity, volume, blur radius, ...).
///
/// Without keys the value is the constant `base`. With keys, `base` is ignored and the value is
/// interpolated between the keys (held constant before the first and after the last key).
/// Plain value type; thread-safety as for any value type (copy before sharing).
struct AnimatedValue
{
    double base = 0.0; ///< value used while the parameter has no keyframes
    QVector<Keyframe> keys; ///< always sorted by frame, at most one key per frame

    AnimatedValue() = default;
    /// Creates a non-animated value.
    explicit AnimatedValue(double baseValue)
        : base(baseValue)
    { }

    /// Value at clip-local frame @p localFrame (base if there are no keys).
    [[nodiscard]] double valueAt(int64_t localFrame) const;

    /// Adds a key at @p localFrame or replaces the value/interp of the existing key there.
    void setKey(int64_t localFrame, double value, Interp interp = Interp::Linear);
    /// Removes the key at @p localFrame (no-op if there is none). Removing the last remaining key
    /// copies its value into `base` so the parameter keeps its current look.
    void removeKey(int64_t localFrame);
    /// True if a key exists exactly at @p localFrame.
    [[nodiscard]] bool hasKeyAt(int64_t localFrame) const;
    /// True if the value has at least one keyframe.
    [[nodiscard]] bool isAnimated() const { return !keys.isEmpty(); }

    /// Inspector-style edit: sets a key at @p localFrame when animated, otherwise sets `base`.
    void setValue(int64_t localFrame, double value);
    /// Frame of the closest key strictly before @p localFrame, if any.
    [[nodiscard]] std::optional<int64_t> previousKeyFrame(int64_t localFrame) const;
    /// Frame of the closest key strictly after @p localFrame, if any.
    [[nodiscard]] std::optional<int64_t> nextKeyFrame(int64_t localFrame) const;
    /// Moves every key by @p deltaFrames (used when a clip's head is trimmed or a clip is split).
    void shiftKeys(int64_t deltaFrames);

    friend bool operator==(const AnimatedValue&, const AnimatedValue&) = default;
};

/// A named set of parameters: keyframeable scalars in `values`, non-animatable settings
/// (colours, strings, bools, choice ids) in `statics`. Keys are the well-known names from
/// core/ParamKeys.h or an EffectDescriptor's ParamDescriptor::key.
struct ParamSet
{
    QMap<QString, AnimatedValue> values;
    QMap<QString, QVariant> statics;

    /// Value of animated parameter @p key at @p localFrame, or @p fallback if the key is absent.
    [[nodiscard]] double value(const QString& key, int64_t localFrame, double fallback) const;
    /// Value of animated parameter @p key using the documented default from kk::keys as fallback.
    [[nodiscard]] double value(const QString& key, int64_t localFrame) const;
    /// Static parameter @p key, or @p fallback if absent.
    [[nodiscard]] QVariant staticValue(const QString& key, const QVariant& fallback = {}) const;
    /// Returns the animated parameter @p key, creating it with base @p defaultBase if missing.
    AnimatedValue& animated(const QString& key, double defaultBase);
    /// Sets the constant (non-keyframed) value of @p key, creating it if needed.
    void setBase(const QString& key, double value);
    /// Sets static parameter @p key.
    void setStatic(const QString& key, const QVariant& value) { statics.insert(key, value); }
    /// True if any parameter in the set has keyframes.
    [[nodiscard]] bool isAnimated() const;
    /// Shifts the keys of every animated parameter by @p deltaFrames.
    void shiftKeys(int64_t deltaFrames);

    friend bool operator==(const ParamSet&, const ParamSet&) = default;
};

/// Stable string id of an interpolation mode (used in project files), e.g. "easeInOut".
[[nodiscard]] QString interpToString(Interp interp);
/// Parses a string produced by interpToString(); unknown strings yield Interp::Linear.
[[nodiscard]] Interp interpFromString(const QString& text);

} // namespace kk
