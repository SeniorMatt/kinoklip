// SPDX-License-Identifier: GPL-3.0-or-later
// PluginHost — the window's side of the plugin system (docs/plugins/design.md): applies the
// presets of plugins (recipes of command-layer calls) as one undo step, runs plugin scripts and
// fills Workspace > Scripts, installs .kkplugin files dropped on the window. It owns nothing of
// the plugin data (PluginRegistry does); it knows the window's project and command layer.
#pragma once

#include "app/commands/Command.h"
#include "plugins/PresetRecipe.h"
#include "plugins/ScriptRunner.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class QMenu;
class QPlainTextEdit;
class QDialog;

namespace kk {

class Project;
struct AppContext;

/// Thread-safety: GUI thread.
class PluginHost : public QObject
{
    Q_OBJECT
public:
    /// @p target is the window's project as the command layer sees it (see CommandRegistry::
    /// executeLive()); @p window is the parent of the dialogs it shows.
    PluginHost(AppContext& ctx, commands::LiveTarget target, QWidget* window);

    /// Applies preset @p presetId (a full plugin id) to @p clips (target "clip") or at the
    /// playhead (target "playhead") as one undo step. Returns the number of targets it was
    /// applied to, 0 when it failed (presetFailed() says why).
    int applyPreset(Project& project, const QVector<quint64>& clips, const QString& presetId);

    /// Applies an "edit_point" preset (a transition preset) at the cut at the head (@p head) or
    /// tail of clip @p clipId: the clip touching it on the other side, if any, is $left or
    /// $right. One undo step; 1 when applied, 0 when it failed.
    int applyPresetAtEdge(
        Project& project, quint64 clipId, bool head, const QString& presetId);

    /// Runs script @p scriptId (full id) of an enabled plugin and shows its output window.
    /// False (and presetFailed-like message in the output) when it cannot start.
    bool runScript(const QString& scriptId);
    [[nodiscard]] plugins::ScriptRunner* runner() { return m_runner; }

    /// Keeps @p menu (Workspace > Scripts) filled with the scripts of the enabled plugins.
    void attachScriptsMenu(QMenu* menu);

    /// Installs the .kkplugin files and plugin folders among @p paths, with the usual messages;
    /// returns true when at least one was a plugin file (the drop is then consumed).
    bool installDropped(const QStringList& paths);
    /// True if one of @p paths is a .kkplugin file.
    [[nodiscard]] static bool hasPluginFile(const QStringList& paths);

    /// Quiet mode for tests: errors go to presetFailed() only, no message boxes.
    void setQuiet(bool quiet) { m_quiet = quiet; }

    /// The command executor scripts use, with @p permissions (also for tests).
    [[nodiscard]] QJsonObject executeForScript(
        const QStringList& permissions, const QString& command, const QJsonObject& args);

signals:
    /// A preset could not be applied.
    void presetFailed(const QString& message);

private:
    int failPreset(const QString& message);
    int runRecipe(Project& project, const QString& presetId,
        const QVector<plugins::RecipeTarget>& targets);
    void fillScriptsMenu(QMenu* menu);
    void showOutput(const plugins::ScriptEntry& script);

    AppContext& m_ctx;
    commands::LiveTarget m_target;
    QWidget* m_window;
    plugins::ScriptRunner* m_runner = nullptr;
    QDialog* m_outputDialog = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QStringList m_runningPermissions;
    bool m_quiet = false;
};

} // namespace kk
