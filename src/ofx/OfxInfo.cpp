// SPDX-License-Identifier: GPL-3.0-or-later
// OfxInfo — the description structs of OfxHost.h as JSON (the probe program prints them, the
// scan cache stores them).
#include "ofx/OfxHost.h"

#include <QDir>
#include <QJsonArray>

namespace kk::ofx {

namespace {

QJsonArray numbers(const QVector<double>& v)
{
    QJsonArray a;
    for (double d : v)
        a.append(d);
    return a;
}

QVector<double> numbersOf(const QJsonValue& v)
{
    QVector<double> out;
    for (const QJsonValue& x : v.toArray())
        out.append(x.toDouble());
    return out;
}

QJsonArray strings(const QStringList& l)
{
    return QJsonArray::fromStringList(l);
}

QStringList stringsOf(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& x : v.toArray())
        out.append(x.toString());
    return out;
}

} // namespace

QJsonObject ParamInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("name")] = name;
    o[QLatin1String("label")] = label;
    o[QLatin1String("hint")] = hint;
    o[QLatin1String("parent")] = parent;
    o[QLatin1String("type")] = type;
    o[QLatin1String("dims")] = dims;
    o[QLatin1String("def")] = numbers(def);
    o[QLatin1String("min")] = numbers(min);
    o[QLatin1String("max")] = numbers(max);
    o[QLatin1String("displayMin")] = numbers(displayMin);
    o[QLatin1String("displayMax")] = numbers(displayMax);
    o[QLatin1String("defString")] = defString;
    o[QLatin1String("choices")] = strings(choices);
    o[QLatin1String("choiceLabels")] = strings(choiceLabels);
    o[QLatin1String("increment")] = increment;
    o[QLatin1String("digits")] = digits;
    o[QLatin1String("doubleType")] = doubleType;
    o[QLatin1String("coordinates")] = coordinates;
    o[QLatin1String("stringMode")] = stringMode;
    o[QLatin1String("dimLabels")] = strings(dimLabels);
    o[QLatin1String("animates")] = animates;
    o[QLatin1String("secret")] = secret;
    o[QLatin1String("enabled")] = enabled;
    o[QLatin1String("evaluateOnChange")] = evaluateOnChange;
    o[QLatin1String("persistent")] = persistent;
    return o;
}

ParamInfo ParamInfo::fromJson(const QJsonObject& o)
{
    ParamInfo p;
    p.name = o[QLatin1String("name")].toString();
    p.label = o[QLatin1String("label")].toString();
    p.hint = o[QLatin1String("hint")].toString();
    p.parent = o[QLatin1String("parent")].toString();
    p.type = o[QLatin1String("type")].toString();
    p.dims = o[QLatin1String("dims")].toInt(1);
    p.def = numbersOf(o[QLatin1String("def")]);
    p.min = numbersOf(o[QLatin1String("min")]);
    p.max = numbersOf(o[QLatin1String("max")]);
    p.displayMin = numbersOf(o[QLatin1String("displayMin")]);
    p.displayMax = numbersOf(o[QLatin1String("displayMax")]);
    p.defString = o[QLatin1String("defString")].toString();
    p.choices = stringsOf(o[QLatin1String("choices")]);
    p.choiceLabels = stringsOf(o[QLatin1String("choiceLabels")]);
    p.increment = o[QLatin1String("increment")].toDouble(1.0);
    p.digits = o[QLatin1String("digits")].toInt(2);
    p.doubleType = o[QLatin1String("doubleType")].toString();
    p.coordinates = o[QLatin1String("coordinates")].toString();
    p.stringMode = o[QLatin1String("stringMode")].toString();
    p.dimLabels = stringsOf(o[QLatin1String("dimLabels")]);
    p.animates = o[QLatin1String("animates")].toBool();
    p.secret = o[QLatin1String("secret")].toBool();
    p.enabled = o[QLatin1String("enabled")].toBool(true);
    p.evaluateOnChange = o[QLatin1String("evaluateOnChange")].toBool(true);
    p.persistent = o[QLatin1String("persistent")].toBool(true);
    return p;
}

bool ParamInfo::isValueParam() const
{
    return type != QLatin1String("Group") && type != QLatin1String("Page")
        && type != QLatin1String("PushButton") && type != QLatin1String("Custom");
}

QJsonObject ClipInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("name")] = name;
    o[QLatin1String("optional")] = optional;
    o[QLatin1String("mask")] = mask;
    o[QLatin1String("temporal")] = temporal;
    return o;
}

ClipInfo ClipInfo::fromJson(const QJsonObject& o)
{
    ClipInfo c;
    c.name = o[QLatin1String("name")].toString();
    c.optional = o[QLatin1String("optional")].toBool();
    c.mask = o[QLatin1String("mask")].toBool();
    c.temporal = o[QLatin1String("temporal")].toBool();
    return c;
}

