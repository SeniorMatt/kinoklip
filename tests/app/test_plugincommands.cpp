// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin commands (plugin.list, plugin.create, ...), the way the command line and the MCP
// server use them, plugin effects through effect.add / effect.stack, presets and scripts through
// the window's PluginHost (one undo step, permissions), the Plugins page, the Workspace > Scripts
// menu and the tool list of MCP (docs/behavior-spec.md section 157).
#include "app/PluginHost.h"
#include "app/commands/Command.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "plugins/PluginRegistry.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PluginsPage.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::commands;
using kk::plugins::PluginRegistry;

namespace {

QString examplesDir()
{
    return QFileInfo(QStringLiteral(__FILE__)).absolutePath()
        + QStringLiteral("/../../plugins/examples");
}

CommandResult call(const QString& name, QJsonObject args = {})
{
    return CommandRegistry::builtin().execute(name, args);
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

} // namespace

class TestPluginCommands : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_user;

    CommandResult ok(const QString& name, QJsonObject args = {})
    {
        const CommandResult r = call(name, args);
        [&] { QVERIFY2(r.ok, qPrintable(name + ": " + r.errorCode + " " + r.error)); }();
        return r;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_plugincommands"));
        kk::initResources();
        QVERIFY(m_dir.isValid());
        m_user = m_dir.filePath(QStringLiteral("user"));
        qputenv("KK_PLUGIN_EXAMPLES_DIR", examplesDir().toLocal8Bit());
    }

    void init()
    {
        QDir(m_user).removeRecursively();
        QDir().mkpath(m_user);
        QSettings().clear();
        PluginRegistry::instance().setDirectories(
            m_dir.filePath(QStringLiteral("bundled")), m_user);
        PluginRegistry::instance().reload();
    }

    void commandsAreInTheRegistryAndMcpToolList()
    {
        QStringList names;
        for (const Command& c : CommandRegistry::builtin().all())
            names << c.spec.name;
        for (const char* expected :
            {"plugin.list", "plugin.info", "plugin.create", "plugin.copy_builtin",
                "plugin.validate", "plugin.install", "plugin.uninstall", "plugin.enable",
                "plugin.disable", "plugin.reload", "plugin.pack", "plugin.render_preview"}) {
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
            const Command* c = CommandRegistry::builtin().find(QLatin1String(expected));
            QVERIFY(!c->spec.description.isEmpty());
            QCOMPARE(c->spec.use, ProjectUse::None); // no project needed: plugin work is global
            QVERIFY(c->spec.remoteAllowed); // an assistant may use all of them
        }
    }

    void createAnEffectScaffoldsInstallsAndValidates()
    {
        const CommandResult r = ok("plugin.create",
            {{"id", "test.tint"}, {"name", "Tint"}, {"kind", "effect"}, {"entry", "color"},
                {"description", "Tints"}, {"group", "color"},
                {"param",
                    QJsonArray {"amount:number:0:100:50:Amount", "mode:choice:soft|hard:soft",
                        "on:boolean:true", "tint:color:#ff8800", "steps:integer:1:8:4"}}});
        QVERIFY(QFileInfo(m_user + QStringLiteral("/test.tint/plugin.json")).isFile());
        QVERIFY(QFileInfo(m_user + QStringLiteral("/test.tint/tint.glsl")).isFile());
        QCOMPARE(r.data["itemId"].toString(), QString("plugin:test.tint/tint"));
        // It is installed and the effect is in the catalogue with all its parameters.
        const EffectDescriptor* d
            = EffectRegistry::instance().find(QStringLiteral("plugin:test.tint/tint"));
        QVERIFY(d);
        QCOMPARE(d->params.size(), 5);
        QCOMPARE(d->params[0].def, 50.0);
        QCOMPARE(d->params[1].choices, (QStringList {"soft", "hard"}));
        // effect.list --id shows it to an assistant, like a built-in.
        const CommandResult listed = ok("effect.list", {{"id", "plugin:test.tint/tint"}});
        QCOMPARE(listed.data["items"][0]["parameters"].toArray().size(), 5);
        // The template is valid as written (static checks; the GL compile when a context exists).
        const CommandResult v = ok("plugin.validate", {{"target", "test.tint"}, {"static", true}});
        QVERIFY2(v.data["valid"].toBool(), qPrintable(v.text));
        // plugin.info and plugin.list.
        const CommandResult info = ok("plugin.info", {{"id", "test.tint"}});
        QCOMPARE(info.data["items"].toArray().size(), 1);
        QVERIFY(info.data["manifest"]["effects"].isArray());
        bool listedIt = false;
        for (const QJsonValue& p : ok("plugin.list").data["plugins"].toArray())
            listedIt |= p["id"].toString() == QLatin1String("test.tint") && p["enabled"].toBool();
        QVERIFY(listedIt);
        // An existing id is refused unless overwrite is given; a bad id is refused.
        QCOMPARE(call("plugin.create", {{"id", "test.tint"}}).errorCode, QString("exists"));
        QCOMPARE(call("plugin.create", {{"id", "Bad Id"}}).errorCode, QString("bad_value"));
        QCOMPARE(
            call("plugin.create", {{"id", "test.x"}, {"param", QJsonArray {"nope"}}}).errorCode,
            QString("bad_value"));
        QVERIFY(call("plugin.create", {{"id", "test.tint"}, {"overwrite", true}}).ok);
    }

