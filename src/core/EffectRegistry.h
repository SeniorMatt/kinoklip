// SPDX-License-Identifier: GPL-3.0-or-later
// EffectRegistry — catalogue of effect, transition, generator and parameter-group descriptors.
// Descriptors only: the pixel/audio code lives in src/render, keyed by descriptor id.
#pragma once

#include "core/TimelineData.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace kk {

/// What kind of thing a descriptor describes (also the Effects Library sections).
enum class EffectCategory {
    VideoEffect, ///< stored in ClipData::effects
    AudioEffect, ///< stored in ClipData::effects
    Transition, ///< stored in ClipData::transitionIn / transitionOut
    Generator, ///< a clip kind of its own (e.g. solid colour)
    Title, ///< text generators (ClipData::title)
    ClipProperties, ///< built-in parameter groups stored directly on ClipData (transform, ...)
    /// Library section "Effects": clips that process the tracks below them (Adjustment Clip)
    Effects,
    /// Audio clip transitions, stored in ClipData::transitionIn / transitionOut like video
    /// ones; their gain curves are in core/AudioTransitions.h
    AudioTransition
};

/// Value type of a parameter. Double/Int are animatable scalars (ParamSet::values) unless
/// ParamDescriptor::animatable is false; the others are statics (ParamSet::statics).
enum class ParamType {
    Double,
    Int,
    Bool,
    Color,
    Choice, ///< static QString holding one of ParamDescriptor::choices
    String,
    /// Static QString of tone-curve points "x,y x,y ..." (0..1, core/CurveParam.h); edited
    /// by the Inspector's curve widget. Not keyframable.
    Curve,
    /// Static QString holding an absolute file path (e.g. a .cube LUT). Project files store
    /// it absolute and relative to the project (core/FileParams.h). Not keyframable.
    File
};

/// Describes one parameter so the Inspector can build an editor generically.
struct ParamDescriptor
{
    QString key; ///< key inside the ParamSet
    QString label; ///< translated UI label
    ParamType type = ParamType::Double;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0; ///< default for Double/Int
    double step = 1.0; ///< spin box / slider step
    QString unit; ///< display suffix, e.g. "px", "%", "°", "dB"
    QStringList choices; ///< Choice: stored values
    QStringList choiceLabels; ///< Choice: translated labels, parallel to choices
    QVariant defStatic; ///< default for Bool/Color/Choice/String
    bool animatable = true; ///< Double/Int only: may be keyframed
    /// Color, or a Double hue in degrees: the Inspector shows an eyedropper button that picks
    /// the value from the timeline viewer (SelectionModel::colorPicker(), the viewer's
    /// ColorPickOverlay). A hue parameter receives the hue of the picked colour.
    bool viewerPicker = false;
    /// File only: name filter of the file dialog, e.g. "LUT files (*.cube)".
    QString fileFilter;
    /// Double only: the Inspector, the Keyframes panel and the curve editors show, take and
    /// draw the negated stored value. Transform Position Y / Anchor Y are stored in pixels
    /// down from the centre (the renderer's convention, unchanged in project files) and shown
    /// as Resolve shows them, positive up (spec 24.16).
    bool displayNegated = false;
    /// The Inspector does not list the parameter (stored data such as a track, or the points
    /// of a warp that have their own controls). It is still saved, keyframed and copied.
    bool hidden = false;

    /// The value shown for stored value @p stored (and back: the same negation).
    [[nodiscard]] double displayed(double stored) const
    {
        return displayNegated ? -stored : stored;
    }

    /// True if the value lives in ParamSet::values (keyframeable scalar).
    [[nodiscard]] bool isScalar() const
    {
        return type == ParamType::Double || type == ParamType::Int;
    }
};

/// Describes one effect / transition / generator / parameter group.
struct EffectDescriptor
{
    QString id; ///< stable id stored in project files, e.g. "gaussian_blur"
    QString name; ///< translated display name
    EffectCategory category = EffectCategory::VideoEffect;
    QString description; ///< one-line tooltip
    /// Where the parameters are stored for ClipProperties groups / generators:
    /// "transform", "audio" or "title" (ClipData member; generators keep their parameters in
    /// ClipData::title too), "solidColor"; empty = own ParamSet.
    QString storage;
    /// Effects library folder of a video effect (one of effectgroups::, e.g. "key"); empty =
    /// directly in its category section.
    QString group;
    QVector<ParamDescriptor> params;

    /// Parameter descriptor with key @p key, or nullptr.
    [[nodiscard]] const ParamDescriptor* param(const QString& key) const;
};

/// Well-known descriptor ids of the built-in parameter groups.
namespace groups {
inline constexpr char Transform[] = "builtin.transform";
inline constexpr char Cropping[] = "builtin.cropping";
inline constexpr char Composite[] = "builtin.composite"; ///< Composite Mode + Opacity
/// Dynamic Zoom (spec 24.20): the switch is the group header's, the rectangles are edited in
/// the viewer; the descriptor holds the ease.
inline constexpr char DynamicZoom[] = "builtin.dynamic_zoom";
inline constexpr char Audio[] = "builtin.audio";
/// The Audio tab's Equalizer section (4 bands, stored in ClipData::audio).
inline constexpr char AudioEq[] = "builtin.audio_eq";
} // namespace groups

