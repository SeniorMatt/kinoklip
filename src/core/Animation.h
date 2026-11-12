// SPDX-License-Identifier: GPL-3.0-or-later
// Keyframe animation value types: Interp, KeyHandle, Keyframe, AnimatedValue and ParamSet.
#pragma once

#include <QMap>
#include <QString>
#include <QVariant>
#include <QVector>

#include <cstdint>
#include <optional>

namespace kk {

/// Keyframe type as the user picks it (timeline / inspector context menu, curve editor tool bar).
/// It is a preset for the two handles of a key (see KeyHandle) plus the hold flag:
///
/// | Interp     | in handle | out handle | hold |
/// |------------|-----------|------------|------|
/// | Linear     | Linear    | Linear     | no   |
/// | EaseIn     | Ease      | Linear     | no   |  arrives at the key slowly
/// | EaseOut    | Linear    | Ease       | no   |  leaves the key slowly
/// | EaseInOut  | Ease      | Ease       | no   |
/// | Smooth     | Smooth    | Smooth     | no   |  automatic Bezier tangents through the key
/// | Hold       | (kept)    | (kept)     | yes  |  "static": value jumps at the next key
///
/// Custom is never applied; keyInterp() reports it for keys whose handles were dragged in the
/// curve editor (KeyHandle::Mode::Free) or that mix modes in a way no preset describes.
enum class Interp {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    Hold,
    Smooth,
    Custom
};

/// One Bezier handle of a keyframe (the incoming one points back in time, the outgoing one
/// forward). Handles live in (frame, value) space; the curve of a segment A -> B is the cubic
/// Bezier through A, A + out(A), B - in(B), B, evaluated as value over time.
///
/// Except for Free, a handle has no stored geometry: it is resolved from the neighbouring keys
/// (always one third of the segment length in time), so moving keys never distorts a curve.
struct KeyHandle
{
    enum class Mode : quint8 {
        Linear, ///< points at the neighbouring key: straight segment when both ends are Linear
        Ease, ///< horizontal: the value arrives / leaves with zero speed
        Smooth, ///< tangent from the neighbours (Catmull-Rom), flat at extrema and at the ends
        Free ///< user-dragged geometry in dtFrames / dv
    };
    Mode mode = Mode::Linear;
    /// Free only: time length of the handle in frames (> 0; clamped to the segment on use,
    /// keeping the slope). Unused for the other modes.
    double dtFrames = 0.0;
    /// Free only: value offset of the handle tip from the key (for the incoming handle it is
    /// key.value - tip.value, i.e. the same sign convention as a slope times dtFrames).
    double dv = 0.0;

    /// A handle of mode @p m without stored geometry.
    [[nodiscard]] static KeyHandle withMode(Mode m)
    {
        KeyHandle h;
        h.mode = m;
        return h;
    }

    friend bool operator==(const KeyHandle&, const KeyHandle&) = default;
};

/// One keyframe of an AnimatedValue.
struct Keyframe
{
    int64_t frame = 0; ///< clip-local frame (timeline frame - clip.start), see PLAN 5.1
    double value = 0.0;
    KeyHandle in; ///< handle of the segment arriving at this key
    KeyHandle out; ///< handle of the segment leaving this key
    /// "Static" key: the value stays at this key's value until the next key, then jumps.
    bool hold = false;
    /// "Step In": the segment arriving at this key is flat at this key's value, so the value
    /// jumps right after the previous key (the mirror image of `hold`; Fusion's Step In).
    bool stepIn = false;

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// Sets the handles / hold flag of @p key to preset @p interp (Custom is ignored). Hold keeps
/// the handles so switching back restores the previous shape.
void applyInterp(Keyframe& key, Interp interp);
/// The preset that describes @p key, or Interp::Custom.
[[nodiscard]] Interp keyInterp(const Keyframe& key);

/// What an animated value does before its first and after its last key (Fusion's Set Loop /
/// Pre-Loop, Blender's F-Curve Cycles, Natron's extrapolation). The repeated range is the keyed
/// range [first key, last key]; with a single key every mode is Constant.
enum class Extrapolation : quint8 {
    Constant, ///< hold the first / last key's value (the default)
    Loop, ///< repeat the keyed range
    PingPong, ///< repeat the keyed range, every other repeat reversed in time
    Relative, ///< repeat the keyed range, each repeat starting at the previous repeat's end value
    Linear ///< continue along the tangent of the end key
};

/// Stable string id of an extrapolation mode as stored in project files ("constant", "loop",
/// "pingPong", "relative", "linear").
[[nodiscard]] QString extrapolationToString(Extrapolation mode);
/// Parses extrapolationToString(); unknown strings yield Constant.
[[nodiscard]] Extrapolation extrapolationFromString(const QString& text);

/// A keyframeable scalar parameter (position, opacity, volume, blur radius, ...).
///
/// Without keys the value is the constant `base`. With keys, `base` is ignored and the value is
/// interpolated between the keys; before the first and after the last key it follows `pre` /
/// `post` (held constant by default).
/// Plain value type; thread-safety as for any value type (copy before sharing).
struct AnimatedValue
{
    double base = 0.0; ///< value used while the parameter has no keyframes
    QVector<Keyframe> keys; ///< always sorted by frame, at most one key per frame
    Extrapolation pre = Extrapolation::Constant; ///< before the first key
    Extrapolation post = Extrapolation::Constant; ///< after the last key

    AnimatedValue() = default;
    /// Creates a non-animated value.
    explicit AnimatedValue(double baseValue)
        : base(baseValue)
    { }