    void everyKindScaffoldIsValid()
    {
        for (const char* kind : {"effect", "transition", "preset", "script"}) {
            const QString id = QStringLiteral("test.%1").arg(QLatin1String(kind));
            ok("plugin.create", {{"id", id}, {"kind", kind}, {"name", "Thing"}});
            const CommandResult v = ok("plugin.validate", {{"target", id}, {"static", true}});
            QVERIFY2(v.data["valid"].toBool(), qPrintable(id + ": " + v.text));
        }
        // The script plugin was installed switched off.
        QVERIFY(!PluginRegistry::instance().find(QStringLiteral("test.script"))->enabled);
    }

    void validateReportsTheLineOfAProblem()
    {
        ok("plugin.create", {{"id", "test.bad"}, {"name", "Bad"}});
        writeFile(m_user + QStringLiteral("/test.bad/bad.glsl"),
            "vec4 effect(vec2 uv)\n{\n    float patch = 2.0;\n    return texture(uTex, uv) * "
            "patch;\n}\n");
        const CommandResult v = ok("plugin.validate", {{"target", "test.bad"}, {"static", true}});
        QVERIFY(!v.data["valid"].toBool());
        bool found = false;
        for (const QJsonValue& p : v.data["problems"].toArray())
            found |= p["line"].toInt() == 3 && p["message"].toString().contains("patch");
        QVERIFY2(found, qPrintable(v.text));
        // A folder that is not installed can be validated too.
        const QString dir = m_dir.filePath(QStringLiteral("elsewhere"));
        writeFile(dir + QStringLiteral("/plugin.json"), R"({"id":"test.ok"})");
        const CommandResult w = ok("plugin.validate", {{"target", dir}, {"static", true}});
        QVERIFY(!w.data["valid"].toBool());
        QCOMPARE(w.data["problems"][0]["where"].toString(), QString("name"));
        QCOMPARE(call("plugin.validate", {{"target", "nothing.here"}}).errorCode,
            QString("unknown_plugin"));
    }

    void copyABuiltInMakesAnEditablePlugin()
    {
        ok("plugin.copy_builtin",
            {{"builtin", "invert"}, {"id", "me.my-invert"}, {"name", "My Invert"}});
        const auto* p = PluginRegistry::instance().find(QStringLiteral("me.my-invert"));
        QVERIFY(p && p->valid);
        QCOMPARE(p->manifest.name, QString("My Invert"));
        QVERIFY(EffectRegistry::instance().find(QStringLiteral("plugin:me.my-invert/invert")));
        QVERIFY(
            readAll(m_user + QStringLiteral("/me.my-invert/invert.glsl")).contains("vec3 apply"));
        // The transition too, and a built-in that is not a plain shader says what is possible.
        ok("plugin.copy_builtin", {{"builtin", "dip_to_color"}, {"id", "me.my-dip"}});
        const CommandResult no
            = call("plugin.copy_builtin", {{"builtin", "gaussian_blur"}, {"id", "me.blur"}});
        QCOMPARE(no.errorCode, QString("not_exportable"));
        QVERIFY2(no.error.contains("invert") && no.error.contains("plugin.create"),
            qPrintable(no.error));
        QCOMPARE(call("plugin.copy_builtin", {{"builtin", "nonsense"}, {"id", "me.x"}}).errorCode,
            QString("not_exportable"));
        QCOMPARE(
            call("plugin.copy_builtin", {{"builtin", "invert"}, {"id", "me.my-invert"}}).errorCode,
            QString("exists"));
    }

