// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: transition.list and effect.list. They only read the effect catalogue
// (core/EffectRegistry.h) so that a caller can see which ids `transition add --type` takes and
// which effects, generators and titles exist; they need no project.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/EffectRegistry.h"

namespace kk::commands {

namespace {

QString paramTypeName(kk::ParamType type)
{
    switch (type) {
    case kk::ParamType::Double:
        return QStringLiteral("number");
    case kk::ParamType::Int:
        return QStringLiteral("integer");
    case kk::ParamType::Bool:
        return QStringLiteral("boolean");
    case kk::ParamType::Color:
        return QStringLiteral("color");
    case kk::ParamType::Choice:
        return QStringLiteral("choice");
    case kk::ParamType::String:
        return QStringLiteral("string");
    case kk::ParamType::Curve:
        return QStringLiteral("curve");
    case kk::ParamType::File:
        return QStringLiteral("file");
    }
    return {};
}

QJsonObject paramToJson(const ParamDescriptor& p)
{
    QJsonObject o {{QStringLiteral("key"), p.key}, {QStringLiteral("label"), p.label},
        {QStringLiteral("type"), paramTypeName(p.type)}};
    if (p.isScalar()) {
        o.insert(QStringLiteral("min"), p.min);
        o.insert(QStringLiteral("max"), p.max);
        o.insert(QStringLiteral("default"), p.def);
        if (!p.unit.isEmpty())
            o.insert(QStringLiteral("unit"), p.unit);
    } else if (p.defStatic.isValid()) {
        o.insert(QStringLiteral("default"), p.defStatic.toString());
    }
    if (!p.choices.isEmpty())
        o.insert(QStringLiteral("choices"), QJsonArray::fromStringList(p.choices));
    return o;
}

QJsonObject descriptorToJson(const EffectDescriptor& d, bool withParams)
{
    QJsonObject o {{QStringLiteral("id"), d.id}, {QStringLiteral("name"), d.name},
        {QStringLiteral("category"), EffectRegistry::categoryName(d.category)},
        {QStringLiteral("description"), d.description}};
    if (!d.group.isEmpty())
        o.insert(QStringLiteral("folder"), EffectRegistry::groupName(d.group));
    if (withParams) {
        QJsonArray params;
        for (const ParamDescriptor& p : d.params)
            params.append(paramToJson(p));
        o.insert(QStringLiteral("parameters"), params);
    }
    return o;
}

/// The catalogue entries of @p categories (all when @p id is empty), or just the one named @p id.
CommandResult listDescriptors(
    const QVector<EffectCategory>& categories, const CommandArgs& args, const QString& what)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const QString id = args.string(QStringLiteral("id"));
    QJsonArray list;
    if (!id.isEmpty()) {
        const EffectDescriptor* d = registry.find(id);
        if (!d || !categories.contains(d->category))
            return fail("unknown_effect", QStringLiteral("no %1 with id \"%2\"").arg(what, id));
        list.append(descriptorToJson(*d, true));
    } else {
        for (const EffectDescriptor& d : registry.all()) {
            if (categories.contains(d.category))
                list.append(descriptorToJson(d, false));
        }
    }
    QString text = QStringLiteral("%1 %2(s)").arg(list.size()).arg(what);
    for (const QJsonValue& v : std::as_const(list)) {
        const QJsonObject o = v.toObject();
        text += QStringLiteral("\n  %1  %2")
                    .arg(o[QStringLiteral("id")].toString(), -28)
                    .arg(o[QStringLiteral("name")].toString());
    }
    return CommandResult::success({{QStringLiteral("items"), list}}, text);
}

CommandResult runTransitionList(CommandContext&, const CommandArgs& args)
{
    return listDescriptors({EffectCategory::Transition, EffectCategory::AudioTransition}, args,
        QStringLiteral("transition"));
}

CommandResult runEffectList(CommandContext&, const CommandArgs& args)
{
    const QString category = args.string(QStringLiteral("category"));
    QVector<EffectCategory> categories {EffectCategory::VideoEffect, EffectCategory::AudioEffect,
        EffectCategory::Generator, EffectCategory::Title, EffectCategory::Effects};
    if (category == QLatin1String("video"))
        categories = {EffectCategory::VideoEffect};
    else if (category == QLatin1String("audio"))
        categories = {EffectCategory::AudioEffect};
    else if (category == QLatin1String("generator"))
        categories = {EffectCategory::Generator};
    else if (category == QLatin1String("title"))
        categories = {EffectCategory::Title};
    return listDescriptors(categories, args, QStringLiteral("effect"));
}

const ParamSpec kIdParam {QStringLiteral("id"), ParamType::String,
    QStringLiteral("show only this id, with all its parameters (key, type, range, default, "
                   "choices)"),
    false, {}, {}};

} // namespace

void registerEffectCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("transition.list"),
                      QStringLiteral("List the video and audio transitions with their ids (the "
                                     "--type of transition add) and Effects library folders."),
                      {kIdParam}, ProjectUse::None, false},
        runTransitionList});
    registry.add({{QStringLiteral("effect.list"),
                      QStringLiteral("List the effects, generators and titles of the Effects "
                                     "library with their ids."),
                      {{QStringLiteral("category"), ParamType::String,
                           QStringLiteral("only this kind (default: all)"), false, {},
                           {QStringLiteral("video"), QStringLiteral("audio"),
                               QStringLiteral("generator"), QStringLiteral("title")}},
                          kIdParam},
                      ProjectUse::None, false},
        runEffectList});
}

} // namespace kk::commands
