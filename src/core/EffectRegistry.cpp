// SPDX-License-Identifier: GPL-3.0-or-later
// EffectRegistry lookup functions. The built-in catalogue is in BuiltinEffects.cpp.
#include "core/EffectRegistry.h"

#include <QCoreApplication>

namespace cw {

const ParamDescriptor* EffectDescriptor::param(const QString& key) const
{
    for (const ParamDescriptor& p : params) {
        if (p.key == key)
            return &p;
    }
    return nullptr;
}

EffectRegistry::EffectRegistry()
{
    registerBuiltins();
}

EffectRegistry& EffectRegistry::instance()
{
    static EffectRegistry registry; // thread-safe initialisation (C++11 magic statics)
    return registry;
}

void EffectRegistry::add(EffectDescriptor descriptor)
{
    if (const auto it = m_index.constFind(descriptor.id); it != m_index.cend()) {
        m_descriptors[*it] = std::move(descriptor);
        return;
    }
    m_index.insert(descriptor.id, m_descriptors.size());
    m_descriptors.append(std::move(descriptor));
}

const EffectDescriptor* EffectRegistry::find(const QString& id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.cend() ? nullptr : &m_descriptors[*it];
}

QVector<const EffectDescriptor*> EffectRegistry::byCategory(EffectCategory category) const
{
    QVector<const EffectDescriptor*> result;
    for (const EffectDescriptor& d : m_descriptors) {
        if (d.category == category)
            result.append(&d);
    }
    return result;
}

ParamSet EffectRegistry::defaultParams(const QString& id) const
{
    ParamSet params;
    const EffectDescriptor* d = find(id);
    if (!d)
        return params;
    for (const ParamDescriptor& p : d->params) {
        if (p.isScalar())
            params.setBase(p.key, p.def);
        else
            params.setStatic(p.key, p.defStatic);
    }
    return params;
}

EffectInstance EffectRegistry::createEffect(const QString& id, quint64 instanceId) const
{
    EffectInstance fx;
    fx.id = instanceId;
    fx.type = id;
    fx.params = defaultParams(id);
    return fx;
}

TransitionData EffectRegistry::createTransition(const QString& id, int64_t durationFrames) const
{
    TransitionData t;
    t.type = id;
    t.duration = durationFrames;
    t.params = defaultParams(id);
    return t;
}

QString EffectRegistry::categoryName(EffectCategory category)
{
    switch (category) {
    case EffectCategory::VideoEffect:
        return QCoreApplication::translate("cw::EffectRegistry", "Video Effects");
    case EffectCategory::AudioEffect:
        return QCoreApplication::translate("cw::EffectRegistry", "Audio Effects");
    case EffectCategory::Transition:
        return QCoreApplication::translate("cw::EffectRegistry", "Transitions");
    case EffectCategory::Generator:
        return QCoreApplication::translate("cw::EffectRegistry", "Generators");
    case EffectCategory::Title:
        return QCoreApplication::translate("cw::EffectRegistry", "Titles");
    case EffectCategory::ClipProperties:
        return QCoreApplication::translate("cw::EffectRegistry", "Clip Properties");
    }
    return {};
}

} // namespace cw
