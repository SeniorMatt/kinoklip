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

namespace cw {

/// What kind of thing a descriptor describes (also the Effects Library sections).
enum class EffectCategory {
    VideoEffect, ///< stored in ClipData::effects
    AudioEffect, ///< stored in ClipData::effects
    Transition, ///< stored in ClipData::transitionIn / transitionOut
    Generator, ///< a clip kind of its own (e.g. solid colour)
    Title, ///< text generators (ClipData::title)
    ClipProperties ///< built-in parameter groups stored directly on ClipData (transform, ...)
};

/// Value type of a parameter. Double/Int are animatable scalars (ParamSet::values) unless
/// ParamDescriptor::animatable is false; the others are statics (ParamSet::statics).
enum class ParamType {
    Double,
    Int,
    Bool,
    Color,
    Choice, ///< static QString holding one of ParamDescriptor::choices
    String
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
    /// "transform", "audio" or "title" (ClipData member), "solidColor"; empty = own ParamSet.
    QString storage;
    QVector<ParamDescriptor> params;

    /// Parameter descriptor with key @p key, or nullptr.
    [[nodiscard]] const ParamDescriptor* param(const QString& key) const;
};

/// Well-known descriptor ids of the built-in parameter groups.
namespace groups {
inline constexpr char Transform[] = "builtin.transform";
inline constexpr char Cropping[] = "builtin.cropping";
inline constexpr char Audio[] = "builtin.audio";
} // namespace groups

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

private:
    EffectRegistry();
    void registerBuiltins();

    QVector<EffectDescriptor> m_descriptors;
    QHash<QString, qsizetype> m_index; ///< id → position in m_descriptors (O(1) find())
};

} // namespace cw
