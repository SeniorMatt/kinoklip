// SPDX-License-Identifier: GPL-3.0-or-later
// EffectBuilders — small helpers that build ParamDescriptor / EffectDescriptor values, so the
// built-in catalogue (BuiltinEffects.cpp, BuiltinVideoEffects.cpp) stays readable.
#pragma once

#include "core/EffectRegistry.h"

#include <QColor>

namespace kk::builders {

/// Double parameter (animatable).
inline ParamDescriptor number(const QString& key, const QString& label, double min, double max,
    double def, double step, const QString& unit = {})
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Double;
    p.min = min;
    p.max = max;
    p.def = def;
    p.step = step;
    p.unit = unit;
    return p;
}

/// Int parameter (animatable).
inline ParamDescriptor integer(
    const QString& key, const QString& label, int min, int max, int def, const QString& unit = {})
{
    ParamDescriptor p = number(key, label, min, max, def, 1.0, unit);
    p.type = ParamType::Int;
    return p;
}

/// Bool static.
inline ParamDescriptor boolean(const QString& key, const QString& label, bool def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Bool;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

/// Color static.
inline ParamDescriptor color(const QString& key, const QString& label, const QColor& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Color;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

/// Choice static: @p values are stored, @p labels shown.
inline ParamDescriptor choice(const QString& key, const QString& label, const QStringList& values,
    const QStringList& labels, const QString& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Choice;
    p.choices = values;
    p.choiceLabels = labels;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

/// String static.
inline ParamDescriptor text(const QString& key, const QString& label, const QString& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::String;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

/// Curve static (core/CurveParam.h text form); the identity curve by default.
inline ParamDescriptor curve(const QString& key, const QString& label, const QString& def)
{
    ParamDescriptor p = text(key, label, def);
    p.type = ParamType::Curve;
    return p;
}

/// File static (absolute path, empty by default); @p filter is the file dialog's filter.
inline ParamDescriptor file(const QString& key, const QString& label, const QString& filter)
{
    ParamDescriptor p = text(key, label, QString());
    p.type = ParamType::File;
    p.fileFilter = filter;
    return p;
}

/// Descriptor of @p id with @p params.
inline EffectDescriptor descriptor(const QString& id, const QString& name, EffectCategory category,
    const QString& description, QVector<ParamDescriptor> params, const QString& storage = {})
{
    EffectDescriptor d;
    d.id = id;
    d.name = name;
    d.category = category;
    d.description = description;
    d.storage = storage;
    d.params = std::move(params);
    return d;
}

} // namespace kk::builders