    void installPackUninstallEnableReload()
    {
        ok("plugin.create", {{"id", "test.pack-me"}, {"name", "Pack Me"}});
        const QString file = m_dir.filePath(QStringLiteral("pack-me.kkplugin"));
        ok("plugin.pack", {{"id", "test.pack-me"}, {"file", file}});
        QVERIFY(QFileInfo(file).size() > 100);
        QCOMPARE(call("plugin.pack", {{"id", "test.pack-me"}, {"file", m_dir.filePath("x.zip")}})
                     .errorCode,
            QString("bad_value"));
        ok("plugin.uninstall", {{"id", "test.pack-me"}});
        QVERIFY(!QFileInfo::exists(m_user + QStringLiteral("/test.pack-me")));
        QCOMPARE(call("plugin.uninstall", {{"id", "test.pack-me"}}).errorCode,
            QString("uninstall_failed"));
        ok("plugin.install", {{"path", file}});
        QVERIFY(QFileInfo::exists(m_user + QStringLiteral("/test.pack-me/plugin.json")));
        QCOMPARE(call("plugin.install", {{"path", m_dir.filePath("missing")}}).errorCode,
            QString("install_failed"));
        ok("plugin.disable", {{"id", "test.pack-me"}});
        QVERIFY(EffectRegistry::instance()
                    .find(QStringLiteral("plugin:test.pack-me/pack_me"))
                    ->unavailable);
        ok("plugin.enable", {{"id", "test.pack-me"}});
        QVERIFY(!EffectRegistry::instance()
                     .find(QStringLiteral("plugin:test.pack-me/pack_me"))
                     ->unavailable);
        QCOMPARE(call("plugin.enable", {{"id", "no.such"}}).errorCode, QString("unknown_plugin"));
        // Edit on disk, reload through the command.
        writeFile(m_user + QStringLiteral("/test.pack-me/pack_me.glsl"),
            "vec4 effect(vec2 uv) { return vec4(0.5); }\n");
        const CommandResult reloaded = ok("plugin.reload");
        QCOMPARE(reloaded.data["broken"].toInt(), 0);
        QVERIFY(PluginRegistry::instance()
                    .shaderSpec(QStringLiteral("plugin:test.pack-me/pack_me"))
                    ->source.contains("vec4(0.5)"));
    }

