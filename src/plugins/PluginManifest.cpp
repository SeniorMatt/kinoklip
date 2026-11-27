// SPDX-License-Identifier: GPL-3.0-or-later
// PluginManifest — parser and validator of plugin.json (see PluginManifest.h).
#include "plugins/PluginManifest.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <functional>

namespace kk::plugins {

namespace {

constexpr int kMaxParams = 32; ///< per effect or transition (uniform budget of small GPUs)
constexpr int kMaxItems = 200; ///< per kind and plugin
constexpr int kMaxChoices = 64;

/// Collects problems while walking the manifest.
class Walker
{
public:
    QVector<Problem> problems;

    void add(const QString& where, const QString& message) { problems.append({where, message, 0}); }

    /// Required string member; empty when absent or not a string (a problem is recorded).
    QString requiredString(const QJsonObject& o, const QString& path, const char* key)
    {
        const QString where = join(path, QLatin1String(key));
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined()) {
            add(where, QStringLiteral("is required"));
            return {};
        }
        if (!v.isString() || v.toString().trimmed().isEmpty()) {
            add(where, QStringLiteral("must be a non-empty string"));
            return {};
        }
        return v.toString().trimmed();
    }

    QString optionalString(const QJsonObject& o, const QString& path, const char* key)
    {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull())
            return {};
        if (!v.isString()) {
            add(join(path, QLatin1String(key)), QStringLiteral("must be a string"));
            return {};
        }
        return v.toString();
    }

    static QString join(const QString& path, const QString& key)
    {
        return path.isEmpty() ? key : path + QLatin1Char('.') + key;
    }
};

bool matches(const QString& text, const char* pattern)
{
    // Built per call: manifests are small and this stays safe from any thread.
    return QRegularExpression(QString::fromLatin1(pattern)).match(text).hasMatch();
}

bool readNumber(const QJsonObject& o, const char* key, double* out)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble())
        return false;
    *out = v.toDouble();
    return std::isfinite(*out);
}

