// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin system without a GPU (docs/behavior-spec.md section 157): manifest parsing and every
// validation message, the .kkplugin package (store and deflate, hostile names), install /
// uninstall / enable / reload with the registry, descriptors in the EffectRegistry, a missing
// plugin in a saved project, the shader lint and compile-log parser, preset recipes, the script
// bridge and the schema file.
#include "core/EffectRegistry.h"
#include "core/Serialization.h"
#include "core/TimelineData.h"
#include "plugins/PluginManifest.h"
#include "plugins/PluginRegistry.h"
#include "plugins/PresetRecipe.h"
#include "plugins/ScriptRunner.h"
#include "plugins/ZipArchive.h"
#include "render/PluginShader.h"
#include "ui/common/Resources.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::plugins;

namespace {

QString sourceDir()
{
    return QFileInfo(QStringLiteral(__FILE__)).absolutePath();
}
QString examplesDir()
{
    return sourceDir() + QStringLiteral("/../../plugins/examples");
}

QByteArray manifestJson(const QString& extra = {}, const QString& effects = {})
{
    const QString fx = effects.isEmpty()
        ? QStringLiteral(R"("effects":[{"id":"fx","name":"Fx","file":"fx.glsl"}])")
        : effects;
    return (
        QStringLiteral(
            R"({"id":"test.pack","name":"Pack","version":"1.0.0","author":"A","license":"MIT","api":1,)")
        + fx + extra + QStringLiteral("}"))
        .toUtf8();
}

bool hasProblem(const ParseResult& r, const QString& where, const QString& containing = {})
{
    for (const Problem& p : r.problems) {
        if (p.where == where && p.message.contains(containing))
            return true;
    }
    return false;
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

const char kShader[] = "vec4 effect(vec2 uv)\n{\n    return texture(uTex, uv);\n}\n";

} // namespace

class TestPlugins : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_bundled;
    QString m_user;

    QString makePlugin(const QString& root, const QString& id, const QByteArray& manifest,
        const QByteArray& shader = kShader)
    {
        const QString dir = root + QLatin1Char('/') + id;
        writeFile(dir + QStringLiteral("/plugin.json"), manifest);
        writeFile(dir + QStringLiteral("/fx.glsl"), shader);
        return dir;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_plugins"));
        QSettings().clear();
        kk::initResources();
        QVERIFY(m_dir.isValid());
        m_bundled = m_dir.filePath(QStringLiteral("bundled"));
        m_user = m_dir.filePath(QStringLiteral("user"));
        QDir().mkpath(m_bundled);
        QDir().mkpath(m_user);
    }

    void init()
    {
        QDir(m_user).removeRecursively();
        QDir(m_bundled).removeRecursively();
        QDir().mkpath(m_user);
        QDir().mkpath(m_bundled);
        QSettings().clear();
        PluginRegistry::instance().setDirectories(m_bundled, m_user);
        PluginRegistry::instance().reload();
    }

    // ---- Manifest ----

    void validManifestParses()
    {
        const ParseResult r = parseManifest(manifestJson(QStringLiteral(
            R"(,"permissions":["project.read"],"presets":[{"id":"p","name":"P","file":"p.json"}])")));
        QVERIFY2(r.ok, qPrintable(r.problems.isEmpty() ? QString() : r.problems[0].text()));
        QCOMPARE(r.manifest.id, QString("test.pack"));
        QCOMPARE(r.manifest.effects.size(), 1);
        QCOMPARE(r.manifest.effects[0].fullId, QString("plugin:test.pack/fx"));
        QCOMPARE(r.manifest.effects[0].entry, QString("main")); // the default
        QCOMPARE(r.manifest.kinds(), (QStringList {"effect", "preset"}));
        QCOMPARE(r.manifest.permissions, QStringList {"project.read"});
    }