    /// Value at clip-local frame @p localFrame (base if there are no keys). Hot path: binary
    /// search plus at most a few Newton steps (only for Free handles); never allocates.
    [[nodiscard]] double valueAt(int64_t localFrame) const;
    /// Value at a fractional clip-local frame (audio gain envelopes evaluate between frames).
    /// Hold keys jump exactly at the next key's frame.
    [[nodiscard]] double valueAtTime(double localFrame) const;

    /// Adds a key at @p localFrame or replaces the value of the existing key there. A new key
    /// gets preset @p interp; an existing key only changes its preset when @p interp is given.
    void setKey(int64_t localFrame, double value, std::optional<Interp> interp = std::nullopt);
    /// Inserts or replaces the complete key @p key (handles included).
    void putKey(const Keyframe& key);
    /// Removes the key at @p localFrame (no-op if there is none). Removing the last remaining key
    /// copies its value into `base` so the parameter keeps its current look.
    void removeKey(int64_t localFrame);
    /// Index of the key at exactly @p localFrame, or -1.
    [[nodiscard]] qsizetype indexOf(int64_t localFrame) const;
    /// True if a key exists exactly at @p localFrame.
    [[nodiscard]] bool hasKeyAt(int64_t localFrame) const { return indexOf(localFrame) >= 0; }
    /// True if the value has at least one keyframe.
    [[nodiscard]] bool isAnimated() const { return !keys.isEmpty(); }

    /// Inspector-style edit: sets a key at @p localFrame when animated, otherwise sets `base`.
    void setValue(int64_t localFrame, double value);
    /// Frame of the closest key strictly before @p localFrame, if any.
    [[nodiscard]] std::optional<int64_t> previousKeyFrame(int64_t localFrame) const;
    /// Frame of the closest key strictly after @p localFrame, if any.
    [[nodiscard]] std::optional<int64_t> nextKeyFrame(int64_t localFrame) const;
    /// Mirrors the keys at @p frames in time inside their own range (the first becomes the last):
    /// frames, handles (in <-> out), hold <-> stepIn. Frames without a key are ignored; fewer
    /// than two keys: no-op. Returns the new frames of the moved keys (ascending).
    QVector<int64_t> reverseKeys(const QVector<int64_t>& frames);
    /// Scales the time of the keys at @p frames around @p pivot (frame' = pivot + (frame -
    /// pivot) * @p factor, rounded; free handle lengths scale too, keeping their slope). A key
    /// landing on another key replaces it. @p factor must be > 0. Returns the new frames.
    QVector<int64_t> stretchKeys(const QVector<int64_t>& frames, double pivot, double factor);
    /// True if a mode other than Constant applies outside the keys.
    [[nodiscard]] bool isExtrapolated() const
    {
        return keys.size() > 1
            && (pre != Extrapolation::Constant || post != Extrapolation::Constant);
    }
    /// Moves every key by @p deltaFrames (used when a clip's head is trimmed or a clip is split).
    void shiftKeys(int64_t deltaFrames);

    /// Handle tip offsets of key @p index as they are evaluated (Free handles clamped to their
    /// segment, the other modes resolved from the neighbours): returns {dtFrames, dv}, dt >= 0.
    /// For the first key's in handle / the last key's out handle the neighbour is missing; the
    /// handle then has the length of the existing segment (or 0 for a single key).
    [[nodiscard]] std::pair<double, double> inHandle(qsizetype index) const;
    /// Outgoing counterpart of inHandle().
    [[nodiscard]] std::pair<double, double> outHandle(qsizetype index) const;

    friend bool operator==(const AnimatedValue&, const AnimatedValue&) = default;

private:
    [[nodiscard]] double segmentValue(qsizetype i, double localFrame) const;
    [[nodiscard]] double innerValue(double localFrame) const;
    [[nodiscard]] double extrapolatedValue(double localFrame, bool before) const;
    [[nodiscard]] double smoothSlope(qsizetype index) const;
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

/// Stable string id of an interpolation preset (used in the UI and tests), e.g. "easeInOut".
[[nodiscard]] QString interpToString(Interp interp);
/// The preset new keyframes get when no preset is asked for (Preferences > Editing > "New
/// keyframes", setting "edit/newKeyInterp"; Linear by default, never Custom).
[[nodiscard]] Interp defaultKeyInterp();
/// Stores the "New keyframes" preference (Custom is ignored).
void setDefaultKeyInterp(Interp interp);
/// Parses a string produced by interpToString(); unknown strings yield Interp::Linear.
[[nodiscard]] Interp interpFromString(const QString& text);
/// Stable string id of a handle mode stored in project files ("linear", "ease", "smooth",
/// "free").
[[nodiscard]] QString handleModeToString(KeyHandle::Mode mode);
/// Parses handleModeToString(); unknown strings yield Mode::Linear.
[[nodiscard]] KeyHandle::Mode handleModeFromString(const QString& text);

/// Converts keys stored with the format-version-1 meaning of "interp" (the curve of the segment
/// that *starts* at the key: easeIn = t^2, easeOut = 1-(1-t)^2, easeInOut = smoothstep, hold)
/// into handles: easeIn -> Ease out handle of the key, easeOut -> Ease in handle of the next
/// key, easeInOut -> both, hold -> hold. Ease-in-out is reproduced exactly (cubic Bezier with
/// flat thirds handles is the smoothstep); easeIn / easeOut become the closest Bezier ease.
/// @p legacy holds one v1 interp per key, parallel to @p value.keys.
void migrateLegacyInterp(AnimatedValue& value, const QVector<Interp>& legacy);

} // namespace kk