void parseParam(Walker& w, const QJsonObject& o, const QString& path, PluginParam& p)
{
    p.key = w.requiredString(o, path, "key");
    if (!p.key.isEmpty() && !isValidParamKey(p.key))
        w.add(path + QStringLiteral(".key"),
            QStringLiteral("\"%1\" is not a valid key: start with a letter, then letters and "
                           "digits only (it becomes the shader uniform u_%1)")
                .arg(p.key));
    p.label = w.optionalString(o, path, "label");
    if (p.label.isEmpty())
        p.label = p.key;
    p.tooltip = w.optionalString(o, path, "tooltip");
    p.unit = w.optionalString(o, path, "unit");
    const QString type = w.requiredString(o, path, "type");
    if (!o.value(QLatin1String("keyframable")).isUndefined()) {
        if (!o.value(QLatin1String("keyframable")).isBool())
            w.add(path + QStringLiteral(".keyframable"), QStringLiteral("must be true or false"));
        else
            p.keyframable = o.value(QLatin1String("keyframable")).toBool();
    }
    const QJsonValue def = o.value(QLatin1String("default"));
    if (type == QLatin1String("number") || type == QLatin1String("integer")) {
        p.type = type == QLatin1String("number") ? ParamKind::Number : ParamKind::Integer;
        double min = 0.0;
        double max = 1.0;
        double value = 0.0;
        double step = p.type == ParamKind::Integer ? 1.0 : 0.01;
        if (!readNumber(o, "min", &min))
            w.add(
                path + QStringLiteral(".min"), QStringLiteral("is required and must be a number"));
        if (!readNumber(o, "max", &max))
            w.add(
                path + QStringLiteral(".max"), QStringLiteral("is required and must be a number"));
        if (!readNumber(o, "default", &value))
            w.add(path + QStringLiteral(".default"),
                QStringLiteral("is required and must be a number"));
        if (!o.value(QLatin1String("step")).isUndefined()
            && (!readNumber(o, "step", &step) || step <= 0.0))
            w.add(path + QStringLiteral(".step"), QStringLiteral("must be a number above 0"));
        if (min >= max)
            w.add(path + QStringLiteral(".min"), QStringLiteral("must be smaller than max"));
        else if (value < min || value > max)
            w.add(path + QStringLiteral(".default"),
                QStringLiteral("%1 is outside min..max (%2..%3)").arg(value).arg(min).arg(max));
        if (p.type == ParamKind::Integer
            && (min != std::floor(min) || max != std::floor(max) || value != std::floor(value)))
            w.add(path,
                QStringLiteral("an integer parameter needs whole numbers for min, max "
                               "and default (use type \"number\")"));
        p.min = min;
        p.max = max;
        p.def = value;
        p.step = step;
    } else if (type == QLatin1String("boolean")) {
        p.type = ParamKind::Boolean;
        p.keyframable = false;
        if (def.isUndefined())
            w.add(path + QStringLiteral(".default"), QStringLiteral("is required (true or false)"));
        else if (!def.isBool())
            w.add(path + QStringLiteral(".default"), QStringLiteral("must be true or false"));
        else
            p.boolDefault = def.toBool();
    } else if (type == QLatin1String("color")) {
        p.type = ParamKind::Color;
        p.keyframable = false;
        const QColor c = QColor::fromString(def.toString());
        if (!def.isString() || !c.isValid())
            w.add(path + QStringLiteral(".default"),
                QStringLiteral("is required: a colour like \"#ff8800\" or \"#80ff8800\" "
                               "(alpha first)"));
        else
            p.colorDefault = c;
    } else if (type == QLatin1String("choice")) {
        p.type = ParamKind::Choice;
        p.keyframable = false;
        const QJsonValue list = o.value(QLatin1String("choices"));
        if (!list.isArray() || list.toArray().isEmpty()) {
            w.add(path + QStringLiteral(".choices"),
                QStringLiteral("is required: a list of strings or {\"value\",\"label\"} objects"));
        } else if (list.toArray().size() > kMaxChoices) {
            w.add(path + QStringLiteral(".choices"),
                QStringLiteral("has more than %1 entries").arg(kMaxChoices));
        } else {
            int i = 0;
            for (const QJsonValue& c : list.toArray()) {
                QString value;
                QString label;
                if (c.isString()) {
                    value = label = c.toString();
                } else if (c.isObject()) {
                    value = c.toObject().value(QLatin1String("value")).toString();
                    label = c.toObject().value(QLatin1String("label")).toString(value);
                }
                if (value.isEmpty())
                    w.add(QStringLiteral("%1.choices[%2]").arg(path).arg(i),
                        QStringLiteral("needs a non-empty value"));
                else if (p.choices.contains(value))
                    w.add(QStringLiteral("%1.choices[%2]").arg(path).arg(i),
                        QStringLiteral("value \"%1\" appears twice").arg(value));
                else {
                    p.choices.append(value);
                    p.choiceLabels.append(label);
                }
                ++i;
            }
            const QString d = def.toString();
            if (!def.isString() || !p.choices.contains(d))
                w.add(path + QStringLiteral(".default"),
                    QStringLiteral("is required and must be one of the choices"));
            else
                p.stringDefault = d;
        }
    } else if (type == QLatin1String("string")) {
        p.type = ParamKind::String;
        p.keyframable = false;
        p.stringDefault = def.toString();
    } else if (!type.isEmpty()) {
        w.add(path + QStringLiteral(".type"),
            QStringLiteral("\"%1\" is not a parameter type (number, integer, boolean, color, "
                           "choice, string)")
                .arg(type));
    }
}

