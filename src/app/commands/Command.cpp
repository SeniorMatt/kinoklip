// SPDX-License-Identifier: GPL-3.0-or-later
// Command layer core: argument checking, project loading and saving around every command.
#include "app/commands/Command.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/TimecodeFormat.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonValue>

#include <cmath>

namespace kk::commands {

namespace {

QString typeName(ParamType type)
{
    switch (type) {
    case ParamType::String:
        return QStringLiteral("string");
    case ParamType::Path:
        return QStringLiteral("path");
    case ParamType::Integer:
        return QStringLiteral("integer");
    case ParamType::Number:
        return QStringLiteral("number");
    case ParamType::Boolean:
        return QStringLiteral("boolean");
    case ParamType::Time:
        return QStringLiteral("time");
    case ParamType::StringList:
        return QStringLiteral("string-list");
    }
    return {};
}

/// Converts @p value to the type of @p param. std::nullopt (with @p why) if it does not fit.
std::optional<QJsonValue> coerce(const ParamSpec& param, const QJsonValue& value, QString* why)
{
    const bool isString = value.isString();
    const QString text = isString ? value.toString() : QString();
    switch (param.type) {
    case ParamType::String:
    case ParamType::Path: {
        if (isString)
            return value;
        if (value.isDouble())
            return QJsonValue(QString::number(value.toDouble(), 'g', 15));
        if (value.isBool()) // a parameter value of an effect (effect.set value true)
            return QJsonValue(value.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
        break;
    }
    case ParamType::Integer: {
        if (value.isDouble() && value.toDouble() == std::floor(value.toDouble()))
            return QJsonValue(qint64(value.toDouble()));
        bool ok = false;
        const qint64 n = text.toLongLong(&ok);
        if (isString && ok)
            return QJsonValue(n);
        break;
    }
    case ParamType::Number: {
        if (value.isDouble())
            return value;
        bool ok = false;
        const double n = text.toDouble(&ok);
        if (isString && ok)
            return QJsonValue(n);
        break;
    }
    case ParamType::Boolean: {
        if (value.isBool())
            return value;
        const QString t = text.toLower();
        if (isString
            && (t.isEmpty() || t == QLatin1String("true") || t == QLatin1String("yes")
                || t == QLatin1String("1")))
            return QJsonValue(true);
        if (isString
            && (t == QLatin1String("false") || t == QLatin1String("no") || t == QLatin1String("0")))
            return QJsonValue(false);
        break;
    }
    case ParamType::Time: {
        if (value.isDouble() && value.toDouble() == std::floor(value.toDouble()))
            return QJsonValue(QString::number(qint64(value.toDouble())));
        if (isString && !text.trimmed().isEmpty())
            return QJsonValue(text.trimmed());
        break;
    }
    case ParamType::StringList: {
        if (isString)
            return QJsonValue(QJsonArray {text});
        if (value.isArray()) {
            for (const QJsonValue& v : value.toArray()) {
                if (!v.isString()) {
                    if (why)
                        *why = QStringLiteral("a list of strings is expected");
                    return std::nullopt;
                }
            }
            return value;
        }
        break;
    }
    }
    if (why)
        *why = QStringLiteral("a value of type %1 is expected").arg(typeName(param.type));
    return std::nullopt;
}

/// Checks @p arguments against @p params; fills @p typed, or returns the failure.
std::optional<CommandResult> checkArguments(
    const QVector<ParamSpec>& params, const QJsonObject& arguments, QJsonObject& typed)
{
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        const auto found = std::find_if(
            params.begin(), params.end(), [&](const ParamSpec& p) { return p.name == it.key(); });
        if (found == params.end())
            return CommandResult::failure(QStringLiteral("unknown_parameter"),
                QStringLiteral("unknown parameter \"%1\"").arg(it.key()));
        QString why;
        const std::optional<QJsonValue> value = coerce(*found, it.value(), &why);
        if (!value)
            return CommandResult::failure(QStringLiteral("bad_value"),
                QStringLiteral("parameter \"%1\": %2").arg(it.key(), why));
        if (!found->choices.isEmpty() && !found->choices.contains(value->toString()))
            return CommandResult::failure(QStringLiteral("bad_value"),
                QStringLiteral("parameter \"%1\" must be one of: %2")
                    .arg(it.key(), found->choices.join(QStringLiteral(", "))));
        typed.insert(it.key(), *value);
    }
    for (const ParamSpec& p : params) {
        if (p.required && !typed.contains(p.name))
            return CommandResult::failure(QStringLiteral("missing_parameter"),
                QStringLiteral("missing parameter \"%1\"").arg(p.name));
    }
    return std::nullopt;
}

/// Makes the timeline named by @p which (a uid or a name) the active one.
std::optional<CommandResult> selectTimeline(Project& project, const QString& which)
{
    quint64 found = 0;
    bool isNumber = false;
    const quint64 uid = which.toULongLong(&isNumber);
    for (quint64 candidate : project.timelineUids()) {
        if (isNumber && candidate == uid) {
            found = candidate;
            break;
        }
    }
    for (quint64 candidate : project.timelineUids()) {
        const auto data = project.timelineData(candidate);
        if (!found && data && data->name == which)
            found = candidate;
    }
    if (!found)
        return CommandResult::failure(QStringLiteral("unknown_timeline"),
            QStringLiteral("no timeline with id or name \"%1\"").arg(which));
    if (found != project.activeTimelineUid() && !project.setActiveTimeline(found))
        return CommandResult::failure(
            QStringLiteral("timeline_busy"), QStringLiteral("cannot switch timeline now"));
    return std::nullopt;
}

} // namespace

QJsonObject CommandResult::toJson() const
{
    if (ok)
        return {{QStringLiteral("ok"), true}, {QStringLiteral("result"), data}};
    return {{QStringLiteral("ok"), false},
        {QStringLiteral("error"),
            QJsonObject {{QStringLiteral("code"), errorCode}, {QStringLiteral("message"), error}}}};
}

QString CommandArgs::string(const QString& name, const QString& fallback) const
{
    const QJsonValue v = m_values.value(name);
    return v.isString() ? v.toString() : fallback;
}

qint64 CommandArgs::integer(const QString& name, qint64 fallback) const
{
    const QJsonValue v = m_values.value(name);
    return v.isDouble() ? qint64(v.toDouble()) : fallback;
}

double CommandArgs::number(const QString& name, double fallback) const
{
    const QJsonValue v = m_values.value(name);
    return v.isDouble() ? v.toDouble() : fallback;
}

bool CommandArgs::boolean(const QString& name, bool fallback) const
{
    const QJsonValue v = m_values.value(name);
    return v.isBool() ? v.toBool() : fallback;
}

QStringList CommandArgs::strings(const QString& name) const
{
    QStringList out;
    for (const QJsonValue& v : m_values.value(name).toArray())
        out.append(v.toString());
    return out;
}

std::optional<int64_t> CommandArgs::time(const QString& name, Rational fps, QString* error) const
{
    if (!has(name))
        return std::nullopt;
    const QString text = string(name);
    const std::optional<int64_t> frames = timecode::parse(text, fps);
    if (!frames && error)
        *error = QStringLiteral("parameter \"%1\": \"%2\" is not a frame count or timecode")
                     .arg(name, text);
    return frames;
}

const CommandRegistry& CommandRegistry::builtin()
{
    static const CommandRegistry registry = [] {
        CommandRegistry r;
        registerProjectCommands(r);
        registerMediaCommands(r);
        registerTimelineCommands(r);
        registerClipCommands(r);
        registerMarkerCommands(r);
        registerTransitionCommands(r);
        registerExportCommands(r);
        registerFrameCommands(r);
        registerEffectCommands(r);
        registerClipEffectCommands(r);
        registerGeneratorCommands(r);
        registerKeyframeCommands(r);
        registerTrackCommands(r);
        return r;
    }();
    return registry;
}

void CommandRegistry::add(Command command)
{
    m_commands.append(std::move(command));
}

const Command* CommandRegistry::find(const QString& name) const
{
    for (const Command& c : m_commands) {
        if (c.spec.name == name)
            return &c;
    }
    return nullptr;
}

QStringList CommandRegistry::groups() const
{
    QStringList out;
    for (const Command& c : m_commands) {
        const QString group = c.spec.name.section(QLatin1Char('.'), 0, 0);
        if (!out.contains(group))
            out.append(group);
    }
    return out;
}

QVector<ParamSpec> CommandRegistry::fullParams(const CommandSpec& spec)
{
    QVector<ParamSpec> params = spec.params;
    if (spec.use != ProjectUse::None)
        params.append({QStringLiteral("project"), ParamType::Path,
            spec.use == ProjectUse::Create ? QStringLiteral("project file to create")
                                           : QStringLiteral("project file (.kkproj)"),
            true, {}, {}});
    if (spec.use == ProjectUse::Modify)
        params.append({QStringLiteral("output"), ParamType::Path,
            QStringLiteral("save the changed project here and leave --project untouched"), false,
            {}, {}});
    if (spec.use == ProjectUse::Create)
        params.append({QStringLiteral("force"), ParamType::Boolean,
            QStringLiteral("overwrite the file when it exists"), false, {}, {}});
    if (spec.usesTimeline)
        params.append({QStringLiteral("timeline"), ParamType::String,
            QStringLiteral("timeline id or name (default: the active timeline)"),
            spec.timelineRequired, {}, {}});
    return params;
}

QJsonObject CommandRegistry::describe(const CommandSpec& spec)
{
    QJsonArray params;
    for (const ParamSpec& p : fullParams(spec)) {
        QJsonObject o {{QStringLiteral("name"), p.name}, {QStringLiteral("type"), typeName(p.type)},
            {QStringLiteral("description"), p.description},
            {QStringLiteral("required"), p.required}};
        if (!p.defaultText.isEmpty())
            o.insert(QStringLiteral("default"), p.defaultText);
        if (!p.choices.isEmpty())
            o.insert(QStringLiteral("choices"), QJsonArray::fromStringList(p.choices));
        params.append(o);
    }
    return {{QStringLiteral("name"), spec.name}, {QStringLiteral("description"), spec.description},
        {QStringLiteral("parameters"), params},
        {QStringLiteral("modifiesProject"),
            spec.use == ProjectUse::Modify || spec.use == ProjectUse::Create}};
}

QJsonObject CommandRegistry::describeAll() const
{
    QJsonArray list;
    for (const Command& c : m_commands)
        list.append(describe(c.spec));
    return {{QStringLiteral("commands"), list}};
}

CommandResult CommandRegistry::execute(
    const QString& name, const QJsonObject& arguments, const ProgressFn& progress) const
{
    const Command* command = find(name);
    if (!command)
        return CommandResult::failure(
            QStringLiteral("unknown_command"), QStringLiteral("unknown command \"%1\"").arg(name));
    const CommandSpec& spec = command->spec;
    QJsonObject typed;
    if (auto bad = checkArguments(fullParams(spec), arguments, typed))
        return *bad;

    Project project;
    quint64 originalTimeline = 0;
    CommandContext ctx;
    ctx.progress = progress;
    QString savePath;
    if (spec.use != ProjectUse::None) {
        ctx.project = &project;
        ctx.projectPath
            = QFileInfo(typed.value(QStringLiteral("project")).toString()).absoluteFilePath();
        savePath = ctx.projectPath;
        if (spec.use == ProjectUse::Create) {
            if (QFileInfo::exists(ctx.projectPath)
                && !typed.value(QStringLiteral("force")).toBool())
                return CommandResult::failure(QStringLiteral("file_exists"),
                    QStringLiteral("%1 exists (use --force to overwrite)").arg(ctx.projectPath));
        } else {
            if (!QFileInfo(ctx.projectPath).isFile())
                return CommandResult::failure(QStringLiteral("file_not_found"),
                    QStringLiteral("project file %1 does not exist").arg(ctx.projectPath));
            QString error;
            if (!project.load(ctx.projectPath, &error))
                return CommandResult::failure(QStringLiteral("project_error"), error);
        }
        if (spec.use == ProjectUse::Modify && typed.contains(QStringLiteral("output")))
            savePath
                = QFileInfo(typed.value(QStringLiteral("output")).toString()).absoluteFilePath();
        originalTimeline = project.activeTimelineUid();
        if (spec.usesTimeline && typed.contains(QStringLiteral("timeline"))) {
            if (auto bad
                = selectTimeline(project, typed.value(QStringLiteral("timeline")).toString()))
                return *bad;
        }
    }

    CommandResult result = command->run(ctx, CommandArgs(typed));
    // `--timeline` only addresses a timeline for this call; timeline.select is what changes
    // the project's active timeline.
    if (originalTimeline && !spec.timelineRequired)
        project.setActiveTimeline(originalTimeline);
    if (result.ok && (spec.use == ProjectUse::Modify || spec.use == ProjectUse::Create)) {
        QString error;
        if (!project.save(savePath, &error))
            return CommandResult::failure(QStringLiteral("save_failed"), error);
        result.data.insert(QStringLiteral("project"), savePath);
    }
    return result;
}

} // namespace kk::commands
