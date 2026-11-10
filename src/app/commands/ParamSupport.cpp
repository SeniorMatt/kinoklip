// SPDX-License-Identifier: GPL-3.0-or-later
// ParamSupport implementation.
#include "app/commands/ParamSupport.h"

#include "app/commands/CommandSupport.h"
#include "core/Generators.h"
#include "core/TitlePresets.h"

#include <QColor>
#include <QFileInfo>

#include <cmath>

namespace kk::commands {

namespace {

constexpr double kRangeSlack = 1e-9; ///< a value this far outside the range still counts as inside

QString keyList(const EffectDescriptor& descriptor)
{
    QStringList keys;
    for (const ParamDescriptor& p : descriptor.params)
        keys.append(p.key);
    return keys.join(QStringLiteral(", "));
}

const ParamDescriptor* findIn(const EffectDescriptor* descriptor, const QString& key)
{
    return descriptor ? descriptor->param(key) : nullptr;
}

} // namespace

std::optional<CommandResult> checkParamValue(
    const ParamDescriptor& d, const QString& text, ParamSetting& out)
{
    out = {};
    out.descriptor = &d;
    const QString value = text.trimmed();
    switch (d.type) {
    case kk::ParamType::Double:
    case kk::ParamType::Int: {
        bool ok = false;
        const double n = value.toDouble(&ok);
        if (!ok || !std::isfinite(n))
            return fail(
                "bad_value", QStringLiteral("\"%1\": \"%2\" is not a number").arg(d.key, text));
        if (d.type == kk::ParamType::Int && n != std::floor(n))
            return fail("bad_value",
                QStringLiteral("\"%1\": \"%2\" is not a whole number").arg(d.key, text));
        if (n < d.min - kRangeSlack || n > d.max + kRangeSlack)
            return fail("out_of_range",
                QStringLiteral("\"%1\" must be between %2 and %3")
                    .arg(d.key)
                    .arg(d.min)
                    .arg(d.max));
        out.number = n;
        return std::nullopt;
    }
    case kk::ParamType::Bool: {
        const QString t = value.toLower();
        if (t == QLatin1String("true") || t == QLatin1String("yes") || t == QLatin1String("1"))
            out.value = true;
        else if (t == QLatin1String("false") || t == QLatin1String("no") || t == QLatin1String("0"))
            out.value = false;
        else
            return fail("bad_value",
                QStringLiteral("\"%1\": \"%2\" is not true or false").arg(d.key, text));
        return std::nullopt;
    }
    case kk::ParamType::Choice:
        if (!d.choices.contains(value))
            return fail("bad_value",
                QStringLiteral("\"%1\" must be one of: %2")
                    .arg(d.key, d.choices.join(QStringLiteral(", "))));
        out.value = value;
        return std::nullopt;
    case kk::ParamType::Color: {
        const QColor color = QColor::fromString(value);
        if (!color.isValid())
            return fail("bad_value",
                QStringLiteral("\"%1\": \"%2\" is not a colour (#rrggbb or a colour name)")
                    .arg(d.key, text));
        out.value = color;
        return std::nullopt;
    }
    case kk::ParamType::File: {
        const QString path = QFileInfo(value).absoluteFilePath();
        if (!QFileInfo(path).isFile())
            return fail("file_not_found", QStringLiteral("%1 does not exist").arg(path));
        out.value = path;
        return std::nullopt;
    }
    case kk::ParamType::String:
    case kk::ParamType::Curve:
        out.value = text; // not trimmed: a title may start with a space
        return std::nullopt;
    }
    return fail("bad_value", QStringLiteral("\"%1\" cannot be set").arg(d.key));
}

std::optional<CommandResult> checkSettings(
    const EffectDescriptor& descriptor, const QStringList& settings, QVector<ParamSetting>& out)
{
    for (const QString& entry : settings) {
        const qsizetype eq = entry.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return fail("bad_value", QStringLiteral("\"%1\" is not key=value").arg(entry));
        const QString key = entry.left(eq).trimmed();
        const ParamDescriptor* p = descriptor.param(key);
        if (!p)
            return fail("unknown_parameter",
                QStringLiteral("\"%1\" has no parameter \"%2\" (it has: %3)")
                    .arg(descriptor.id, key, keyList(descriptor)));
        ParamSetting setting;
        if (auto bad = checkParamValue(*p, entry.mid(eq + 1), setting))
            return bad;
        out.append(setting);
    }
    return std::nullopt;
}

void applySettings(ClipData& clip, const ParamRef& base,
    const QVector<ParamSetting>& settings, int64_t localFrame)
{
    for (const ParamSetting& s : settings) {
        ParamRef ref = base;
        ref.key = s.descriptor->key;
        if (s.descriptor->isScalar())
            paramaccess::setScalar(clip, ref, *s.descriptor, localFrame, s.number);
        else
            paramaccess::setStatic(clip, ref, s.value);
    }
}

QStringList settingsOf(const CommandArgs& args)
{
    QStringList out = args.strings(QStringLiteral("set"));
    if (args.has(QStringLiteral("key")) || args.has(QStringLiteral("value")))
        out.append(args.string(QStringLiteral("key")) + QLatin1Char('=')
            + args.string(QStringLiteral("value")));
    return out;
}

std::optional<CommandResult> resolveParam(
    const ClipData& clip, quint64 effectId, const QString& key, ResolvedParam& out)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    if (effectId != 0) {
        const EffectInstance* fx = paramaccess::effect(clip, effectId);
        if (!fx)
            return fail("unknown_effect",
                QStringLiteral("clip %1 has no effect instance %2 (see effect.stack)")
                    .arg(clip.id)
                    .arg(effectId));
        const EffectDescriptor* d = registry.find(fx->type);
        const ParamDescriptor* p = findIn(d, key);
        if (!p)
            return fail("unknown_parameter",
                QStringLiteral("effect %1 has no parameter \"%2\"%3")
                    .arg(fx->type, key,
                        d ? QStringLiteral(" (it has: ") + keyList(*d) + QLatin1Char(')')
                          : QString()));
        out.ref = {clip.id, ParamScope::Effect, effectId, key};
        out.descriptor = p;
        out.owner = d;
        return std::nullopt;
    }
    for (const char* id : {groups::Transform, groups::Cropping, groups::Composite}) {
        const EffectDescriptor* d = registry.find(QLatin1String(id));
        if (const ParamDescriptor* p = findIn(d, key)) {
            out.ref = {clip.id, ParamScope::Transform, 0, key};
            out.descriptor = p;
            out.owner = d;
            return std::nullopt;
        }
    }
    if (const EffectDescriptor* d = registry.find(QLatin1String(groups::Audio))) {
        if (const ParamDescriptor* p = findIn(d, key)) {
            out.ref = {clip.id, ParamScope::Audio, 0, key};
            out.descriptor = p;
            out.owner = d;
            return std::nullopt;
        }
    }
    const EffectDescriptor* own = nullptr;
    if (clip.kind == ClipKind::Title)
        own = titles::descriptorOf(clip);
    else if (clip.kind == ClipKind::Generator)
        own = registry.find(clip.generator);
    if (const ParamDescriptor* p = findIn(own, key)) {
        out.ref = {clip.id, ParamScope::Title, 0, key};
        out.descriptor = p;
        out.owner = own;
        return std::nullopt;
    }
    return fail("unknown_parameter",
        QStringLiteral("clip %1 has no parameter \"%2\" (clip.set and keyframe.list show the "
                       "names; for an effect give its instance id)")
            .arg(clip.id)
            .arg(key));
}

QJsonValue paramValueJson(const ClipData& clip, const ParamRef& ref,
    const ParamDescriptor& d, int64_t localFrame)
{
    if (d.isScalar())
        return paramaccess::scalar(clip, ref, d, localFrame);
    const QVariant v = paramaccess::staticValue(clip, ref, d);
    switch (d.type) {
    case kk::ParamType::Bool:
        return v.toBool();
    case kk::ParamType::Color:
        return v.value<QColor>().name(QColor::HexArgb);
    default:
        return v.toString();
    }
}

ParamSpec setListParam(const char* what)
{
    return {QStringLiteral("set"), ParamType::StringList,
        QStringLiteral("%1 parameters as key=value (repeat the option, or give a list); keys, "
                       "ranges and choices are shown by effect.list --id")
            .arg(QString::fromLatin1(what)),
        false, {}, {}};
}

} // namespace kk::commands