/// Effects library folders of the video effects (EffectDescriptor::group), named the way an
/// editor user expects them. The library shows them in alphabetical order of their names.
namespace effectgroups {
inline constexpr char Blur[] = "blur";
inline constexpr char Color[] = "color";
inline constexpr char Distort[] = "distort";
inline constexpr char Key[] = "key";
inline constexpr char Light[] = "light";
inline constexpr char Stylize[] = "stylize";
inline constexpr char Time[] = "time";
inline constexpr char Temporal[] = "temporal";
inline constexpr char Texture[] = "texture";
inline constexpr char Revival[] = "revival";
inline constexpr char Sharpen[] = "sharpen";
inline constexpr char Refine[] = "refine";
inline constexpr char Transform[] = "transform";
/// Folders of the video transitions (Resolve's Video Transitions bins).
inline constexpr char Dissolve[] = "dissolve";
inline constexpr char Iris[] = "iris";
inline constexpr char Motion[] = "motion";
inline constexpr char Shape[] = "shape";
inline constexpr char Wipe[] = "wipe";
inline constexpr char FusionTransitions[] = "fusion_transitions";
inline constexpr char ResolveFxTransitions[] = "resolve_fx_transitions";
} // namespace effectgroups

/// Process-wide catalogue of descriptors.
///
/// Built-ins are registered on first access. Further descriptors may be added at start-up
/// (GUI thread) with add(); after that the registry is read-only and safe to read from any
/// thread.
class EffectRegistry
{
public:
    /// The global registry with the built-in catalogue (PLAN §5.4).
    static EffectRegistry& instance();

    /// Registers (or replaces) a descriptor.
    void add(EffectDescriptor descriptor);
    /// Descriptor with id @p id, or nullptr. O(1), no allocation (called per effect per frame
    /// by the renderer). Pointers stay valid as long as no descriptor is added.
    [[nodiscard]] const EffectDescriptor* find(const QString& id) const;
    /// All descriptors of @p category in registration order.
    [[nodiscard]] QVector<const EffectDescriptor*> byCategory(EffectCategory category) const;
    /// All descriptors in registration order.
    [[nodiscard]] const QVector<EffectDescriptor>& all() const { return m_descriptors; }

    /// ParamSet filled with the defaults of descriptor @p id (empty if unknown).
    [[nodiscard]] ParamSet defaultParams(const QString& id) const;
    /// New EffectInstance of type @p id with default parameters and the given instance id.
    [[nodiscard]] EffectInstance createEffect(const QString& id, quint64 instanceId) const;
    /// New TransitionData of type @p id with default parameters.
    [[nodiscard]] TransitionData createTransition(const QString& id, int64_t durationFrames) const;

    /// Translated name of a category, for section headers.
    [[nodiscard]] static QString categoryName(EffectCategory category);
    /// Translated name of an effects library folder (effectgroups::), e.g. "Key"; the id
    /// itself for an unknown folder.
    [[nodiscard]] static QString groupName(const QString& group);
    /// Every folder id used by a descriptor of @p category, sorted by groupName().
    [[nodiscard]] QStringList groups(EffectCategory category) const;

private:
    EffectRegistry();
    void registerBuiltins();
    void registerGenerators(); ///< the generator catalogue (BuiltinEffects.cpp)
    void registerTitles(); ///< the title presets (BuiltinEffects.cpp, core/TitlePresets.h)
    void registerTransitions(); ///< the video transitions of the folders (BuiltinTransitions.cpp)
    void registerVideoEffects(); ///< the v4 video effects (BuiltinVideoEffects.cpp)
    /// The second batch of v4 video effects (BuiltinVideoEffects2.cpp).
    void registerVideoEffects2();
    void registerVideoEffects3();
    void registerVideoEffects4(); ///< 1.2 second part (spec 57)
    void registerVideoEffects5(); ///< 1.2 multi-frame effects, Patch Replacer (spec 60)
    void registerVideoEffects6(); ///< 1.2 template looks: CCTV, Glitch, Drone, DSLR, DVE, Call,
                                  ///< Camera (spec 65)
    void registerVideoEffects7(); ///< 1.2 tracking: Match Move, Surface Tracker, Warper (spec 70)
    void registerVideoEffects8(); ///< 1.4 Background Removal (AI, spec 78)
    /// The clip audio effects (BuiltinAudioEffects.cpp).
    void registerAudioEffects();

    QVector<EffectDescriptor> m_descriptors;
    QHash<QString, qsizetype> m_index; ///< id → position in m_descriptors (O(1) find())
};

} // namespace kk