    void everyParameterTypeParses()
    {
        const ParseResult r = parseManifest(manifestJson(
            {}, QStringLiteral(R"("effects":[{"id":"fx","name":"Fx","file":"fx.glsl","params":[
            {"key":"a","type":"number","min":0,"max":10,"default":5,"step":0.5,"unit":"px"},
            {"key":"b","type":"integer","min":1,"max":9,"default":3,"keyframable":false},
            {"key":"c","type":"boolean","default":true},
            {"key":"d","type":"color","default":"#80ff8800"},
            {"key":"e","type":"choice","choices":["x",{"value":"y","label":"Why"}],"default":"y"}]}])")));
        QVERIFY2(r.ok, qPrintable(r.problems.isEmpty() ? QString() : r.problems[0].text()));
        const auto& p = r.manifest.effects[0].params;
        QCOMPARE(p.size(), 5);
        QCOMPARE(p[0].type, ParamKind::Number);
        QCOMPARE(p[0].unit, QString("px"));
        QVERIFY(p[0].keyframable);
        QCOMPARE(p[1].type, ParamKind::Integer);
        QVERIFY(!p[1].keyframable);
        QVERIFY(p[2].boolDefault);
        QCOMPARE(p[3].colorDefault.alpha(), 0x80);
        QCOMPARE(p[4].choices, (QStringList {"x", "y"}));
        QCOMPARE(p[4].choiceLabels[1], QString("Why"));
        QCOMPARE(p[4].stringDefault, QString("y"));
    }

    void problemsNameTheField_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<QString>("where");
        QTest::addColumn<QString>("text");
        const auto fx = [](const QString& paramList) {
            return QStringLiteral(
                R"("effects":[{"id":"fx","name":"Fx","file":"fx.glsl","params":[%1]}])")
                .arg(paramList);
        };
        const QString base = QStringLiteral(
            R"({"id":"test.pack","name":"P","version":"1.0","author":"A","license":"MIT","api":1,%1})");
        QTest::newRow("missing id") << QStringLiteral(
            R"({"name":"P","version":"1.0","author":"A","license":"MIT","api":1,"effects":[]})")
                                    << "id" << "required";
        QTest::newRow("bad id") << base.arg(fx({})).replace("test.pack", "Bad Id") << "id"
                                << "not a valid plugin id";
        QTest::newRow("bad version")
            << base.arg(fx({})).replace("\"1.0\"", "\"one\"") << "version" << "not a version";
        QTest::newRow("api too new")
            << base.arg(fx({})).replace("\"api\":1", "\"api\":99") << "api" << "update Kinoklip";
        QTest::newRow("missing licence") << QStringLiteral(
            R"({"id":"test.pack","name":"P","version":"1.0","author":"A","api":1,"effects":[{"id":"a","name":"A","file":"a.glsl"}]})")
                                         << "license" << "required";
        QTest::newRow("empty") << QStringLiteral(
            R"({"id":"test.pack","name":"P","version":"1.0","author":"A","license":"MIT","api":1})")
                               << "effects" << "contains nothing";
        QTest::newRow("bad item id") << base.arg(fx({})).replace("\"fx\"", "\"Bad-Id\"")
                                     << "effects[0].id" << "not a valid id";
        QTest::newRow("missing file") << base.arg(R"("effects":[{"id":"fx","name":"Fx"}])")
                                      << "effects[0].file" << "required";
        QTest::newRow("path escape")
            << base.arg(R"("effects":[{"id":"fx","name":"Fx","file":"../x.glsl"}])")
            << "effects[0].file" << "inside the plugin folder";
        QTest::newRow("wrong suffix")
            << base.arg(R"("effects":[{"id":"fx","name":"Fx","file":"fx.txt"}])")
            << "effects[0].file" << "must end in";
        QTest::newRow("bad entry")
            << base.arg(R"("effects":[{"id":"fx","name":"Fx","file":"a.glsl","entry":"zzz"}])")
            << "effects[0].entry" << "not an entry";
        QTest::newRow("duplicate item") << base.arg(
            R"("effects":[{"id":"fx","name":"A","file":"a.glsl"},{"id":"fx","name":"B","file":"b.glsl"}])")
                                        << "effects[1].id" << "used twice";
        QTest::newRow("bad key") << base.arg(fx(R"({"key":"1x","type":"boolean","default":true})"))
                                 << "effects[0].params[0].key" << "not a valid key";
        QTest::newRow("duplicate key") << base.arg(fx(
            R"({"key":"a","type":"boolean","default":true},{"key":"a","type":"boolean","default":false})"))
                                       << "effects[0].params[1].key" << "used twice";
        QTest::newRow("bad type") << base.arg(fx(R"({"key":"a","type":"vector"})"))
                                  << "effects[0].params[0].type" << "not a parameter type";
        QTest::newRow("range inverted")
            << base.arg(fx(R"({"key":"a","type":"number","min":5,"max":1,"default":2})"))
            << "effects[0].params[0].min" << "smaller than max";
        QTest::newRow("default outside")
            << base.arg(fx(R"({"key":"a","type":"number","min":0,"max":1,"default":2})"))
            << "effects[0].params[0].default" << "outside";
        QTest::newRow("integer fraction")
            << base.arg(fx(R"({"key":"a","type":"integer","min":0,"max":9,"default":1.5})"))
            << "effects[0].params[0]" << "whole numbers";
        QTest::newRow("bad colour")
            << base.arg(fx(R"({"key":"a","type":"color","default":"nope"})"))
            << "effects[0].params[0].default" << "colour";
        QTest::newRow("choice default")
            << base.arg(fx(R"({"key":"a","type":"choice","choices":["x"],"default":"y"})"))
            << "effects[0].params[0].default" << "one of the choices";
        QTest::newRow("string in shader")
            << base.arg(fx(R"({"key":"a","type":"string","default":"x"})"))
            << "effects[0].params[0].type" << "cannot read a string";
        QTest::newRow("bad permission") << base.arg(
            R"("permissions":["root"],"effects":[{"id":"fx","name":"Fx","file":"a.glsl"}])")
                                        << "permissions" << "not a permission";
        QTest::newRow("bad target")
            << base.arg(R"("presets":[{"id":"p","name":"P","file":"p.json","target":"moon"}])")
            << "presets[0].target" << "not a target";
        QTest::newRow("script suffix")
            << base.arg(R"("scripts":[{"id":"s","name":"S","file":"s.sh"}])") << "scripts[0].file"
            << "must end in";
    }
    void problemsNameTheField()
    {
        QFETCH(QString, json);
        QFETCH(QString, where);
        QFETCH(QString, text);
        const ParseResult r = parseManifest(json.toUtf8());
        QVERIFY(!r.ok);
        QVERIFY2(hasProblem(r, where, text),
            qPrintable(
                QStringLiteral("wanted %1: %2, got: %3")
                    .arg(where, text, r.problems.isEmpty() ? QString() : r.problems[0].text())));
    }

    void jsonSyntaxErrorHasALine()
    {
        const ParseResult r = parseManifest("{\n  \"id\": \"x\",\n  \"name\": ,\n}");
        QVERIFY(!r.ok);
        QCOMPARE(r.problems[0].where, QString("plugin.json"));
        QVERIFY2(r.problems[0].line >= 3, qPrintable(QString::number(r.problems[0].line)));
        QVERIFY(r.problems[0].text().contains("line"));
    }

    void allProblemsAreReportedNotJustTheFirst()
    {
        const ParseResult r = parseManifest(
            R"({"id":"BAD","name":"","version":"x","author":"A","license":"MIT","api":1,"effects":[{"id":"fx","name":"Fx"}]})");
        QVERIFY(r.problems.size() >= 4);
    }

    void idHelpers()
    {
        QVERIFY(isValidPluginId("com.example.tint-pack"));
        QVERIFY(!isValidPluginId("Com.Example"));
        QVERIFY(!isValidPluginId("a"));
        QVERIFY(isValidItemId("my_fx_2"));
        QVERIFY(!isValidItemId("my-fx"));
        QVERIFY(isValidParamKey("amount2"));
        QVERIFY(!isValidParamKey("my_amount"));
        QString plugin, item;
        QVERIFY(splitFullId("plugin:com.example.tint/glow", &plugin, &item));
        QCOMPARE(plugin, QString("com.example.tint"));
        QCOMPARE(item, QString("glow"));
        QVERIFY(!splitFullId("gaussian_blur", nullptr, nullptr));
        QVERIFY(!isSafeRelativePath("../x"));
        QVERIFY(!isSafeRelativePath("/etc/passwd"));
        QVERIFY(!isSafeRelativePath("a/../b"));
        QVERIFY(isSafeRelativePath("shaders/a.glsl"));
    }

    void schemaFileIsValidAndMatchesTheExamples()
    {
        QFile f(sourceDir() + QStringLiteral("/../../docs/plugins/plugin.schema.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonParseError err;
        const QJsonObject schema = QJsonDocument::fromJson(f.readAll(), &err).object();
        QVERIFY2(err.error == QJsonParseError::NoError, qPrintable(err.errorString()));
        QStringList required;
        for (const QJsonValue& v : schema["required"].toArray())
            required << v.toString();
        QCOMPARE(required, (QStringList {"id", "name", "version", "author", "license", "api"}));
        // The example plugins carry every required field and the permissions the schema knows.
        const QJsonArray known = schema["properties"]["permissions"]["items"]["enum"].toArray();
        for (const QFileInfo& e :
            QDir(examplesDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile m(e.absoluteFilePath() + QStringLiteral("/plugin.json"));
            QVERIFY(m.open(QIODevice::ReadOnly));
            const QJsonObject o = QJsonDocument::fromJson(m.readAll()).object();
            for (const QString& key : required)
                QVERIFY2(o.contains(key), qPrintable(e.fileName() + " " + key));
            for (const QJsonValue& p : o["permissions"].toArray())
                QVERIFY(known.contains(p));
        }
    }

    // ---- The .kkplugin package ----

    void zipRoundTripAndHostileNames()
    {
        const QString src = m_dir.filePath(QStringLiteral("zipsrc"));
        writeFile(src + QStringLiteral("/a.txt"), "hello");
        writeFile(src + QStringLiteral("/sub/b.bin"), QByteArray(1000, '\x7f'));
        const QString zipPath = m_dir.filePath(QStringLiteral("t.zip"));
        QString error;
        QVERIFY2(zip::createFromFolder(src, zipPath, &error), qPrintable(error));
        QCOMPARE(zip::listFiles(zipPath, &error), (QStringList {"a.txt", "sub/b.bin"}));
        const QString out = m_dir.filePath(QStringLiteral("zipout"));
        QDir().mkpath(out);
        QVERIFY2(zip::extractAll(zipPath, out, &error), qPrintable(error));
        QFile b(out + QStringLiteral("/sub/b.bin"));
        QVERIFY(b.open(QIODevice::ReadOnly));
        QCOMPARE(b.readAll(), QByteArray(1000, '\x7f'));
        // A name that leaves the folder is refused (zip slip), without writing anything.
        QByteArray data;
        QVERIFY(zip::readFile(zipPath, "a.txt", &data, &error));
        QCOMPARE(data, QByteArray("hello"));
        QByteArray bytes;
        {
            QFile f(zipPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            bytes = f.readAll();
        }
        const QByteArray evil = QByteArray(bytes).replace("a.txt", "../x1");
        writeFile(m_dir.filePath(QStringLiteral("evil.zip")), evil);
        QVERIFY(!zip::extractAll(m_dir.filePath(QStringLiteral("evil.zip")), out, &error));
        QVERIFY2(error.contains("unsafe"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("x1"))));
        // A damaged archive and a wrong checksum are refused.
        QByteArray damaged = bytes;
        damaged[damaged.indexOf("hello")] = 'J';
        writeFile(m_dir.filePath(QStringLiteral("bad.zip")), damaged);
        QVERIFY(!zip::extractAll(m_dir.filePath(QStringLiteral("bad.zip")), out, &error));
        QVERIFY2(error.contains("checksum"), qPrintable(error));
        writeFile(m_dir.filePath(QStringLiteral("short.zip")), "PK");
        QVERIFY(!zip::extractAll(m_dir.filePath(QStringLiteral("short.zip")), out, &error));
    }

    void deflatedZipFromAnotherProgramIsRead()
    {
        // tests/plugins/data/deflated.kkplugin was written by Python's zipfile (deflate, level 9).
        const QString file = sourceDir() + QStringLiteral("/data/deflated.kkplugin");
        QString error;
        const QStringList names = zip::listFiles(file, &error);
        QCOMPARE(names, (QStringList {"tint-plugin/plugin.json", "tint-plugin/tint.glsl"}));
        QByteArray shader;
        QVERIFY2(zip::readFile(file, "tint-plugin/tint.glsl", &shader, &error), qPrintable(error));
        QVERIFY(shader.contains("vec4 effect(vec2 uv)"));
        QVERIFY(shader.size() > 4000); // it really was compressed
        QCOMPARE(zip::crc32("123456789"), 0xCBF43926u);
    }

    // ---- Registry: install, enable, reload ----

    void installFolderRegistersTheEffect()
    {
        const QString src = makePlugin(m_dir.filePath(QStringLiteral("src")),
            QStringLiteral("test.pack"),
            manifestJson({},
                QStringLiteral(
                    R"("effects":[{"id":"fx","name":"Tint","description":"d","group":"color","file":"fx.glsl",
                "params":[{"key":"amount","label":"Amount","type":"number","min":0,"max":100,"default":25,"unit":"%"}]}])")));
        PluginRegistry& reg = PluginRegistry::instance();
        QSignalSpy changed(&reg, &PluginRegistry::changed);
        QString error;
        QString id;
        QVERIFY2(reg.install(src, &error, &id), qPrintable(error));
        QCOMPARE(id, QString("test.pack"));
        QVERIFY(changed.count() >= 1);
        QVERIFY(QFileInfo(m_user + QStringLiteral("/test.pack/plugin.json")).isFile());
        const EffectDescriptor* d
            = EffectRegistry::instance().find(QStringLiteral("plugin:test.pack/fx"));
        QVERIFY(d);
        QCOMPARE(d->name, QString("Tint"));
        QCOMPARE(d->category, EffectCategory::VideoEffect);
        QCOMPARE(d->group, QString("color"));
        QCOMPARE(d->plugin, QString("test.pack"));
        QVERIFY(!d->unavailable);
        QCOMPARE(d->params.size(), 1);
        QCOMPARE(d->params[0].def, 25.0);
        QCOMPARE(d->params[0].unit, QString("%"));
        QVERIFY(d->params[0].animatable); // keyframable like a built-in
        // createEffect() gives the defaults, like for a built-in.
        const EffectInstance fx = EffectRegistry::instance().createEffect(d->id, 7);
        QCOMPARE(fx.params.value(QStringLiteral("amount"), 0, -1.0), 25.0);
        QVERIFY(EffectRegistry::instance().byCategory(EffectCategory::VideoEffect).contains(d));
        // The shader is there for the renderer.
        const auto spec = reg.shaderSpec(d->id);
        QVERIFY(spec);
        QVERIFY(spec->source.contains("vec4 effect"));
    }

    void installKkpluginFile()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        QString id;
        QVERIFY2(reg.install(sourceDir() + QStringLiteral("/data/deflated.kkplugin"), &error, &id),
            qPrintable(error));
        QCOMPARE(id, QString("test.deflated"));
        QVERIFY(EffectRegistry::instance().find(QStringLiteral("plugin:test.deflated/tint")));
        // Pack it again and install the result somewhere else: the same plugin.
        const QString packed = m_dir.filePath(QStringLiteral("again.kkplugin"));
        QVERIFY2(reg.exportPackage(id, packed, &error), qPrintable(error));
        QVERIFY(reg.uninstall(id, &error));
        QVERIFY(!QFileInfo::exists(m_user + QStringLiteral("/test.deflated")));
        QVERIFY2(reg.install(packed, &error, &id), qPrintable(error));
        QCOMPARE(id, QString("test.deflated"));
    }

    void installRefusesBrokenPluginsWithMessages()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        // A shader file the manifest names is missing.
        const QString src = m_dir.filePath(QStringLiteral("broken"));
        writeFile(src + QStringLiteral("/plugin.json"), manifestJson());
        QVERIFY(!reg.install(src, &error));
        QVERIFY2(error.contains("fx.glsl") && error.contains("does not exist"), qPrintable(error));
        // A manifest error.
        writeFile(src + QStringLiteral("/plugin.json"), R"({"id":"x"})");
        QVERIFY(!reg.install(src, &error));
        QVERIFY2(error.contains("name") && error.contains("required"), qPrintable(error));
        // Not a plugin at all.
        QVERIFY(!reg.install(m_dir.filePath(QStringLiteral("nothing")), &error));
        writeFile(m_dir.filePath(QStringLiteral("junk.kkplugin")),
            "not a zip at all, but long enough to be looked at");
        QVERIFY(!reg.install(m_dir.filePath(QStringLiteral("junk.kkplugin")), &error));
        QVERIFY2(error.contains("not a usable"), qPrintable(error));
        // Nothing was left in the plugin folder.
        QVERIFY(QDir(m_user).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void installReplacesAndUninstallRemoves()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        const QString v1 = makePlugin(
            m_dir.filePath(QStringLiteral("v1")), QStringLiteral("test.pack"), manifestJson());
        QVERIFY2(reg.install(v1, &error), qPrintable(error));
        // Version 2 has another effect name; installing it replaces the folder.
        const QString v2 = makePlugin(m_dir.filePath(QStringLiteral("v2")),
            QStringLiteral("test.pack"), manifestJson().replace("\"Fx\"", "\"Fx Two\""));
        QVERIFY2(reg.install(v2, &error), qPrintable(error));
        QCOMPARE(EffectRegistry::instance().find(QStringLiteral("plugin:test.pack/fx"))->name,
            QString("Fx Two"));
        QCOMPARE(reg.plugins().size(), 1);
        QVERIFY2(reg.uninstall(QStringLiteral("test.pack"), &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(m_user + QStringLiteral("/test.pack")));
        // The descriptor stays findable (projects keep their effects) but is not offered.
        const EffectDescriptor* d
            = EffectRegistry::instance().find(QStringLiteral("plugin:test.pack/fx"));
        QVERIFY(d);
        QVERIFY(d->unavailable);
        QVERIFY(!EffectRegistry::instance().byCategory(EffectCategory::VideoEffect).contains(d));
        QVERIFY(!reg.shaderSpec(QStringLiteral("plugin:test.pack/fx")));
        QVERIFY(!reg.uninstall(QStringLiteral("test.pack"), &error));
    }

    void disableAndEnable()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        QVERIFY(reg.install(makePlugin(m_dir.filePath(QStringLiteral("s")),
                                QStringLiteral("test.pack"), manifestJson()),
            &error));
        const QString id = QStringLiteral("plugin:test.pack/fx");
        QVERIFY(reg.setEnabled(QStringLiteral("test.pack"), false));
        QVERIFY(EffectRegistry::instance().find(id)->unavailable);
        QVERIFY(!reg.shaderSpec(id));
        QVERIFY(reg.setEnabled(QStringLiteral("test.pack"), true));
        QVERIFY(!EffectRegistry::instance().find(id)->unavailable);
        QVERIFY(reg.shaderSpec(id));
        QVERIFY(!reg.setEnabled(QStringLiteral("no.such"), true));
        // The choice survives a reload.
        reg.setEnabled(QStringLiteral("test.pack"), false);
        reg.reload();
        QVERIFY(!reg.find(QStringLiteral("test.pack"))->enabled);
    }

    void reloadPicksUpAnEditedShaderAndAnotherGeneration()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        QVERIFY(reg.install(makePlugin(m_dir.filePath(QStringLiteral("s")),
                                QStringLiteral("test.pack"), manifestJson()),
            &error));
        const QString id = QStringLiteral("plugin:test.pack/fx");
        const quint64 generation = reg.generation();
        const QByteArray keyBefore = reg.contentKey();
        QVERIFY(reg.shaderSpec(id)->source.contains("texture(uTex, uv)"));
        writeFile(m_user + QStringLiteral("/test.pack/fx.glsl"),
            "vec4 effect(vec2 uv) { return vec4(1.0); }\n");
        reg.reload();
        QVERIFY(reg.generation() > generation); // the renderer drops its compiled shaders
        QVERIFY(reg.shaderSpec(id)->source.contains("vec4(1.0)"));
        QVERIFY(reg.contentKey() != keyBefore); // and the render cache keys change
        // A manifest edited into an error disables the plugin and says why.
        writeFile(m_user + QStringLiteral("/test.pack/plugin.json"), R"({"id":"test.pack"})");
        reg.reload();
        const LoadedPlugin* p = reg.find(QStringLiteral("test.pack"));
        QVERIFY(p);
        QVERIFY(!p->valid);
        QVERIFY(!p->problems.isEmpty());
        QVERIFY(EffectRegistry::instance().find(id)->unavailable);
    }

    void bundledPluginsCannotBeRemovedAndIdsDoNotClash()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        makePlugin(m_bundled, QStringLiteral("test.bundled"),
            manifestJson().replace("test.pack", "test.bundled"));
        reg.reload();
        const LoadedPlugin* p = reg.find(QStringLiteral("test.bundled"));
        QVERIFY(p && p->valid);
        QCOMPARE(p->source, PluginSource::Bundled);
        QString error;
        QVERIFY(!reg.uninstall(QStringLiteral("test.bundled"), &error));
        QVERIFY2(error.contains("bundled"), qPrintable(error));
        // A user plugin with the id of a bundled one is refused.
        const QString src = makePlugin(m_dir.filePath(QStringLiteral("clash")),
            QStringLiteral("test.bundled"), manifestJson().replace("test.pack", "test.bundled"));
        QVERIFY(!reg.install(src, &error));
        QVERIFY2(error.contains("bundled"), qPrintable(error));
        // But it can be switched off.
        QVERIFY(reg.setEnabled(QStringLiteral("test.bundled"), false));
    }

    void pluginsWithScriptsStartSwitchedOff()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        const QString src = m_dir.filePath(QStringLiteral("scripty"));
        writeFile(src + QStringLiteral("/plugin.json"),
            R"({"id":"test.script","name":"S","version":"1.0","author":"A","license":"MIT","api":1,
                "permissions":["project.read"],"scripts":[{"id":"s","name":"Run","file":"s.py","permissions":["project.read"]}]})");
        writeFile(src + QStringLiteral("/s.py"), "print('hi')\n");
        QString error;
        QVERIFY2(reg.install(src, &error), qPrintable(error));
        const LoadedPlugin* p = reg.find(QStringLiteral("test.script"));
        QVERIFY(p->valid);
        QVERIFY2(
            !p->enabled, "a plugin with scripts must not run anything until the person enables it");
        QVERIFY(reg.scripts().isEmpty());
        reg.setEnabled(QStringLiteral("test.script"), true);
        QCOMPARE(reg.scripts().size(), 1);
        QCOMPARE(reg.scripts()[0].def.fullId, QString("plugin:test.script/s"));
    }

    void presetRecipeIsFoundAndChecked()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        const QString src = m_dir.filePath(QStringLiteral("presets"));
        writeFile(src + QStringLiteral("/plugin.json"),
            R"({"id":"test.presets","name":"P","version":"1.0","author":"A","license":"MIT","api":1,
                "presets":[{"id":"blur","name":"Blur","file":"blur.json"}]})");
        writeFile(src + QStringLiteral("/blur.json"),
            R"({"commands":[{"command":"effect.add","args":{"clip":"$clip","type":"gaussian_blur"}}]})");
        QString error;
        QVERIFY2(reg.install(src, &error), qPrintable(error));
        QCOMPARE(reg.presets().size(), 1);
        QVERIFY(reg.findPreset(QStringLiteral("plugin:test.presets/blur")));
        QVERIFY(!reg.presetRecipe(QStringLiteral("plugin:test.presets/blur"), &error).isEmpty());
        // A recipe that is not JSON stops the install with the line.
        writeFile(src + QStringLiteral("/blur.json"), "{\n \"commands\": [\n  oops\n ]\n}");
        QVERIFY(!reg.install(src, &error));
        QVERIFY2(error.contains("blur.json"), qPrintable(error));
    }

    // ---- A project that uses a plugin which is not there ----

    void missingPluginKeepsTheEffectData()
    {
        PluginRegistry& reg = PluginRegistry::instance();
        QString error;
        QVERIFY(reg.install(
            makePlugin(m_dir.filePath(QStringLiteral("s")), QStringLiteral("test.pack"),
                manifestJson({},
                    QStringLiteral(
                        R"("effects":[{"id":"fx","name":"Fx","file":"fx.glsl","params":[{"key":"amount","type":"number","min":0,"max":10,"default":1}]}])"))),
            &error));
        const QString type = QStringLiteral("plugin:test.pack/fx");
        EffectInstance fx = EffectRegistry::instance().createEffect(type, 11);
        fx.params.setBase(QStringLiteral("amount"), 7.5);
        fx.params.animated(QStringLiteral("amount"), 7.5).setKey(10, 2.0);
        ClipData clip;
        clip.id = 1;
        clip.kind = ClipKind::Solid;
        clip.duration = 50;
        clip.effects.append(fx);
        const QJsonObject json = serialization::clipToJson(clip);
        // The plugin goes away: uninstalled, then a reload (a new session would not know it).
        QVERIFY(reg.uninstall(QStringLiteral("test.pack"), &error));
        const ClipData back = serialization::clipFromJson(json);
        QCOMPARE(back.effects.size(), 1);
        QCOMPARE(back.effects[0].type, type);
        QCOMPARE(back.effects[0].params.value(QStringLiteral("amount"), 10, -1.0), 2.0);
        QVERIFY(back.effects[0].params.isAnimated());
        // It saves again unchanged, and the registry says "unavailable" instead of crashing.
        QCOMPARE(serialization::clipToJson(back), json);
        const EffectDescriptor* d = EffectRegistry::instance().find(type);
        QVERIFY(!d || d->unavailable);
        // Reinstalling brings it back to life with the same settings.
        QVERIFY(reg.install(m_dir.filePath(QStringLiteral("s/test.pack")), &error));
        QVERIFY(!EffectRegistry::instance().find(type)->unavailable);
    }

    // ---- Shader text ----

    void composedShaderDeclaresParametersAndResetsTheLineNumbers()
    {
        PluginShaderSpec spec;
        spec.fullId = QStringLiteral("plugin:t/e");
        spec.entry = QStringLiteral("main");
        spec.file = QStringLiteral("e.glsl");
        spec.source = QStringLiteral("vec4 effect(vec2 uv) { return vec4(u_amount); }\n");
        PluginParam p;
        p.key = QStringLiteral("amount");
        p.type = ParamKind::Number;
        PluginParam c;
        c.key = QStringLiteral("tint");
        c.type = ParamKind::Color;
        PluginParam n;
        n.key = QStringLiteral("count");
        n.type = ParamKind::Integer;
        PluginParam b;
        b.key = QStringLiteral("on");
        b.type = ParamKind::Boolean;
        PluginParam m;
        m.key = QStringLiteral("mode");
        m.type = ParamKind::Choice;
        spec.params = {p, c, n, b, m};
        const QString text = QString::fromStdString(render::pluginshader::composeEffect(spec));
        QVERIFY(text.contains("uniform float u_amount;"));
        QVERIFY(text.contains("uniform vec4 u_tint;"));
        QVERIFY(text.contains("uniform int u_count;"));
        QVERIFY(text.contains("uniform bool u_on;"));
        QVERIFY(text.contains("uniform int u_mode;"));
        QVERIFY(text.contains("#line 1\nvec4 effect(vec2 uv)"));
        QVERIFY(text.contains("void main() { fragColor = effect(vUv); }"));
        // The colour entry adds the wrapper of the built-in colour effects.
        spec.entry = QStringLiteral("color");
        QVERIFY(QString::fromStdString(render::pluginshader::composeEffect(spec))
                    .contains("straight(t)"));
        spec.transition = true;
        const QString tr = QString::fromStdString(render::pluginshader::compose(spec));
        QVERIFY(tr.contains("getFromColor") && tr.contains("transition(vec2(vUv.x, 1.0 - vUv.y))"));
    }

    void lintFindsTheRulesOfBothApis()
    {
        PluginShaderSpec spec;
        spec.fullId = QStringLiteral("plugin:t/e");
        spec.entry = QStringLiteral("main");
        spec.file = QStringLiteral("e.glsl");
        spec.source = QStringLiteral("// patch in a comment is fine\n"
                                     "#version 330 core\n"
                                     "uniform float u_amount;\n"
                                     "vec4 effect(vec2 uv)\n"
                                     "{\n"
                                     "    float patch = 1.0;\n"
                                     "    float k = 1;\n"
                                     "    vec4 c = texture2D(uTex, uv);\n"
                                     "    return c;\n"
                                     "}\n"
                                     "void main() { }\n");
        const QVector<Problem> problems = render::pluginshader::lint(spec);
        const auto line = [&](int n, const QString& text) {
            for (const Problem& p : problems) {
                if (p.line == n && p.message.contains(text))
                    return true;
            }
            return false;
        };
        QVERIFY(line(2, "#version"));
        QVERIFY(line(3, "u_amount"));
        QVERIFY(line(6, "patch")); // the usual ES 3 accident
        QVERIFY(line(7, "int to a float"));
        QVERIFY(line(8, "texture2D"));
        QVERIFY(line(11, "main()"));
        QVERIFY(!line(1, "patch")); // comments are not code
        // A clean shader and a missing entry function.
        spec.source = QString::fromLatin1(kShader);
        QVERIFY(render::pluginshader::lint(spec).isEmpty());
        spec.source = QStringLiteral("vec3 foo() { return vec3(0.0); }\n");
        const auto missing = render::pluginshader::lint(spec);
        QVERIFY(!missing.isEmpty());
        QVERIFY(missing[0].message.contains("vec4 effect(vec2 uv)"));
    }

    void compileLogsOfEveryDriverBecomeLines()
    {
        using render::pluginshader::parseCompileLog;
        const auto mesa = parseCompileLog(
            QStringLiteral("plugin:t/e: 0:12(5): error: 'foo' undeclared\n"), "e.glsl", 20);
        QCOMPARE(mesa.size(), 1);
        QCOMPARE(mesa[0].line, 12);
        QVERIFY(mesa[0].message.contains("undeclared"));
        const auto nvidia = parseCompileLog(
            QStringLiteral("0(7) : error C1008: undefined variable \"x\"\n"), "e.glsl", 20);
        QCOMPARE(nvidia[0].line, 7);
        const auto angle = parseCompileLog(
            QStringLiteral("ERROR: 0:3: 'x' : undeclared identifier\n"), "e.glsl", 20);
        QCOMPARE(angle[0].line, 3);
        // Beyond the author's file: the code Kinoklip appends; the hint says what to check.
        const auto beyond = parseCompileLog(
            QStringLiteral("0:99(1): error: no matching function effect\n"), "e.glsl", 10);
        QCOMPARE(beyond[0].line, 0);
        QVERIFY(beyond[0].message.contains("entry function"));
    }

    // ---- Preset recipes ----

    void recipeExpandsPlaceholders()
    {
        const QJsonObject recipe = QJsonDocument::fromJson(R"({"commands":[
            {"command":"effect.add","args":{"clip":"$clip","type":"blur","set":["radius=$duration"]}},
            {"command":"keyframe.set","args":{"clip":"$clip","effect":"$last_effect","at":"$end-24","value":"1"}}]})")
                                       .object();
        RecipeTarget t;
        t.clip = 12;
        t.start = 100;
        t.end = 199;
        t.duration = 100;
        QString error;
        const QVector<RecipeStep> steps = expandRecipe(recipe, t, &error);
        QCOMPARE(steps.size(), 2);
        QCOMPARE(steps[0].args["clip"].toInt(), 12); // a number, not "12"
        QCOMPARE(
            steps[0].args["set"].toArray()[0].toString(), QString("radius=100")); // inside text
        QCOMPARE(steps[1].args["at"].toInt(), 175);
        QCOMPARE(steps[1].args["effect"].toString(), QString("$last_effect")); // for the runner
        QVERIFY(usesLastEffect(steps[1].args));
        QCOMPARE(substituteResult(steps[1].args, 33)["effect"].toInt(), 33);
        QVERIFY(expandRecipe(QJsonObject {}, t, &error).isEmpty());
        QVERIFY(error.contains("commands"));
    }

    void editPointRecipeKnowsBothSidesOfTheCut()
    {
        const QJsonObject recipe = QJsonDocument::fromJson(R"({"commands":[
            {"command":"transition.add","args":{"clip":"$right","edge":"head","duration":"$edit"}},
            {"command":"clip.set","args":{"clip":"$left","note":"cut at $edit"}}]})")
                                       .object();
        RecipeTarget t;
        t.edgePoint = true;
        t.left = 5;
        t.right = 6;
        t.edit = 240;
        QString error;
        const QVector<RecipeStep> steps = expandRecipe(recipe, t, &error);
        QCOMPARE(steps.size(), 2);
        QCOMPARE(steps[0].args["clip"].toInt(), 6);
        QCOMPARE(steps[1].args["clip"].toInt(), 5);
        QCOMPARE(steps[0].args["duration"].toInt(), 240);
        QCOMPARE(steps[1].args["note"].toString(), QString("cut at 240"));
        t.left = 0; // the first clip of a track: nothing on the left
        QVERIFY(expandRecipe(recipe, t, &error).isEmpty());
        QVERIFY2(error.contains("left"), qPrintable(error));
        // the manifest accepts the target
        const ParseResult r = parseManifest(manifestJson(QStringLiteral(
            R"(,"presets":[{"id":"p","name":"P","file":"p.json","target":"edit_point"}])")));
        QVERIFY2(r.ok, qPrintable(r.problems.isEmpty() ? QString() : r.problems[0].text()));
        QVERIFY(r.manifest.presets[0].target == PresetTarget::EditPoint);
    }

    // ---- Scripts ----

    void scriptRunsThroughTheBridgeAndPermissionsAreTheExecutors()
    {
        if (ScriptRunner::pythonProgram().isEmpty())
            QSKIP("python3 is not installed");
        const QString dir = m_dir.filePath(QStringLiteral("scriptplugin"));
        writeFile(dir + QStringLiteral("/s.py"),
            "import kinoklip\n"
            "r = kinoklip.call('clip.list')\n"
            "print('clips', len(r['clips']))\n"
            "try:\n"
            "    kinoklip.call('clip.delete', clip=5)\n"
            "except kinoklip.CommandError as e:\n"
            "    print('denied', e.code)\n"
            "print('names', len(kinoklip.commands()))\n"
            "import sys; sys.exit(3)\n");
        QStringList seen;
        ScriptRunner runner([&seen](const QString& command, const QJsonObject& args) {
            seen << command;
            if (command == QLatin1String("clip.list"))
                return QJsonObject {
                    {"ok", true}, {"result", QJsonObject {{"clips", QJsonArray {1, 2, 3}}}}};
            if (command == QLatin1String("kk.commands"))
                return QJsonObject {{"ok", true},
                    {"result",
                        QJsonObject {{"commands", QJsonArray {QJsonObject {{"name", "a"}}}}}}};
            Q_UNUSED(args);
            return QJsonObject {{"ok", false},
                {"error", QJsonObject {{"code", "permission_denied"}, {"message", "no"}}}};
        });
        ScriptEntry entry;
        entry.def.name = QStringLiteral("T");
        entry.def.file = QStringLiteral("s.py");
        entry.dir = dir;
        QSignalSpy finished(&runner, &ScriptRunner::finished);
        QString error;
        QVERIFY2(runner.start(entry, &error), qPrintable(error));
        QVERIFY(finished.wait(20000));
        QCOMPARE(finished[0][0].toInt(), 3);
        QVERIFY(finished[0][1].toString().contains("exit code 3"));
        const QString out = runner.output();
        QVERIFY2(out.contains("clips 3"), qPrintable(out));
        QVERIFY2(out.contains("denied permission_denied"), qPrintable(out));
        QVERIFY2(out.contains("names 1"), qPrintable(out));
        QCOMPARE(seen, (QStringList {"clip.list", "clip.delete", "kk.commands"}));
        // Permissions as the host applies them.
        QVERIFY(permissionAllows({"project.read"}, "read"));
        QVERIFY(!permissionAllows({"project.read"}, "edit"));
        QVERIFY(permissionAllows({"project.edit"}, "read"));
        QVERIFY(permissionAllows({}, "none"));
        QVERIFY(!permissionAllows({}, "read"));
    }

    void scriptCannotShadowThePythonLibrary()
    {
        if (ScriptRunner::pythonProgram().isEmpty())
            QSKIP("python3 is not installed");
        // A file named json.py next to the script must not be imported (python -I).
        const QString dir = m_dir.filePath(QStringLiteral("shadow"));
        writeFile(dir + QStringLiteral("/json.py"), "raise SystemExit('planted json.py ran')\n");
        writeFile(dir + QStringLiteral("/s.py"), "import json\nprint('ok', json.dumps([1]))\n");
        ScriptRunner runner([](const QString&, const QJsonObject&) { return QJsonObject {}; });
        ScriptEntry entry;
        entry.def.file = QStringLiteral("s.py");
        entry.dir = dir;
        QSignalSpy finished(&runner, &ScriptRunner::finished);
        QString error;
        QVERIFY2(runner.start(entry, &error), qPrintable(error));
        QVERIFY(finished.wait(20000));
        QVERIFY2(runner.output().contains("ok [1]"), qPrintable(runner.output()));
    }
};

QTEST_MAIN(TestPlugins)
#include "test_plugins.moc"
