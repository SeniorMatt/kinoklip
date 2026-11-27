// SPDX-License-Identifier: GPL-3.0-or-later
// PluginHost — see PluginHost.h.
#include "app/PluginHost.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "plugins/PluginRegistry.h"
#include "plugins/PresetRecipe.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/PluginsPage.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::PluginHost", text);
}

} // namespace

PluginHost::PluginHost(AppContext& ctx, commands::LiveTarget target, QWidget* window)
    : QObject(window)
    , m_ctx(ctx)
    , m_target(std::move(target))
    , m_window(window)
{
    m_runner = new plugins::ScriptRunner(
        [this](const QString& command, const QJsonObject& args) {
            return executeForScript(m_runningPermissions, command, args);
        },
        this);
}

bool PluginHost::hasPluginFile(const QStringList& paths)
{
    for (const QString& p : paths) {
        if (p.endsWith(QLatin1String(plugins::kPackageSuffix), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool PluginHost::installDropped(const QStringList& paths)
{
    bool any = false;
    for (const QString& p : paths) {
        if (p.endsWith(QLatin1String(plugins::kPackageSuffix), Qt::CaseInsensitive)
            || (QFileInfo(p).isDir()
                && QFileInfo::exists(
                    p + QLatin1Char('/') + QLatin1String(plugins::kManifestFile)))) {
            any = true;
            if (m_quiet) {
                QString error;
                plugins::PluginRegistry::instance().install(p, &error);
            } else {
                PluginsPage::installWithMessages(p, m_window);
            }
        }
    }
    return any;
}

QJsonObject PluginHost::executeForScript(
    const QStringList& permissions, const QString& command, const QJsonObject& args)
{
    const commands::CommandRegistry& registry = commands::CommandRegistry::builtin();
    const auto error = [](const QString& code, const QString& message) {
        return QJsonObject {{QStringLiteral("ok"), false},
            {QStringLiteral("error"),
                QJsonObject {
                    {QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
    };
    if (command == QLatin1String("kk.commands")) {
        return QJsonObject {
            {QStringLiteral("ok"), true}, {QStringLiteral("result"), registry.describeAll()}};
    }
    const commands::Command* c = registry.find(command);
    if (!c)
        return error(QStringLiteral("unknown_command"),
            QStringLiteral("unknown command \"%1\"").arg(command));
    if (!c->spec.remoteAllowed)
        return error(QStringLiteral("not_allowed"),
            QStringLiteral("\"%1\" can only be run by the person at the keyboard").arg(command));
    QString need = QStringLiteral("edit");
    if (c->spec.use == commands::ProjectUse::None)
        need = QStringLiteral("none");
    else if (c->spec.use == commands::ProjectUse::Read)
        need = QStringLiteral("read");
    if (!plugins::permissionAllows(permissions, need))
        return error(QStringLiteral("permission_denied"),
            QStringLiteral("\"%1\" needs the permission %2, which the script did not declare")
                .arg(command,
                    need == QLatin1String("read") ? QStringLiteral("project.read")
                                                  : QStringLiteral("project.edit")));
    return registry.executeLive(command, args, m_target).toJson();
}

namespace {

/// The edit point at the head or tail of clip @p clipId on its track.
plugins::RecipeTarget editPointOf(const TimelineData& data, quint64 clipId, bool head,
    qint64 playhead, bool* found)
{
    plugins::RecipeTarget t;
    t.edgePoint = true;
    t.playhead = playhead;
    const auto loc = editops::findClip(data, clipId);
    *found = loc.has_value();
    if (!loc)
        return t;
    const TrackData& track = *data.track(loc->track);
    const size_t index = size_t(loc->clipIndex);
    const ClipData& c = track.clips[index];
    const ClipData* left = nullptr;
    const ClipData* right = nullptr;
    if (head) {
        right = &c;
        if (index > 0 && track.clips[index - 1].end() == c.start)
            left = &track.clips[index - 1];
        t.edit = c.start;
    } else {
        left = &c;
        if (index + 1 < track.clips.size() && track.clips[index + 1].start == c.end())
            right = &track.clips[index + 1];
        t.edit = c.end();
    }
    t.left = left ? qint64(left->id) : 0;
    t.right = right ? qint64(right->id) : 0;
    const ClipData* main = right ? right : left;
    t.clip = qint64(main->id);
    t.start = main->start;
    t.end = main->start + main->duration - 1;
    t.duration = main->duration;
    return t;
}

} // namespace

int PluginHost::applyPresetAtEdge(
    Project& project, quint64 clipId, bool head, const QString& presetId)
{
    bool found = false;
    const plugins::RecipeTarget t = editPointOf(
        project.timeline()->data(), clipId, head, project.timeline()->playhead(), &found);
    const plugins::PluginPresetDef* def = plugins::PluginRegistry::instance().findPreset(presetId);
    if (!def)
        return failPreset(tr("The preset is not available (its plugin is missing or disabled)."));
    if (!found || def->target != plugins::PresetTarget::EditPoint)
        return 0;
    return runRecipe(project, presetId, {t});
}

int PluginHost::applyPreset(
    Project& project, const QVector<quint64>& clips, const QString& presetId)
{
    plugins::PluginRegistry& registry = plugins::PluginRegistry::instance();
    const plugins::PluginPresetDef* def = registry.findPreset(presetId);
    if (!def)
        return failPreset(tr("The preset is not available (its plugin is missing or disabled)."));

    const TimelineData& data = project.timeline()->data();
    QVector<plugins::RecipeTarget> targets;
    const int64_t playhead = project.timeline()->playhead();
    if (def->target == plugins::PresetTarget::EditPoint) {
        // The edge of each selected clip that is nearest the playhead (an edit point is shared
        // by its two clips: it is applied once).
        QSet<QPair<qint64, qint64>> seen;
        for (const quint64 id : clips) {
            const ClipData* clip = editops::clipById(data, id);
            if (!clip)
                continue;
            const bool head = std::abs(playhead - clip->start)
                <= std::abs(playhead - (clip->start + clip->duration));
            bool found = false;
            const plugins::RecipeTarget t = editPointOf(data, id, head, playhead, &found);
            const auto loc = editops::findClip(data, id);
            const QPair<qint64, qint64> key {
                loc ? qint64(loc->track.index) * 4 + qint64(loc->track.type) : 0, t.edit};
            if (found && !seen.contains(key)) {
                seen.insert(key);
                targets.append(t);
            }
        }
        if (targets.isEmpty())
            return failPreset(tr("Select a clip next to the edit point to apply this preset to."));
    } else if (def->target == plugins::PresetTarget::Playhead) {
        plugins::RecipeTarget t;
        t.playhead = playhead;
        targets.append(t);
    } else {
        for (const quint64 id : clips) {
            const ClipData* clip = editops::clipById(data, id);
            if (!clip)
                continue;
            plugins::RecipeTarget t;
            t.clip = qint64(id);
            t.start = clip->start;
            t.end = clip->start + clip->duration - 1;
            t.duration = clip->duration;
            t.playhead = playhead;
            targets.append(t);
        }
        if (targets.isEmpty())
            return failPreset(tr("Select a clip to apply this preset to."));
    }
    return runRecipe(project, presetId, targets);
}

int PluginHost::failPreset(const QString& message)
{
    emit presetFailed(message);
    if (!m_quiet)
        QMessageBox::warning(m_window, tr("Preset"), message);
    return 0;
}

int PluginHost::runRecipe(
    Project& project, const QString& presetId, const QVector<plugins::RecipeTarget>& targets)
{
    plugins::PluginRegistry& registry = plugins::PluginRegistry::instance();
    const plugins::PluginPresetDef* def = registry.findPreset(presetId);
    if (!def)
        return failPreset(tr("The preset is not available (its plugin is missing or disabled)."));
    QString error;
    const QJsonObject recipe = registry.presetRecipe(presetId, &error);
    if (recipe.isEmpty())
        return failPreset(error);
    (void)project;
    QVector<QPair<QString, QJsonObject>> steps;
    for (const plugins::RecipeTarget& t : targets) {
        const QVector<plugins::RecipeStep> expanded = plugins::expandRecipe(recipe, t, &error);
        if (expanded.isEmpty())
            return failPreset(error);
        for (const plugins::RecipeStep& s : expanded)
            steps.append({s.command, s.args});
    }
    const commands::CommandResult result = commands::CommandRegistry::builtin().executeLiveBatch(
        tr("Apply %1").arg(def->name), steps, m_target);
    if (!result.ok)
        return failPreset(result.error);
    return int(targets.size());
}

void PluginHost::attachScriptsMenu(QMenu* menu)
{
    if (!menu)
        return;
    connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillScriptsMenu(menu); });
    connect(&plugins::PluginRegistry::instance(), &plugins::PluginRegistry::changed, menu,
        [this, menu] { fillScriptsMenu(menu); });
    fillScriptsMenu(menu);
}

void PluginHost::fillScriptsMenu(QMenu* menu)
{
    menu->clear();
    const QVector<plugins::ScriptEntry> scripts = plugins::PluginRegistry::instance().scripts();
    for (const plugins::ScriptEntry& e : scripts) {
        QAction* action = menu->addAction(e.def.name);
        action->setToolTip(e.def.description.isEmpty() ? e.pluginName : e.def.description);
        action->setStatusTip(action->toolTip());
        connect(action, &QAction::triggered, this, [this, id = e.def.fullId] { runScript(id); });
    }
    if (scripts.isEmpty()) {
        QAction* none = menu->addAction(tr("No scripts installed"));
        none->setEnabled(false);
    }
    menu->addSeparator();
    if (QAction* manage
        = m_ctx.actions ? m_ctx.actions->action(QStringLiteral("settings.plugins")) : nullptr)
        menu->addAction(manage);
}

bool PluginHost::runScript(const QString& scriptId)
{
    for (const plugins::ScriptEntry& e : plugins::PluginRegistry::instance().scripts()) {
        if (e.def.fullId != scriptId)
            continue;
        showOutput(e);
        m_runningPermissions = e.def.permissions;
        QString error;
        if (!m_runner->start(e, &error)) {
            m_output->appendPlainText(error);
            return false;
        }
        return true;
    }
    return false;
}

void PluginHost::showOutput(const plugins::ScriptEntry& script)
{
    if (!m_outputDialog) {
        m_outputDialog = new QDialog(m_window);
        m_outputDialog->setObjectName(QStringLiteral("ScriptOutput"));
        auto* layout = new QVBoxLayout(m_outputDialog);
        m_output = new QPlainTextEdit;
        m_output->setObjectName(QStringLiteral("ScriptOutputText"));
        m_output->setReadOnly(true);
        layout->addWidget(m_output);
        auto* buttons = new QHBoxLayout;
        auto* stop = new QPushButton(tr("Stop"));
        stop->setObjectName(QStringLiteral("ScriptStop"));
        auto* close = new QPushButton(tr("Close"));
        buttons->addStretch(1);
        buttons->addWidget(stop);
        buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(stop, &QPushButton::clicked, m_runner, &plugins::ScriptRunner::cancel);
        connect(close, &QPushButton::clicked, m_outputDialog, &QDialog::hide);
        connect(m_runner, &plugins::ScriptRunner::outputLine, m_output,
            [this](const QString& line) { m_output->appendPlainText(line); });
        connect(m_runner, &plugins::ScriptRunner::finished, m_output,
            [this](int, const QString& summary) { m_output->appendPlainText(summary); });
        m_outputDialog->resize(560, 360);
    }
    m_outputDialog->setWindowTitle(tr("Script: %1").arg(script.def.name));
    m_output->clear();
    m_outputDialog->show();
    m_outputDialog->raise();
}

} // namespace kk