void parseParams(
    Walker& w, const QJsonObject& item, const QString& path, QVector<PluginParam>& out, bool shader)
{
    const QJsonValue list = item.value(QLatin1String("params"));
    if (list.isUndefined())
        return;
    if (!list.isArray()) {
        w.add(path + QStringLiteral(".params"), QStringLiteral("must be a list"));
        return;
    }
    const QJsonArray array = list.toArray();
    if (array.size() > kMaxParams)
        w.add(path + QStringLiteral(".params"),
            QStringLiteral("has %1 entries; at most %2").arg(array.size()).arg(kMaxParams));
    QSet<QString> keys;
    for (int i = 0; i < array.size() && i < kMaxParams; ++i) {
        const QString where = QStringLiteral("%1.params[%2]").arg(path).arg(i);
        if (!array[i].isObject()) {
            w.add(where, QStringLiteral("must be an object"));
            continue;
        }
        PluginParam p;
        parseParam(w, array[i].toObject(), where, p);
        if (!p.key.isEmpty() && keys.contains(p.key))
            w.add(
                where + QStringLiteral(".key"), QStringLiteral("\"%1\" is used twice").arg(p.key));
        keys.insert(p.key);
        if (shader && p.type == ParamKind::String)
            w.add(where + QStringLiteral(".type"),
                QStringLiteral("a shader cannot read a string parameter; use choice"));
        out.append(p);
    }
}

/// Common fields of effects, transitions, presets and scripts: id, name, description.
template<typename Def>
void parseItemHead(Walker& w, const QJsonObject& o, const QString& path, Def& d, QSet<QString>& ids)
{
    d.id = w.requiredString(o, path, "id");
    if (!d.id.isEmpty() && !isValidItemId(d.id))
        w.add(path + QStringLiteral(".id"),
            QStringLiteral("\"%1\" is not a valid id: lower case letters, digits and _ only")
                .arg(d.id));
    else if (!d.id.isEmpty() && ids.contains(d.id))
        w.add(path + QStringLiteral(".id"), QStringLiteral("\"%1\" is used twice").arg(d.id));
    ids.insert(d.id);
    d.name = w.requiredString(o, path, "name");
    d.description = w.optionalString(o, path, "description");
}

void checkFile(Walker& w, const QString& path, const QString& file, const char* suffixes)
{
    if (file.isEmpty())
        return;
    if (!isSafeRelativePath(file)) {
        w.add(path + QStringLiteral(".file"),
            QStringLiteral("\"%1\" must be a relative path inside the plugin folder (no \"..\")")
                .arg(file));
        return;
    }
    bool ok = false;
    for (const QString& s : QString::fromLatin1(suffixes).split(QLatin1Char(' ')))
        ok |= file.endsWith(s, Qt::CaseInsensitive);
    if (!ok)
        w.add(path + QStringLiteral(".file"),
            QStringLiteral("\"%1\" must end in %2").arg(file, QString::fromLatin1(suffixes)));
}

template<typename Def>
void parseList(Walker& w, const QJsonObject& root, const char* key,
    const std::function<void(const QJsonObject&, const QString&, Def&, QSet<QString>&)>& one,
    QVector<Def>& out)
{
    const QJsonValue list = root.value(QLatin1String(key));
    if (list.isUndefined())
        return;
    if (!list.isArray()) {
        w.add(QLatin1String(key), QStringLiteral("must be a list"));
        return;
    }
    const QJsonArray array = list.toArray();
    if (array.size() > kMaxItems)
        w.add(QLatin1String(key), QStringLiteral("has more than %1 entries").arg(kMaxItems));
    QSet<QString> ids;
    for (int i = 0; i < array.size() && i < kMaxItems; ++i) {
        const QString where = QStringLiteral("%1[%2]").arg(QLatin1String(key)).arg(i);
        if (!array[i].isObject()) {
            w.add(where, QStringLiteral("must be an object"));
            continue;
        }
        Def d;
        one(array[i].toObject(), where, d, ids);
        out.append(d);
    }
}

