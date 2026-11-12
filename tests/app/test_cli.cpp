// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the built kinoklip binary as a process: `kinoklip <command> ...` works without a window,
// prints JSON on request, exits non-zero with a message on stderr for errors, and a project given
// as the first argument is still opened as before (checked through --help / unknown options only,
// as the window needs a display).
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

namespace {

struct Run
{
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
};

/// obj["a"]["b"] for objects: json(r, "result", "clips").
QJsonValue at(const QJsonObject& object, const QString& key, const QString& second = {})
{
    const QJsonValue v = object.value(key);
    return second.isEmpty() ? v : v.toObject().value(second);
}

/// Starts `kinoklip mcp`, writes @p input to its standard input, closes it and waits.
Run runMcp(const QByteArray& input)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("KK_NO_AUDIO"), QStringLiteral("1"));
    // Never reach for the control socket of a Kinoklip window the person may have open.
    env.insert(
        QStringLiteral("KK_CONTROL_SOCKET"), QStringLiteral("/nonexistent/kinoklip-test.sock"));
    process.setProcessEnvironment(env);
    process.start(QStringLiteral(KK_BINARY_PATH), {QStringLiteral("mcp")});
    Run r;
    if (!process.waitForStarted(30'000))
        return r;
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(120'000)) {
        process.kill();
        process.waitForFinished();
    }
    r.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    r.out = process.readAllStandardOutput();
    r.err = process.readAllStandardError();
    return r;
}

Run run(const QStringList& args)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("KK_NO_AUDIO"), QStringLiteral("1"));
    // Never reach for the control socket of a Kinoklip window the person may have open.
    env.insert(
        QStringLiteral("KK_CONTROL_SOCKET"), QStringLiteral("/nonexistent/kinoklip-test.sock"));
    process.setProcessEnvironment(env);
    process.start(QStringLiteral(KK_BINARY_PATH), args);
    Run r;
    if (!process.waitForFinished(120'000)) {
        process.kill();
        process.waitForFinished();
    }
    r.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    r.out = process.readAllStandardOutput();
    r.err = process.readAllStandardError();
    return r;
}

} // namespace

class TestCli : public QObject
{
    Q_OBJECT
private slots:
    void commandsJsonListsEveryCommand()
    {
        const Run r = run({"commands", "--json"});
        QCOMPARE(r.exitCode, 0);
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(r.out, &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(error.errorString()));
        const QJsonArray list = doc.object()["commands"].toArray();
        QStringList names;
        for (const QJsonValue& v : list) {
            const QJsonObject c = v.toObject();
            names << c["name"].toString();
            QVERIFY(!c["description"].toString().isEmpty());
            QVERIFY(c["parameters"].isArray());
        }
        for (const char* expected : {"version", "project.new", "project.info", "media.import",
                 "media.list", "media.relink", "timeline.list", "timeline.new", "timeline.select",
                 "clip.list", "clip.add", "clip.move", "clip.trim", "clip.split", "clip.delete",
                 "clip.set", "marker.add", "marker.list", "marker.delete", "transition.add",
                 "export.otio", "export.edl", "import.otio", "render", "frame.export", "frame.view",
                 "effect.list", "transition.list"})
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
        // Every listed command has usage text.
        for (const QString& name : names) {
            const Run help = run({QStringLiteral("help"), name});
            QCOMPARE(help.exitCode, 0);
            QVERIFY2(help.out.contains("Usage: kinoklip"), qPrintable(name));
        }
    }