QJsonObject ContextInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("context")] = context;
    QJsonArray c, p;
    for (const ClipInfo& clip : clips)
        c.append(clip.toJson());
    for (const ParamInfo& param : params)
        p.append(param.toJson());
    o[QLatin1String("clips")] = c;
    o[QLatin1String("params")] = p;
    o[QLatin1String("usable")] = usable;
    o[QLatin1String("whyNot")] = whyNot;
    return o;
}

ContextInfo ContextInfo::fromJson(const QJsonObject& o)
{
    ContextInfo c;
    c.context = o[QLatin1String("context")].toString();
    for (const QJsonValue& v : o[QLatin1String("clips")].toArray())
        c.clips.append(ClipInfo::fromJson(v.toObject()));
    for (const QJsonValue& v : o[QLatin1String("params")].toArray())
        c.params.append(ParamInfo::fromJson(v.toObject()));
    c.usable = o[QLatin1String("usable")].toBool();
    c.whyNot = o[QLatin1String("whyNot")].toString();
    return c;
}

const ContextInfo* PluginInfo::usableContext() const
{
    for (const char* want : {"Filter", "General", "Transition"}) {
        for (const ContextInfo& c : contexts) {
            if (c.usable && c.context == QLatin1String(want))
                return &c;
        }
    }
    return nullptr;
}

bool PluginInfo::isTransition() const
{
    const ContextInfo* c = usableContext();
    return c && c->context == QLatin1String("Transition");
}

QJsonObject PluginInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("identifier")] = identifier;
    o[QLatin1String("label")] = label;
    o[QLatin1String("shortLabel")] = shortLabel;
    o[QLatin1String("group")] = group;
    o[QLatin1String("description")] = description;
    o[QLatin1String("versionLabel")] = versionLabel;
    o[QLatin1String("index")] = index;
    o[QLatin1String("major")] = major;
    o[QLatin1String("minor")] = minor;
    o[QLatin1String("pixelDepths")] = strings(pixelDepths);
    o[QLatin1String("threadSafety")] = threadSafety;
    o[QLatin1String("multiResolution")] = multiResolution;
    o[QLatin1String("tiles")] = tiles;
    o[QLatin1String("temporalAccess")] = temporalAccess;
    QJsonArray c;
    for (const ContextInfo& ctx : contexts)
        c.append(ctx.toJson());
    o[QLatin1String("contexts")] = c;
    o[QLatin1String("error")] = error;
    return o;
}

PluginInfo PluginInfo::fromJson(const QJsonObject& o)
{
    PluginInfo p;
    p.identifier = o[QLatin1String("identifier")].toString();
    p.label = o[QLatin1String("label")].toString();
    p.shortLabel = o[QLatin1String("shortLabel")].toString();
    p.group = o[QLatin1String("group")].toString();
    p.description = o[QLatin1String("description")].toString();
    p.versionLabel = o[QLatin1String("versionLabel")].toString();
    p.index = o[QLatin1String("index")].toInt();
    p.major = o[QLatin1String("major")].toInt(1);
    p.minor = o[QLatin1String("minor")].toInt();
    p.pixelDepths = stringsOf(o[QLatin1String("pixelDepths")]);
    p.threadSafety = o[QLatin1String("threadSafety")].toString();
    p.multiResolution = o[QLatin1String("multiResolution")].toBool();
    p.tiles = o[QLatin1String("tiles")].toBool();
    p.temporalAccess = o[QLatin1String("temporalAccess")].toBool();
    for (const QJsonValue& v : o[QLatin1String("contexts")].toArray())
        p.contexts.append(ContextInfo::fromJson(v.toObject()));
    p.error = o[QLatin1String("error")].toString();
    return p;
}

QJsonObject BundleInfo::toJson() const
{
    QJsonObject o;
    o[QLatin1String("bundlePath")] = bundlePath;
    o[QLatin1String("binary")] = binary;
    o[QLatin1String("error")] = error;
    QJsonArray a;
    for (const PluginInfo& p : plugins)
        a.append(p.toJson());
    o[QLatin1String("plugins")] = a;
    return o;
}

BundleInfo BundleInfo::fromJson(const QJsonObject& o)
{
    BundleInfo b;
    b.bundlePath = o[QLatin1String("bundlePath")].toString();
    b.binary = o[QLatin1String("binary")].toString();
    b.error = o[QLatin1String("error")].toString();
    for (const QJsonValue& v : o[QLatin1String("plugins")].toArray())
        b.plugins.append(PluginInfo::fromJson(v.toObject()));
    return b;
}

QString bundleBinary(const QString& bundleDir)
{
#if defined(__aarch64__)
    const QStringList archs = {QStringLiteral("Linux-arm-64"), QStringLiteral("Linux-arm64"),
        QStringLiteral("Linux-aarch64")};
#else
    const QStringList archs = {QStringLiteral("Linux-x86-64")};
#endif
    for (const QString& arch : archs) {
        const QDir dir(bundleDir + QStringLiteral("/Contents/") + arch);
        const QStringList files = dir.entryList({QStringLiteral("*.ofx")}, QDir::Files, QDir::Name);
        if (!files.isEmpty())
            return dir.filePath(files.first());
    }
    return {};
}

} // namespace kk::ofx