int lineOfOffset(const QByteArray& text, int offset)
{
    int line = 1;
    for (int i = 0; i < offset && i < text.size(); ++i)
        line += text[i] == '\n' ? 1 : 0;
    return line;
}

} // namespace

QString Problem::text() const
{
    QString s = where;
    if (warning)
        s = QStringLiteral("warning ") + s;
    if (line > 0)
        s += QStringLiteral(" (line %1)").arg(line);
    return s.isEmpty() ? message : s + QStringLiteral(": ") + message;
}

bool isValidPluginId(const QString& id)
{
    return matches(id, "^[a-z0-9][a-z0-9._-]{1,63}$");
}

bool isValidItemId(const QString& id)
{
    return matches(id, "^[a-z0-9_]{1,48}$");
}

bool isValidParamKey(const QString& key)
{
    return matches(key, "^[a-zA-Z][a-zA-Z0-9]{0,31}$");
}

QString fullId(const QString& pluginId, const QString& itemId)
{
    return QLatin1String(kIdPrefix) + pluginId + QLatin1Char('/') + itemId;
}

bool splitFullId(const QString& id, QString* pluginId, QString* itemId)
{
    if (!id.startsWith(QLatin1String(kIdPrefix)))
        return false;
    const QString rest = id.mid(int(sizeof(kIdPrefix)) - 1);
    const qsizetype slash = rest.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0 || slash == rest.size() - 1)
        return false;
    if (pluginId)
        *pluginId = rest.left(slash);
    if (itemId)
        *itemId = rest.mid(slash + 1);
    return true;
}

bool isSafeRelativePath(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))
        || path.contains(QLatin1Char(':')) || path.contains(QLatin1Char('\0')))
        return false;
    for (const QString& part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String("..") || part == QLatin1String("."))
            return false;
    }
    return true;
}

QStringList knownPermissions()
{
    return {QStringLiteral("project.read"), QStringLiteral("project.edit"),
        QStringLiteral("files.read"), QStringLiteral("files.write"), QStringLiteral("network"),
        QStringLiteral("processes")};
}

QString permissionText(const QString& permission)
{
    // The first two are enforced by Kinoklip's command bridge; the others are declarations the
    // person sees before enabling the plugin (a script is a program: it can do what you can).
    if (permission == QLatin1String("project.read"))
        return QCoreApplication::translate("kk::plugins", "Read the open project");
    if (permission == QLatin1String("project.edit"))
        return QCoreApplication::translate("kk::plugins", "Change the open project");
    if (permission == QLatin1String("files.read"))
        return QCoreApplication::translate("kk::plugins", "Read files on this computer");
    if (permission == QLatin1String("files.write"))
        return QCoreApplication::translate("kk::plugins", "Write files on this computer");
    if (permission == QLatin1String("network"))
        return QCoreApplication::translate("kk::plugins", "Use the network");
    if (permission == QLatin1String("processes"))
        return QCoreApplication::translate("kk::plugins", "Start other programs");
    return permission;
}

QStringList PluginManifest::kinds() const
{
    QStringList k;
    if (!effects.isEmpty())
        k << QStringLiteral("effect");
    if (!transitions.isEmpty())
        k << QStringLiteral("transition");
    if (!presets.isEmpty())
        k << QStringLiteral("preset");
    if (!scripts.isEmpty())
        k << QStringLiteral("script");
    return k;
}

QStringList PluginManifest::files() const
{
    QStringList f;
    for (const auto& e : effects)
        f << e.file;
    for (const auto& t : transitions)
        f << t.file;
    for (const auto& p : presets)
        f << p.file;
    for (const auto& s : scripts)
        f << s.file;
    return f;
}

