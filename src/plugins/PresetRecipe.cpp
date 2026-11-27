// SPDX-License-Identifier: GPL-3.0-or-later
// PresetRecipe — see PresetRecipe.h.
#include "plugins/PresetRecipe.h"

#include <QJsonDocument>
#include <QRegularExpression>

namespace kk::plugins {

namespace {

constexpr int kMaxSteps = 200;

QJsonValue expandValue(const QJsonValue& v, const RecipeTarget& t);

QString numberText(qint64 n)
{
    return QString::number(n);
}

QJsonValue expandString(const QString& text, const RecipeTarget& t)
{
    // Exactly one placeholder, with an optional offset: a number.
    static const QRegularExpression exact(
        QStringLiteral("^\\$(clip|start|end|duration|playhead|left|right|edit)(?:([+-])(\\d+))?$"));
    const auto m = exact.match(text);
    const auto valueOf = [&t](const QString& name) -> qint64 {
        if (name == QLatin1String("clip"))
            return t.clip;
        if (name == QLatin1String("start"))
            return t.start;
        if (name == QLatin1String("end"))
            return t.end;
        if (name == QLatin1String("duration"))
            return t.duration;
        if (name == QLatin1String("left"))
            return t.left;
        if (name == QLatin1String("right"))
            return t.right;
        if (name == QLatin1String("edit"))
            return t.edit;
        return t.playhead;
    };
    if (m.hasMatch()) {
        qint64 n = valueOf(m.captured(1));
        if (!m.captured(2).isEmpty()) {
            const qint64 offset = m.captured(3).toLongLong();
            n += m.captured(2) == QLatin1String("-") ? -offset : offset;
        }
        return double(n);
    }
    if (!text.contains(QLatin1Char('$')))
        return text;
    QString out = text;
    for (const char* name : {"duration", "playhead", "clip", "start", "end", "left", "right", "edit"}) {
        out.replace(
            QStringLiteral("$") + QLatin1String(name), numberText(valueOf(QLatin1String(name))));
    }
    return out;
}

QJsonValue expandValue(const QJsonValue& v, const RecipeTarget& t)
{
    if (v.isString())
        return expandString(v.toString(), t);
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue& e : v.toArray())
            out.append(expandValue(e, t));
        return out;
    }
    if (v.isObject()) {
        QJsonObject out;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            out.insert(it.key(), expandValue(it.value(), t));
        return out;
    }
    return v;
}

QJsonValue replaceLast(const QJsonValue& v, qint64 id)
{
    if (v.isString()) {
        if (v.toString() == QLatin1String("$last_effect"))
            return double(id);
        return v;
    }
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue& e : v.toArray())
            out.append(replaceLast(e, id));
        return out;
    }
    if (v.isObject()) {
        QJsonObject out;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            out.insert(it.key(), replaceLast(it.value(), id));
        return out;
    }
    return v;
}

bool mentionsLast(const QJsonValue& v)
{
    if (v.isString())
        return v.toString() == QLatin1String("$last_effect");
    if (v.isArray()) {
        for (const QJsonValue& e : v.toArray()) {
            if (mentionsLast(e))
                return true;
        }
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (mentionsLast(it.value()))
                return true;
        }
    }
    return false;
}

} // namespace

QVector<RecipeStep> expandRecipe(
    const QJsonObject& recipe, const RecipeTarget& target, QString* error)
{
    QVector<RecipeStep> steps;
    const QJsonValue list = recipe.value(QLatin1String("commands"));
    if (!list.isArray()) {
        if (error)
            *error = QStringLiteral("the recipe has no \"commands\" list");
        return {};
    }
    const QJsonArray array = list.toArray();
    if (array.isEmpty() || array.size() > kMaxSteps) {
        if (error)
            *error = QStringLiteral("a recipe has 1 to %1 commands").arg(kMaxSteps);
        return {};
    }
    // An edit point can lack a clip on one side: say so instead of sending clip 0 to a command.
    if (target.edgePoint) {
        const QByteArray text = QJsonDocument(recipe).toJson(QJsonDocument::Compact);
        for (const auto& side : {std::pair<const char*, qint64>{"$left", target.left},
                 std::pair<const char*, qint64>{"$right", target.right}}) {
            if (side.second == 0 && text.contains(side.first)) {
                if (error)
                    *error = QStringLiteral("this edit point has no clip on its %1 side")
                                 .arg(QLatin1String(side.first + 1));
                return {};
            }
        }
    }
    int i = 0;
    for (const QJsonValue& v : array) {
        const QJsonObject o = v.toObject();
        const QString command = o.value(QLatin1String("command")).toString();
        const QJsonValue args = o.value(QLatin1String("args"));
        if (command.isEmpty() || (!args.isUndefined() && !args.isObject())) {
            if (error)
                *error = QStringLiteral("commands[%1] needs a \"command\" name and an \"args\" "
                                        "object")
                             .arg(i);
            return {};
        }
        RecipeStep step;
        step.command = command;
        step.args = expandValue(o.value(QLatin1String("args")).toObject(), target).toObject();
        steps.append(step);
        ++i;
    }
    return steps;
}

QJsonObject substituteResult(const QJsonObject& args, qint64 effectId)
{
    return replaceLast(args, effectId).toObject();
}

bool usesLastEffect(const QJsonObject& args)
{
    return mentionsLast(args);
}

} // namespace kk::plugins