    void projectMediaClipRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString project = dir.filePath("cli.kkproj");
        const QString media = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        Run r = run({"project", "new", "--project", project, "--width", "640", "--height", "360",
            "--fps", "24"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        QVERIFY(r.out.contains("created"));
        r = run({"media", "import", "--project", project, "--file", media, "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        const QString id = at(r.json(), "result", "imported")
                               .toArray()
                               .first()
                               .toObject()
                               .value("id")
                               .toString();
        QVERIFY(!id.isEmpty());
        r = run({"clip", "add", "--project", project, "--media", id, "--source-out", "00:00:02:00",
            "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        QCOMPARE(at(r.json(), "result", "clips").toArray().size(), 2);
        r = run({"clip", "list", "--project", project, "--track", "V1", "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        const QJsonArray clips = at(r.json(), "result", "clips").toArray();
        QCOMPARE(clips.size(), 1);
        QCOMPARE(clips[0].toObject()["duration"].toInteger(), 48);
        QCOMPARE(clips[0].toObject()["track"].toString(), QString("V1"));
        // Human output, an option with = and a negative time value.
        r = run({"clip", "trim", "--project=" + project, "--clip",
            QString::number(clips[0].toObject()["id"].toInteger()), "--edge", "tail", "--delta",
            "-8"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        QVERIFY(r.out.contains("trimmed"));
        r = run({"clip", "list", "--project", project});
        QVERIFY(r.out.contains("00:00:01:16"));
    }

    // Spec 68.15: "title layer add" is the command title.layer_add, and the MCP tool is
    // title_layer_add.
    void spec68_15_titleLayerWords()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString project = dir.filePath("layers.kkproj");
        Run r = run({"project", "new", "--project", project, "--width", "640", "--height", "360",
            "--fps", "24"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        r = run({"title", "add", "--project", project, "--text", "Hello", "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        const qint64 clip = at(r.json(), "result", "clip").toObject()["id"].toInteger();
        const QString id = QString::number(clip);
        r = run({"title", "layer", "add", "--project", project, "--clip", id, "--kind", "shape",
            "--shape", "ellipse", "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        QCOMPARE(at(r.json(), "result", "layers").toArray().size(), 2);
        const QString layer = QString::number(at(r.json(), "result", "layer").toInteger());
        r = run({"title", "layer", "move", "--project", project, "--clip", id, "--layer", layer,
            "--by", "-1"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        r = run({"title", "layer", "set", "--project", project, "--clip", id, "--layer", layer,
            "--visible", "false", "--name", "Ring", "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        r = run({"title", "layer", "list", "--project", project, "--clip", id, "--json"});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        const QJsonArray layers = at(r.json(), "result", "layers").toArray();
        QCOMPARE(layers.size(), 2);
        QCOMPARE(layers[0].toObject()["name"].toString(), QString("Ring"));
        QCOMPARE(layers[0].toObject()["visible"].toBool(), false);
        r = run({"title", "layer", "remove", "--project", project, "--clip", id, "--layer", layer});
        QVERIFY2(r.exitCode == 0, r.err.constData());
        r = run({"commands"});
        QVERIFY(r.out.contains("title layer add"));
        // The MCP server names it title_layer_add.
        const Run mcp = runMcp("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                               "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"
                               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
        QCOMPARE(mcp.exitCode, 0);
        QVERIFY2(mcp.out.contains("title_layer_add"), mcp.out.left(300).constData());
    }

    void errorsExitNonZeroWithMessageOnStderr()
    {
        Run r = run({"clip", "frobnicate"});
        QCOMPARE(r.exitCode, 2);
        QVERIFY(r.err.contains("unknown command"));
        r = run({"clip", "list", "--project", "/nonexistent/x.kkproj"});
        QCOMPARE(r.exitCode, 1);
        QVERIFY(r.err.contains("does not exist"));
        r = run({"clip", "list", "--project", "/nonexistent/x.kkproj", "--json"});
        QCOMPARE(r.exitCode, 1);
        QVERIFY(!r.json()["ok"].toBool());
        QCOMPARE(at(r.json(), "error", "code").toString(), QString("file_not_found"));
        r = run({"clip", "list"});
        QCOMPARE(r.exitCode, 2);
        QVERIFY(r.err.contains("missing parameter"));
        r = run({"clip", "list", "--nope", "1"});
        QCOMPARE(r.exitCode, 2);
        QVERIFY(r.err.contains("unknown option"));
        r = run({"help", "nothing"});
        QCOMPARE(r.exitCode, 2);
    }

    void mcpServerAnswersOnStdio()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString project = dir.filePath("mcp.kkproj");
        const QByteArray input
            = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"
              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
              "\n"
              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
              "this is not json\n"
              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"project_new\",\"arguments\":{\"project\":\""
            + project.toUtf8()
            + "\",\"fps\":25}}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"project_info\",\"arguments\":{\"project\":\""
            + project.toUtf8() + "\"}}}\n";
        const Run r = runMcp(input);
        QCOMPARE(r.exitCode, 0);
        // Standard output holds protocol messages only: every line is one JSON object.
        const QList<QByteArray> lines = r.out.split('\n');
        QList<QJsonObject> replies;
        for (const QByteArray& line : lines) {
            if (line.isEmpty())
                continue;
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            QVERIFY2(error.error == QJsonParseError::NoError && doc.isObject(), line.constData());
            replies << doc.object();
        }
        QCOMPARE(replies.size(), 5); // initialize, tools/list, parse error, two calls
        QCOMPARE(replies[0]["id"].toInt(), 1);
        QCOMPARE(
            replies[0]["result"].toObject()["protocolVersion"].toString(), QString("2025-06-18"));
        QVERIFY(replies[1]["result"].toObject()["tools"].toArray().size() >= 28);
        QCOMPARE(replies[2]["error"].toObject()["code"].toInt(), -32700);
        QVERIFY(!replies[3]["result"].toObject()["isError"].toBool());
        const QJsonObject info = replies[4]["result"].toObject();
        QVERIFY(!info["isError"].toBool());
        QCOMPARE(info["structuredContent"].toObject()["fps"].toDouble(), 25.0);
        QVERIFY(QFile::exists(project));
        QVERIFY(r.err.contains("ready")); // the log line is on stderr
    }

    void mcpRejectsOptionsAndHasHelp()
    {
        Run r = run({"mcp", "--bogus"});
        QCOMPARE(r.exitCode, 2);
        QVERIFY(r.err.contains("unknown option"));
        QVERIFY(r.out.isEmpty());
        r = run({"mcp", "--help"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.contains("Model Context Protocol"));
    }

    void versionAndExistingOptionsKeepWorking()
    {
        Run r = run({"version", "--json"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(!at(r.json(), "result", "version").toString().isEmpty());
        r = run({"--version"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.contains("Kinoklip") || r.out.contains("kinoklip"));
        r = run({"--help"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(r.out.contains("--render"));
        r = run({"--no-such-option"});
        QVERIFY(r.exitCode != 0);
    }

    void convertRateWritesANewFileAndNeverTouchesTheOriginal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString project = dir.filePath(QStringLiteral("a.kkproj"));
        const QString out = dir.filePath(QStringLiteral("a60.kkproj"));
        QCOMPARE(run({QStringLiteral("project"), QStringLiteral("new"), QStringLiteral("--project"),
                         project, QStringLiteral("--fps"), QStringLiteral("24")})
                     .exitCode,
            0);
        for (const char* at : {"0", "25"}) {
            QCOMPARE(run({QStringLiteral("generator"), QStringLiteral("add"),
                             QStringLiteral("--generator"), QStringLiteral("solid_color"),
                             QStringLiteral("--at"), QString::fromLatin1(at),
                             QStringLiteral("--duration"), QStringLiteral("25"),
                             QStringLiteral("--project"), project})
                         .exitCode,
                0);
        }
        QFile original(project);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray before = original.readAll();
        original.close();

        // The input path and an existing file are refused.
        const Run same = run({QStringLiteral("timeline"), QStringLiteral("convert-rate"),
            QStringLiteral("--fps"), QStringLiteral("60"), QStringLiteral("--project"), project,
            QStringLiteral("--output"), project});
        QVERIFY(same.exitCode != 0);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("a.kkproj"))));
        const Run missing = run({QStringLiteral("timeline"), QStringLiteral("convert-rate"),
            QStringLiteral("--fps"), QStringLiteral("60"), QStringLiteral("--project"), project});
        QVERIFY(missing.exitCode != 0); // --output is required

        const Run converted = run({QStringLiteral("timeline"), QStringLiteral("convert-rate"),
            QStringLiteral("--fps"), QStringLiteral("60"), QStringLiteral("--project"), project,
            QStringLiteral("--output"), out, QStringLiteral("--json")});
        QCOMPARE(converted.exitCode, 0);
        QVERIFY(QFile::exists(out));
        const Run list = run({QStringLiteral("clip"), QStringLiteral("list"),
            QStringLiteral("--project"), out, QStringLiteral("--json")});
        const QJsonArray clips = at(list.json(), QStringLiteral("result"), QStringLiteral("clips"))
                                     .toArray();
        QCOMPARE(clips.size(), 2);
        QCOMPARE(clips[0].toObject().value(QStringLiteral("start")).toInt(), 0);
        QCOMPARE(clips[0].toObject().value(QStringLiteral("duration")).toInt(), 63); // 62.5 rounds up
        QCOMPARE(clips[1].toObject().value(QStringLiteral("start")).toInt(), 63); // the clips still touch
        QCOMPARE(at(list.json(), QStringLiteral("result"), QStringLiteral("fps")).toInt(), 60);

        // A second run to the same output is refused; the original is byte for byte unchanged.
        QVERIFY(run({QStringLiteral("timeline"), QStringLiteral("convert-rate"),
                        QStringLiteral("--fps"), QStringLiteral("50"), QStringLiteral("--project"),
                        project, QStringLiteral("--output"), out})
                    .exitCode
            != 0);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), before);
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "test_cli.moc"