ParseResult parseManifest(const QByteArray& json)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull()) {
        result.problems.append({QString::fromLatin1(kManifestFile),
            QStringLiteral("not valid JSON: %1").arg(parseError.errorString()),
            lineOfOffset(json, parseError.offset)});
        return result;
    }
    if (!doc.isObject()) {
        result.problems.append({QString::fromLatin1(kManifestFile),
            QStringLiteral("the top level must be a JSON object { ... }"), 1});
        return result;
    }
    const QJsonObject root = doc.object();
    Walker w;
    PluginManifest& m = result.manifest;
    m.raw = root;
    m.id = w.requiredString(root, {}, "id");
    if (!m.id.isEmpty() && !isValidPluginId(m.id))
        w.add(QStringLiteral("id"),
            QStringLiteral("\"%1\" is not a valid plugin id: 2 to 64 characters, lower case "
                           "letters, digits, '.', '-' or '_', for example com.example.tint")
                .arg(m.id));
    m.name = w.requiredString(root, {}, "name");
    m.version = w.requiredString(root, {}, "version");
    if (!m.version.isEmpty()
        && !matches(m.version, "^[0-9]+\\.[0-9]+(\\.[0-9]+)?([-+][0-9A-Za-z.-]+)?$"))
        w.add(QStringLiteral("version"),
            QStringLiteral("\"%1\" is not a version like 1.0 or 1.2.3").arg(m.version));
    m.author = w.requiredString(root, {}, "author");
    m.license = w.requiredString(root, {}, "license");
    m.description = w.optionalString(root, {}, "description");
    m.homepage = w.optionalString(root, {}, "homepage");
    if (!root.value(QLatin1String("api")).isDouble()
        || root.value(QLatin1String("api")).toInt() < 1)
        w.add(QStringLiteral("api"),
            QStringLiteral("is required: the plugin API version, %1 for this Kinoklip")
                .arg(kApiVersion));
    else if (root.value(QLatin1String("api")).toInt() > kApiVersion)
        w.add(QStringLiteral("api"),
            QStringLiteral("the plugin needs plugin API %1, this Kinoklip has %2: update Kinoklip")
                .arg(root.value(QLatin1String("api")).toInt())
                .arg(kApiVersion));
    else
        m.api = root.value(QLatin1String("api")).toInt();

    const QJsonValue perms = root.value(QLatin1String("permissions"));
    if (!perms.isUndefined()) {
        if (!perms.isArray()) {
            w.add(QStringLiteral("permissions"), QStringLiteral("must be a list of strings"));
        } else {
            for (const QJsonValue& v : perms.toArray()) {
                if (!v.isString() || !knownPermissions().contains(v.toString())) {
                    w.add(QStringLiteral("permissions"),
                        QStringLiteral("\"%1\" is not a permission (%2)")
                            .arg(v.toString(), knownPermissions().join(QStringLiteral(", "))));
                } else if (!m.permissions.contains(v.toString())) {
                    m.permissions.append(v.toString());
                }
            }
        }
    }

    parseList<PluginEffectDef>(
        w, root, "effects",
        [&w](const QJsonObject& o, const QString& path, PluginEffectDef& d, QSet<QString>& ids) {
            parseItemHead(w, o, path, d, ids);
            d.group = w.optionalString(o, path, "group");
            d.file = w.requiredString(o, path, "file");
            checkFile(w, path, d.file, ".glsl .frag");
            d.entry = w.optionalString(o, path, "entry");
            if (d.entry.isEmpty())
                d.entry = QStringLiteral("main");
            if (d.entry != QLatin1String("main") && d.entry != QLatin1String("color"))
                w.add(path + QStringLiteral(".entry"),
                    QStringLiteral("\"%1\" is not an entry kind (main, color)").arg(d.entry));
            d.portedFrom = w.optionalString(o, path, "ported_from");
            double pad = 0.0;
            if (readNumber(o, "padding_px", &pad)) {
                if (pad < 0.0 || pad > 2000.0)
                    w.add(path + QStringLiteral(".padding_px"),
                        QStringLiteral("must be between 0 and 2000"));
                else
                    d.paddingPx = pad;
            }
            parseParams(w, o, path, d.params, true);
        },
        m.effects);
    parseList<PluginTransitionDef>(
        w, root, "transitions",
        [&w](
            const QJsonObject& o, const QString& path, PluginTransitionDef& d, QSet<QString>& ids) {
            parseItemHead(w, o, path, d, ids);
            d.group = w.optionalString(o, path, "group");
            d.file = w.requiredString(o, path, "file");
            checkFile(w, path, d.file, ".glsl .frag");
            d.portedFrom = w.optionalString(o, path, "ported_from");
            parseParams(w, o, path, d.params, true);
        },
        m.transitions);
    parseList<PluginPresetDef>(
        w, root, "presets",
        [&w](const QJsonObject& o, const QString& path, PluginPresetDef& d, QSet<QString>& ids) {
            parseItemHead(w, o, path, d, ids);
            d.group = w.optionalString(o, path, "group");
            d.file = w.requiredString(o, path, "file");
            checkFile(w, path, d.file, ".json");
            const QString target = w.optionalString(o, path, "target");
            if (target.isEmpty() || target == QLatin1String("clip"))
                d.target = PresetTarget::Clip;
            else if (target == QLatin1String("playhead"))
                d.target = PresetTarget::Playhead;
            else if (target == QLatin1String("edit_point"))
                d.target = PresetTarget::EditPoint;
            else
                w.add(path + QStringLiteral(".target"),
                    QStringLiteral("\"%1\" is not a target (clip, playhead, edit_point)")
                        .arg(target));
        },
        m.presets);
    parseList<PluginScriptDef>(
        w, root, "scripts",
        [&w](const QJsonObject& o, const QString& path, PluginScriptDef& d, QSet<QString>& ids) {
            parseItemHead(w, o, path, d, ids);
            d.file = w.requiredString(o, path, "file");
            checkFile(w, path, d.file, ".py");
            d.interpreter = w.optionalString(o, path, "interpreter");
            if (d.interpreter.isEmpty())
                d.interpreter = QStringLiteral("python3");
            if (d.interpreter != QLatin1String("python3"))
                w.add(path + QStringLiteral(".interpreter"),
                    QStringLiteral("\"%1\" is not supported (python3)").arg(d.interpreter));
            const QJsonValue p = o.value(QLatin1String("permissions"));
            if (!p.isUndefined() && !p.isArray())
                w.add(path + QStringLiteral(".permissions"), QStringLiteral("must be a list"));
            for (const QJsonValue& v : p.toArray()) {
                if (!v.isString() || !knownPermissions().contains(v.toString()))
                    w.add(path + QStringLiteral(".permissions"),
                        QStringLiteral("\"%1\" is not a permission (%2)")
                            .arg(v.toString(), knownPermissions().join(QStringLiteral(", "))));
                else
                    d.permissions.append(v.toString());
            }
        },
        m.scripts);

    if (m.effects.isEmpty() && m.transitions.isEmpty() && m.presets.isEmpty() && m.scripts.isEmpty()
        && w.problems.isEmpty())
        w.add(QStringLiteral("effects"),
            QStringLiteral("the plugin contains nothing: add at least one of effects, "
                           "transitions, presets, scripts"));
    // Scripts need their permissions declared on the plugin so the person sees them in one place.
    for (PluginScriptDef& s : m.scripts) {
        for (const QString& p : std::as_const(s.permissions)) {
            if (!m.permissions.contains(p))
                m.permissions.append(p);
        }
    }
    if (!m.id.isEmpty()) {
        for (auto& e : m.effects)
            e.fullId = fullId(m.id, e.id);
        for (auto& t : m.transitions)
            t.fullId = fullId(m.id, t.id);
        for (auto& p : m.presets)
            p.fullId = fullId(m.id, p.id);
        for (auto& s : m.scripts)
            s.fullId = fullId(m.id, s.id);
    }
    result.problems = w.problems;
    result.ok = w.problems.isEmpty();
    return result;
}

} // namespace kk::plugins