    void scriptPluginsNeedThePersonsConsentOverTheLiveConnection()
    {
        ok("plugin.create", {{"id", "test.script"}, {"kind", "script"}});
        Project project;
        LiveTarget target;
        target.project = &project;
        const CommandResult r = CommandRegistry::builtin().executeLive(
            QStringLiteral("plugin.enable"), {{"id", "test.script"}}, target);
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QString("needs_consent"));
        QVERIFY(!PluginRegistry::instance().find(QStringLiteral("test.script"))->enabled);
        // The person on their own command line can.
        ok("plugin.enable", {{"id", "test.script"}});
        QVERIFY(PluginRegistry::instance().find(QStringLiteral("test.script"))->enabled);
    }

    void aPluginEffectWorksLikeABuiltInInTheCommandLayer()
    {
        QString error;
        QVERIFY2(PluginRegistry::instance().install(
                     examplesDir() + QStringLiteral("/dev.kinoklip.invert"), &error),
            qPrintable(error));
        Project project;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.start = 0;
        clip.duration = 50;
        project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 clipId = editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        LiveTarget target;
        target.project = &project;
        const CommandRegistry& registry = CommandRegistry::builtin();
        const CommandResult added = registry.executeLive(QStringLiteral("effect.add"),
            {{"clip", qint64(clipId)}, {"type", "plugin:dev.kinoklip.invert/invert"},
                {"set", QJsonArray {"amount=40"}}},
            target);
        QVERIFY2(added.ok, qPrintable(added.error));
        const qint64 fx = added.data["effect"]["id"].toInteger();
        QCOMPARE(added.data["effect"]["parameters"]["amount"].toDouble(), 40.0);
        // Out of the range declared in plugin.json: the same refusal as a built-in.
        const CommandResult bad = registry.executeLive(QStringLiteral("effect.set"),
            {{"clip", qint64(clipId)}, {"effect", fx}, {"key", "amount"}, {"value", 101}}, target);
        QCOMPARE(bad.errorCode, QString("out_of_range"));
        // Keyframes through the command layer.
        const CommandResult key = registry.executeLive(QStringLiteral("keyframe.set"),
            {{"clip", qint64(clipId)}, {"effect", fx}, {"param", "amount"}, {"at", 10},
                {"value", "80"}},
            target);
        QVERIFY2(key.ok, qPrintable(key.error));
        // Undo takes the effect away again (one step per command in the window).
        const int steps = project.undoStack()->count();
        QVERIFY(steps >= 2);
        // Uninstalled: the clip keeps the effect, effect.stack shows it by its id.
        QVERIFY(
            PluginRegistry::instance().uninstall(QStringLiteral("dev.kinoklip.invert"), &error));
        const CommandResult stack = registry.executeLive(
            QStringLiteral("effect.stack"), {{"clip", qint64(clipId)}}, target);
        QVERIFY2(stack.ok, qPrintable(stack.error));
        QCOMPARE(stack.data["effects"][0]["type"].toString(),
            QString("plugin:dev.kinoklip.invert/invert"));
    }

    void presetsApplyAsOneUndoStepToEveryClip()
    {
        QString error;
        QVERIFY2(PluginRegistry::instance().install(
                     examplesDir() + QStringLiteral("/dev.kinoklip.animation-pack"), &error),
            qPrintable(error));
        Project project;
        for (int i = 0; i < 2; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.start = i * 100;
            clip.duration = 80;
            project.beginEdit(QStringLiteral("Add Clip"));
            editops::overwriteClip(
                project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
            project.endEdit();
        }
        const auto& clips = project.timeline()->data().videoTracks[0].clips;
        const QVector<quint64> ids {clips[0].id, clips[1].id};
        ActionRegistry actions;
        registerStandardActions(actions);
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        LiveTarget target;
        target.project = &project;
        PluginHost host(ctx, target, nullptr);
        host.setQuiet(true);
        const int undoBefore = project.undoStack()->count();
        QCOMPARE(host.applyPreset(
                     project, ids, QStringLiteral("plugin:dev.kinoklip.animation-pack/fade_in")),
            2);
        QCOMPARE(project.undoStack()->count(), undoBefore + 1); // both clips, ONE undo step
        for (const ClipData& c : project.timeline()->data().videoTracks[0].clips) {
            const AnimatedValue& opacity = c.transform.values[QStringLiteral("opacity")];
            QCOMPARE(opacity.keys.size(), 2);
            QCOMPARE(opacity.keys[0].frame, int64_t(0)); // clip-local: starts at the clip start
            QCOMPARE(opacity.keys[0].value, 0.0);
            QCOMPARE(opacity.keys[1].frame, int64_t(24));
            QCOMPARE(opacity.keys[1].value, 100.0);
        }
        // The end-anchored preset keys the last frames.
        QCOMPARE(host.applyPreset(project, {ids[1]},
                     QStringLiteral("plugin:dev.kinoklip.animation-pack/fade_out")),
            1);
        const AnimatedValue& last
            = project.timeline()->data().videoTracks[0].clips[1].transform.values[QStringLiteral(
                "opacity")];
        QCOMPARE(last.keys.last().frame, int64_t(79));
        // The effect-stack preset adds effects, and "$last_effect"-free recipes work as is.
        QCOMPARE(host.applyPreset(project, {ids[0]},
                     QStringLiteral("plugin:dev.kinoklip.animation-pack/soft_glow_look")),
            1);
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[0].effects.size(), 2);
        // One undo takes the whole preset back.
        project.undoStack()->undo();
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[0].effects.size(), 0);
        // A failing recipe leaves nothing behind.
        writeFile(m_user + QStringLiteral("/dev.kinoklip.animation-pack/fade_in.json"),
            R"({"commands":[{"command":"effect.add","args":{"clip":"$clip","type":"gaussian_blur"}},{"command":"effect.add","args":{"clip":"$clip","type":"does_not_exist"}}]})");
        PluginRegistry::instance().reload();
        QSignalSpy failed(&host, &PluginHost::presetFailed);
        const int count = project.undoStack()->count();
        QCOMPARE(host.applyPreset(project, {ids[0]},
                     QStringLiteral("plugin:dev.kinoklip.animation-pack/fade_in")),
            0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(project.undoStack()->count(), count);
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[0].effects.size(), 0);
        // No clip selected: a clear message, nothing happens.
        QCOMPARE(host.applyPreset(
                     project, {}, QStringLiteral("plugin:dev.kinoklip.animation-pack/pop_in")),
            0);
        QCOMPARE(failed.count(), 2);
        // A preset of a disabled plugin is not available.
        PluginRegistry::instance().setEnabled(QStringLiteral("dev.kinoklip.animation-pack"), false);
        QCOMPARE(host.applyPreset(
                     project, ids, QStringLiteral("plugin:dev.kinoklip.animation-pack/pop_in")),
            0);
    }

    void transitionPresetsApplyAtAnEditPoint()
    {
        const QString bundled = QFileInfo(QStringLiteral(__FILE__)).absolutePath()
            + QStringLiteral("/../../plugins/bundled");
        QString error;
        for (const char* name : {"dev.kinoklip.gl-transitions", "dev.kinoklip.gl-transitions-2",
                 "dev.kinoklip.transition-presets"})
            QVERIFY2(PluginRegistry::instance().install(
                         bundled + QLatin1Char('/') + QLatin1String(name), &error),
                qPrintable(error));
        Project project;
        for (int i = 0; i < 2; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.start = i * 100;
            clip.duration = 100;
            project.beginEdit(QStringLiteral("Add Clip"));
            editops::overwriteClip(
                project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
            project.endEdit();
        }
        const QVector<quint64> ids {project.timeline()->data().videoTracks[0].clips[0].id,
            project.timeline()->data().videoTracks[0].clips[1].id};
        ActionRegistry actions;
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        LiveTarget target;
        target.project = &project;
        PluginHost host(ctx, target, nullptr);
        host.setQuiet(true);
        const QString quick = QStringLiteral("plugin:dev.kinoklip.transition-presets/quick_dissolve");
        const int undoBefore = project.undoStack()->count();
        // on the head of the second clip: the cut between the two
        QCOMPARE(host.applyPresetAtEdge(project, ids[1], true, quick), 1);
        QCOMPARE(project.undoStack()->count(), undoBefore + 1);
        {
            const auto& t = project.timeline()->data().videoTracks[0].clips[1].transitionIn;
            QVERIFY(t.has_value());
            QCOMPARE(t->type, QString("cross_dissolve"));
            QCOMPARE(t->duration, int64_t(12));
        }
        project.undoStack()->undo();
        QVERIFY(!project.timeline()->data().videoTracks[0].clips[1].transitionIn.has_value());
        // the tail of the first clip is the same cut; a selected clip uses the edge nearest the
        // playhead (here the tail of the first one)
        project.timeline()->setPlayhead(95);
        QCOMPARE(host.applyPreset(project, {ids[0]},
                     QStringLiteral("plugin:dev.kinoklip.transition-presets/dip_to_black")),
            1);
        {
            const auto& t = project.timeline()->data().videoTracks[0].clips[1].transitionIn;
            QVERIFY(t.has_value());
            QCOMPARE(t->type, QString("dip_to_color"));
            QCOMPARE(t->duration, int64_t(24));
        }
        // a preset that uses a transition of the GL Transitions pack
        project.undoStack()->undo();
        QCOMPARE(host.applyPresetAtEdge(project, ids[1], true,
                     QStringLiteral("plugin:dev.kinoklip.transition-presets/whip_pan")),
            1);
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[1].transitionIn->type,
            QString("plugin:dev.kinoklip.gl-transitions-2/whip_pan"));
        // a clip preset does not run at an edit point and the reverse
        QCOMPARE(host.applyPresetAtEdge(project, ids[1], true,
                     QStringLiteral("plugin:dev.kinoklip.animation-pack/fade_in")),
            0);
        QVERIFY(PluginRegistry::instance().uninstall(
            QStringLiteral("dev.kinoklip.transition-presets"), &error));
    }

    void scriptsGetOnlyWhatTheirPermissionsAllow()
    {
        Project project;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.duration = 30;
        project.beginEdit(QStringLiteral("Add Clip"));
        editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        ActionRegistry actions;
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        LiveTarget target;
        target.project = &project;
        PluginHost host(ctx, target, nullptr);
        host.setQuiet(true);
        const auto errorCode = [](const QJsonObject& reply) {
            return reply.value(QStringLiteral("error"))
                .toObject()
                .value(QStringLiteral("code"))
                .toString();
        };
        const auto run
            = [&](const QStringList& perms, const QString& command, const QJsonObject& args) {
                  return host.executeForScript(perms, command, args);
              };
        // Reading needs project.read.
        QCOMPARE(errorCode(run({}, "clip.list", {})), QString("permission_denied"));
        QVERIFY(run({"project.read"}, "clip.list", {})["ok"].toBool());
        // Changing needs project.edit; project.read is not enough.
        const QJsonObject marker {{"at", 5}, {"name", "m"}};
        QCOMPARE(
            errorCode(run({"project.read"}, "marker.add", marker)), QString("permission_denied"));
        QVERIFY(run({"project.edit"}, "marker.add", marker)["ok"].toBool());
        QCOMPARE(project.timeline()->data().markers.size(), 1);
        // Commands without a project are free; unknown ones are errors, never crashes.
        QVERIFY(run({}, "effect.list", {})["ok"].toBool());
        QCOMPARE(errorCode(run({"project.edit"}, "nope.nope", {})), QString("unknown_command"));
        // Commands that only the person may run stay closed.
        QCOMPARE(errorCode(run({"project.edit"}, "settings.import", {{"file", "/x"}})),
            QString("not_allowed"));
        QVERIFY(run({}, "kk.commands", {})["ok"].toBool());
    }

    void aPluginScriptRunsEndToEndWhenTheMenuIsUsed()
    {
        if (plugins::ScriptRunner::pythonProgram().isEmpty())
            QSKIP("python3 is not installed");
        QString error;
        QVERIFY2(PluginRegistry::instance().install(
                     examplesDir() + QStringLiteral("/dev.kinoklip.script-example"), &error),
            qPrintable(error));
        Project project;
        for (int i = 0; i < 2; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.name = QStringLiteral("C%1").arg(i);
            clip.start = i * 60;
            clip.duration = 50;
            project.beginEdit(QStringLiteral("Add Clip"));
            editops::overwriteClip(
                project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
            project.endEdit();
        }
        ActionRegistry actions;
        registerStandardActions(actions);
        SelectionModel selection;
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        LiveTarget target;
        target.project = &project;
        PluginHost host(ctx, target, nullptr);
        host.setQuiet(true);
        // Installed switched off: the menu offers nothing and a run is refused.
        QMenu menu;
        host.attachScriptsMenu(&menu);
        QVERIFY(menu.actions().first()->text().contains("No scripts"));
        QVERIFY(
            !host.runScript(QStringLiteral("plugin:dev.kinoklip.script-example/mark_clip_starts")));
        // The person enables it: the entry appears and runs the script.
        PluginRegistry::instance().setEnabled(QStringLiteral("dev.kinoklip.script-example"), true);
        QCOMPARE(menu.actions().first()->text(), QString("Mark the Start of Every Clip"));
        QSignalSpy finished(host.runner(), &plugins::ScriptRunner::finished);
        menu.actions().first()->trigger();
        QVERIFY(finished.wait(30000));
        QVERIFY2(finished[0][0].toInt() == 0, qPrintable(host.runner()->output()));
        QCOMPARE(project.timeline()->data().markers.size(), 2);
        QVERIFY(host.runner()->output().contains("Added 2 marker"));
    }

    void thePluginsPageListsAndExplains()
    {
        QString error;
        QVERIFY(PluginRegistry::instance().install(
            examplesDir() + QStringLiteral("/dev.kinoklip.script-example"), &error));
        PluginsPage page;
        const QString html
            = PluginsPage::detailsHtml(QStringLiteral("dev.kinoklip.script-example"));
        QVERIFY(html.contains("Script Example"));
        QVERIFY(html.contains("Read the open project")); // the declared permissions in words
        QVERIFY(html.contains("Change the open project"));
        QVERIFY(html.contains("programs")); // the warning about scripts
        // A broken plugin shows its problems.
        writeFile(m_user + QStringLiteral("/broken/plugin.json"), R"({"id":"BROKEN"})");
        PluginRegistry::instance().reload();
        const QString broken = PluginsPage::detailsHtml(QStringLiteral("broken"));
        QVERIFY2(broken.contains("not a valid plugin id"), qPrintable(broken));
    }
};

QTEST_MAIN(TestPluginCommands)
#include "test_plugincommands.moc"
