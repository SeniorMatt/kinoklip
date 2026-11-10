// SPDX-License-Identifier: GPL-3.0-or-later
// Command — the command layer: named editor operations with typed parameters and a JSON result.
// The command-line front end (app/CliRunner.h) calls it, and so will an MCP server.
#pragma once

#include "core/Rational.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace kk {

class Project;

namespace commands {

/// Type of a command parameter. Front ends convert text to it, so a value may arrive as a JSON
/// value of that type or as a string the registry parses.
enum class ParamType {
    String,
    Path, ///< a file or folder name
    Integer,
    Number,
    Boolean,
    /// A time: a frame count (JSON number or "125") or a timecode "HH:MM:SS:FF"; a leading '-'
    /// makes it negative. Read with CommandArgs::time().
    Time,
    StringList ///< a JSON array of strings; the command line repeats the option
};

/// One parameter of a command.
struct ParamSpec
{
    QString name; ///< lower case, words joined by '-' ("source-in")
    ParamType type = ParamType::String;
    QString description;
    bool required = false;
    QString defaultText; ///< shown in the help when not empty
    QStringList choices; ///< allowed values of a String parameter (empty = any)
};

/// What a command does to the project file named by the `project` parameter.
enum class ProjectUse {
    None, ///< needs no project (version)
    Read, ///< loads it, never writes it
    Modify, ///< loads it, saves it afterwards (to `output` when given)
    Create ///< makes a new project and saves it to `project`
};

/// The description of a command: everything a front end needs to list it and check input.
/// The universal parameters `project`, `output` and `timeline` are added by the registry
/// according to @c use and @c usesTimeline.
struct CommandSpec
{
    QString name; ///< "group.verb" ("clip.add") or one word ("render")
    QString description;
    QVector<ParamSpec> params; ///< the command's own parameters
    ProjectUse use = ProjectUse::Read;
    bool usesTimeline = false; ///< accepts `timeline` (a timeline id or name; default: active)
    bool timelineRequired = false; ///< `timeline` must be given (timeline.select)
    /// Live mode (an open window): one undo step for the whole call, cancelled if it fails.
    /// False for commands that must not run inside an open edit (project.save, timeline.select).
    bool liveUndoStep = true;
    /// Live mode: a long job (render, frame export) that runs on a copy of the open project
    /// in a worker thread instead of the window's thread (CommandRegistry::executeOnSnapshot()).
    /// Only for ProjectUse::Read.
    bool background = false;
    /// False for a command only the person at the keyboard may run (settings.import): the MCP
    /// server neither lists nor runs it, and executeLive() (the local control socket) refuses it.
    /// The command line, the dialogs and file-mode execute() keep it.
    bool remoteAllowed = true;
};

/// Succeeded with a JSON object and a short human sentence, or failed with a code and a message.
struct CommandResult
{
    bool ok = false;
    QJsonObject data; ///< the result for machines
    QString text; ///< one or a few lines for people
    /// A picture the command made for the caller to look at (frame.view); null for most commands.
    /// Front ends that cannot show a picture ignore it (the command line prints the text only).
    QImage image;
    QString errorCode; ///< stable lower-case code of a failure ("unknown_clip")
    QString error; ///< message of a failure

    [[nodiscard]] static CommandResult success(QJsonObject data, QString text)
    {
        CommandResult r;
        r.ok = true;
        r.data = std::move(data);
        r.text = std::move(text);
        return r;
    }
    [[nodiscard]] static CommandResult failure(QString code, QString message)
    {
        CommandResult r;
        r.errorCode = std::move(code);
        r.error = std::move(message);
        return r;
    }
    /// {"ok":true,"result":{...}} or {"ok":false,"error":{"code","message"}}.
    [[nodiscard]] QJsonObject toJson() const;
};

/// The checked arguments of one call: every name is a declared parameter and every value has
/// its parameter's type (strings of numbers were converted). Defaults are not filled in.
class CommandArgs
{
public:
    explicit CommandArgs(QJsonObject values = {})
        : m_values(std::move(values))
    { }
    [[nodiscard]] bool has(const QString& name) const { return m_values.contains(name); }
    [[nodiscard]] QString string(const QString& name, const QString& fallback = {}) const;
    [[nodiscard]] qint64 integer(const QString& name, qint64 fallback = 0) const;
    [[nodiscard]] double number(const QString& name, double fallback = 0) const;
    [[nodiscard]] bool boolean(const QString& name, bool fallback = false) const;
    [[nodiscard]] QStringList strings(const QString& name) const;
    /// A Time parameter in frames of @p fps; std::nullopt when absent. Sets @p error (and
    /// returns std::nullopt) when the text is not a time.
    [[nodiscard]] std::optional<int64_t> time(
        const QString& name, Rational fps, QString* error) const;
    [[nodiscard]] const QJsonObject& values() const { return m_values; }

private:
    QJsonObject m_values;
};

/// Progress of a long command (render): @p fraction 0..1 and a short line.
using ProgressFn = std::function<void(double fraction, const QString& message)>;

/// What a running command can use.
struct CommandContext
{
    Project* project = nullptr; ///< loaded (or new) project; null for ProjectUse::None
    QString projectPath; ///< the file the project was loaded from / will be saved to
    ProgressFn progress; ///< may be empty
    /// True when the project is the one open in a window (executeLive()): edits show there and
    /// are one undo step per call; nothing is loaded or saved. False for a project file.
    bool live = false;
    /// Live mode only: the window's normal Save (project.save). Empty in file mode.
    std::function<bool(QString* error)> saveProject;
};

/// The open project of a window, for CommandRegistry::executeLive().
struct LiveTarget
{
    Project* project = nullptr; ///< the window's project (never null)
    /// The window's File > Save; sets the message and returns false on failure. May be empty.
    std::function<bool(QString* error)> save;
};

using CommandHandler = std::function<CommandResult(CommandContext&, const CommandArgs&)>;

/// A spec with the code that runs it.
struct Command
{
    CommandSpec spec;
    CommandHandler run;
};

/// All commands. Built once; execute() is the single entry point of every front end:
/// it checks the arguments, loads the project, selects the timeline, runs the handler and
/// saves the project when the command modifies it.
/// Thread-safety: GUI thread only (commands use the project model).
class CommandRegistry
{
public:
    /// The registry with every built-in command (commands/*Commands.cpp).
    [[nodiscard]] static const CommandRegistry& builtin();

    void add(Command command);
    [[nodiscard]] const Command* find(const QString& name) const;
    [[nodiscard]] const QVector<Command>& all() const { return m_commands; }
    /// The first words of the names ("project", "clip", "render", ...), for the command line.
    [[nodiscard]] QStringList groups() const;

    /// The spec with the universal parameters added.
    /// In live mode (@p live) `project` is optional and `output` / `force` do not exist.
    [[nodiscard]] static QVector<ParamSpec> fullParams(const CommandSpec& spec, bool live = false);
    /// {"name","description","parameters":[{"name","type","description","required","default",
    /// "choices"}],"modifiesProject":bool}.
    [[nodiscard]] static QJsonObject describe(const CommandSpec& spec, bool live = false);
    /// describe() of every command, as {"commands":[...]}.
    [[nodiscard]] QJsonObject describeAll() const;

    /// Runs command @p name with @p arguments (JSON values or strings, by parameter name).
    /// Never throws; every problem is a failed result.
    [[nodiscard]] CommandResult execute(
        const QString& name, const QJsonObject& arguments, const ProgressFn& progress = {}) const;

    /// Runs command @p name on the project open in a window. The same handlers as execute(), no
    /// file is loaded or saved: the call is one undo step of the project (cancelled and the
    /// project left as it was when the command fails), so the user sees the change and can undo
    /// it. `project` is optional and, when given, must be the open project (`wrong_project`);
    /// `output` is refused (`live_unsupported`). A window with an edit in progress is
    /// `window_busy`. Commands that create a file (ProjectUse::Create) do what execute() does.
    /// Must run on the thread that owns the project.
    [[nodiscard]] CommandResult executeLive(const QString& name, const QJsonObject& arguments,
        const LiveTarget& target, const ProgressFn& progress = {}) const;

    /// Runs a CommandSpec::background command (render, frame export) of the open project on a
    /// copy of it: @p documentJson is Project::documentJson(@p documentPath) taken on the
    /// project's thread. Safe to call from a worker thread; the open project is not touched.
    /// @p documentPath is the open project's file (empty if it was never saved); a `project`
    /// argument must match it, as in executeLive().
    [[nodiscard]] CommandResult executeOnSnapshot(const QString& name, const QJsonObject& arguments,
        const QByteArray& documentJson, const QString& documentPath,
        const ProgressFn& progress = {}) const;

private:
    QVector<Command> m_commands;
};

// Registration, one function per file (called by CommandRegistry::builtin()).
void registerProjectCommands(CommandRegistry& registry);
void registerMediaCommands(CommandRegistry& registry);
void registerTimelineCommands(CommandRegistry& registry);
void registerClipCommands(CommandRegistry& registry);
void registerStabilizeCommands(CommandRegistry& registry);
void registerMarkerCommands(CommandRegistry& registry);
void registerTransitionCommands(CommandRegistry& registry);
void registerExportCommands(CommandRegistry& registry);
void registerFrameCommands(CommandRegistry& registry);
void registerEffectCommands(CommandRegistry& registry);
void registerClipEffectCommands(CommandRegistry& registry);
void registerGeneratorCommands(CommandRegistry& registry);
void registerKeyframeCommands(CommandRegistry& registry);
void registerTrackCommands(CommandRegistry& registry);
void registerSettingsCommands(CommandRegistry& registry);

} // namespace commands
} // namespace kk
