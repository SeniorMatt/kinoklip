// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the MCP protocol handler (app/mcp/McpServer): fed with message strings, no stdio.
// initialize and version negotiation, the tool list generated from the command registry, a tool
// call that builds a project and reads it back, a failing call, malformed JSON, unknown methods,
// progress notifications, a frame returned as image content.
#include "app/commands/Command.h"
#include "app/mcp/McpServer.h"
#include "app/mcp/McpTools.h"
#include "ui/common/Resources.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::mcp;

namespace {

const QString kVideo = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4"); // 10 s

/// A JSON value that chains: j["result"]["tools"][0]["name"].toString(). (QJsonValueRef does not
/// chain with string keys in Qt 6.12.)
struct J
{
    QJsonValue v;
    J(const QJsonValue& value = {})
        : v(value)
    { }
    J(const QJsonObject& o)
        : v(o)
    { }
    operator QJsonObject() const { return v.toObject(); }
    J operator[](const char* key) const { return v.toObject().value(QLatin1String(key)); }
    J operator[](int i) const { return v.toArray().at(i); }
    bool contains(const char* key) const { return v.toObject().contains(QLatin1String(key)); }
    QString toString() const { return v.toString(); }
    int toInt() const { return v.toInt(); }
    double toDouble() const { return v.toDouble(); }
    bool toBool() const { return v.toBool(); }
    bool isObject() const { return v.isObject(); }
    bool isNull() const { return v.isNull(); }
    QJsonArray toArray() const { return v.toArray(); }
    QJsonObject toObject() const { return v.toObject(); }
};

QByteArray json(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QJsonObject request(int id, const QString& method, const QJsonObject& params = {})
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

J parse(const QByteArray& reply)
{
    return QJsonDocument::fromJson(reply).object();
}

} // namespace

class TestMcp : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_project;
    McpServer m_server {commands::CommandRegistry::builtin()};

    J rpc(const QString& method, const QJsonObject& params = {})
    {
        return parse(m_server.handle(json(request(1, method, params))));
    }
    /// tools/call; the result object (not the JSON-RPC envelope).
    J call(const QString& tool, QJsonObject arguments, bool withProject = true)
    {
        if (withProject)
            arguments.insert("project", m_project);
        const J reply = rpc("tools/call", {{"name", tool}, {"arguments", arguments}});
        [&] { QVERIFY2(reply.contains("result"), json(reply).constData()); }();
        return reply["result"].toObject();
    }
    J callOk(const QString& tool, const QJsonObject& arguments)
    {
        const J r = call(tool, arguments);
        [&] { QVERIFY2(!r["isError"].toBool(), json(r).constData()); }();
        return r["structuredContent"].toObject();
    }

private slots:
    void initTestCase()
    {
        initResources();
        QVERIFY(m_dir.isValid());
        m_project = m_dir.filePath("mcp.kkproj");
        const J init = rpc("initialize",
            {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject()},
                {"clientInfo", QJsonObject {{"name", "test"}, {"version", "1"}}}});
        QCOMPARE(init["result"]["protocolVersion"].toString(), QString("2025-06-18"));
    }

    void initializeNegotiatesTheVersion()
    {
        McpServer server(commands::CommandRegistry::builtin());
        const auto init = [&](const QString& version) {
            return parse(server.handle(json(request(1, "initialize",
                {{"protocolVersion", version}, {"capabilities", QJsonObject()}}))));
        };
        J r = init("2025-06-18");
        QCOMPARE(r["id"].toInt(), 1);
        QCOMPARE(r["result"]["protocolVersion"].toString(), QString("2025-06-18"));
        QVERIFY(r["result"]["capabilities"]["tools"].isObject());
        QCOMPARE(r["result"]["serverInfo"]["name"].toString(), QString("kinoklip"));
        QVERIFY(!r["result"]["serverInfo"]["version"].toString().isEmpty());
        QVERIFY(!r["result"]["instructions"].toString().isEmpty());
        QCOMPARE(init("2024-11-05")["result"]["protocolVersion"].toString(), QString("2024-11-05"));
        // An unknown version gets the newest one we speak; the client then decides.
        QCOMPARE(init("2030-01-01")["result"]["protocolVersion"].toString(), QString("2025-06-18"));
        // The initialized notification and ping.
        QVERIFY(server.handle(json({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}))
                    .isEmpty());
        const J ping = parse(server.handle(json(request(7, "ping"))));
        QCOMPARE(ping["id"].toInt(), 7);
        QVERIFY(ping["result"].isObject());
        QVERIFY(!ping.contains("error"));
    }

    void settingsImportIsNotForAttachedTools()
    {
        // settings.export is offered; settings.import is neither listed nor runnable.
        QStringList names;
        for (const QJsonValue& t : rpc("tools/list")["result"]["tools"].toArray())
            names << t["name"].toString();
        QVERIFY(names.contains("settings_export"));
        QVERIFY(!names.contains("settings_import"));
        const J reply = rpc("tools/call",
            {{"name", "settings_import"}, {"arguments", QJsonObject {{"file", "/nonexistent"}}}});
        QCOMPARE(reply["error"]["code"].toInt(), -32602);
    }

    void toolListCoversEveryCommandWithAValidSchema()
    {
        const QJsonArray tools = rpc("tools/list")["result"]["tools"].toArray();
        QVector<commands::Command> all; // the commands an attached tool may run
        for (const commands::Command& c : commands::CommandRegistry::builtin().all()) {
            if (c.spec.remoteAllowed)
                all.append(c);
        }
        QCOMPARE(tools.size(), all.size());
        QStringList names;
        for (qsizetype i = 0; i < tools.size(); ++i) {
            const QJsonObject tool = tools[i].toObject();
            const QString name = tool["name"].toString();
            QVERIFY2(QRegularExpression("^[a-zA-Z0-9_-]{1,64}$").match(name).hasMatch(),
                qPrintable(name));
            QCOMPARE(name, toolName(all[i].spec.name));
            QVERIFY(!names.contains(name));
            names << name;
            QVERIFY(!tool["description"].toString().isEmpty());
            const QJsonObject schema = tool["inputSchema"].toObject();
            QCOMPARE(schema["type"].toString(), QString("object"));
            const QJsonObject props = schema["properties"].toObject();
            // Every parameter of the command is a property, with a JSON Schema type.
            for (const commands::ParamSpec& p :
                commands::CommandRegistry::fullParams(all[i].spec)) {
                QVERIFY2(props.contains(p.name), qPrintable(name + "." + p.name));
                const QJsonObject prop = props[p.name].toObject();
                QVERIFY(QStringList({"string", "integer", "number", "boolean", "array"})
                            .contains(prop["type"].toString()));
                QVERIFY(!prop["description"].toString().isEmpty());
                if (p.required)
                    QVERIFY2(schema["required"].toArray().contains(p.name),
                        qPrintable(name + "." + p.name));
                if (p.type == commands::ParamType::Time)
                    QVERIFY(prop["description"].toString().contains("frames or timecode"));
                if (!p.choices.isEmpty())
                    QCOMPARE(prop["enum"].toArray().size(), p.choices.size());
            }
            for (const QJsonValue& r : schema["required"].toArray())
                QVERIFY(props.contains(r.toString()));
        }
        for (const char* expected : {"project_new", "clip_add", "clip_list", "frame_view",
                 "frame_export", "effect_list", "transition_list", "render", "version"})
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
        // clip_add: --media is required, the optional ones are not.
        for (const QJsonValue& t : tools) {
            if (t["name"].toString() != "clip_add")
                continue;
            const QJsonArray required = t["inputSchema"]["required"].toArray();
            QVERIFY(required.contains("media"));
            QVERIFY(required.contains("project"));
            QVERIFY(!required.contains("at"));
            QCOMPARE(t["inputSchema"]["properties"]["at"]["type"].toString(), QString("string"));
        }
    }

    void aCallBuildsAProjectAndReadsItBack()
    {
        J r = call("project_new",
            {{"project", m_project}, {"width", 640}, {"height", 360}, {"fps", 24}}, false);
        QVERIFY2(!r["isError"].toBool(), json(r).constData());
        // The sentence, and the JSON as text for older clients.
        QCOMPARE(r["content"][0]["type"].toString(), QString("text"));
        QVERIFY(r["content"][0]["text"].toString().contains("created"));
        QCOMPARE(parse(r["content"][1]["text"].toString().toUtf8())["width"].toInt(), 640);
        QCOMPARE(r["structuredContent"]["height"].toInt(), 360);

        const J imported = callOk("media_import", {{"file", QJsonArray {kVideo}}});
        const QString mediaId = imported["imported"][0]["id"].toString();
        QVERIFY(!mediaId.isEmpty());
        // Times as a JSON number and as timecode both work.
        const J added = callOk("clip_add", {{"media", mediaId}, {"source-out", 48}});
        QCOMPARE(added["clips"].toArray().size(), 2);
        callOk("clip_split", {{"at", "00:00:01:00"}});
        const J list = callOk("clip_list", {{"track", "V1"}});
        QCOMPARE(list["clips"].toArray().size(), 2);
        QCOMPARE(list["clips"][0]["duration"].toInt(), 24);
        const J info = callOk("project_info", {});
        QCOMPARE(info["width"].toInt(), 640);
        QVERIFY(info["timelines"][0]["clips"].toInt() >= 4);
        // The catalogue commands need no project.
        const J transitions
            = call("transition_list", {}, false)["structuredContent"].toObject();
        QVERIFY(transitions["items"].toArray().size() > 40);
        bool dissolve = false;
        for (const QJsonValue& t : transitions["items"].toArray())
            dissolve |= t["id"].toString() == "cross_dissolve";
        QVERIFY(dissolve);
        const J one
            = call("effect_list", {{"id", "gaussian_blur"}}, false)["structuredContent"].toObject();
        QCOMPARE(one["items"].toArray().size(), 1);
        QVERIFY(one["items"][0]["parameters"].toArray().size() > 0);
        QVERIFY(call("effect_list", {{"category", "audio"}}, false)["structuredContent"]["items"]
                    .toArray()
                    .size()
            > 3);
    }

    void aFailingCallIsAnErrorResultWithTheCode()
    {
        const J r = call("clip_trim", {{"clip", 999}, {"edge", "tail"}, {"delta", -1}});
        QVERIFY(r["isError"].toBool());
        QVERIFY(r["content"][0]["text"].toString().startsWith("unknown_clip"));
        QCOMPARE(r["structuredContent"]["error"]["code"].toString(), QString("unknown_clip"));
        const J missing = call("clip_list", {}, false);
        QVERIFY(missing["isError"].toBool());
        QVERIFY(missing["content"][0]["text"].toString().startsWith("missing_parameter"));
        const J unknown = call("clip_list", {{"nope", 1}});
        QVERIFY(unknown["content"][0]["text"].toString().startsWith("unknown_parameter"));
    }

    void protocolErrors()
    {
        J r = parse(m_server.handle("{not json"));
        QCOMPARE(r["error"]["code"].toInt(), -32700);
        QVERIFY(r["id"].isNull());
        QCOMPARE(parse(m_server.handle("42"))["error"]["code"].toInt(), -32700);
        r = rpc("resources/list");
        QCOMPARE(r["error"]["code"].toInt(), -32601);
        QCOMPARE(r["id"].toInt(), 1);
        QCOMPARE(
            parse(m_server.handle(json({{"id", 3}, {"method", "ping"}})))["error"]["code"].toInt(),
            -32600); // no "jsonrpc"
        r = rpc("tools/call", {{"name", "no_such_tool"}});
        QCOMPARE(r["error"]["code"].toInt(), -32602);
        r = rpc("tools/call", {{"name", "version"}, {"arguments", 5}});
        QCOMPARE(r["error"]["code"].toInt(), -32602);
        // An unknown notification is silently dropped; so is a response from the client.
        QVERIFY(m_server.handle(json({{"jsonrpc", "2.0"}, {"method", "nothing/known"}})).isEmpty());
        QVERIFY(m_server.handle(json({{"jsonrpc", "2.0"}, {"id", 9}, {"result", QJsonObject()}}))
                    .isEmpty());
        // A batch answers each request.
        const QByteArray batch
            = m_server.handle("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},"
                              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}]");
        QCOMPARE(QJsonDocument::fromJson(batch).array().size(), 1);
        // The tool without a project works with no arguments at all.
        const J v
            = parse(m_server.handle("{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/call\","
                                    "\"params\":{\"name\":\"version\"}}"));
        QCOMPARE(v["id"].toString(), QString("a"));
        QVERIFY(!v["result"]["isError"].toBool());
    }

    void oldProtocolHasNoStructuredContent()
    {
        McpServer old(commands::CommandRegistry::builtin());
        (void)old.handle(json(request(1, "initialize", {{"protocolVersion", "2024-11-05"}})));
        const J r
            = parse(old.handle(json(request(2, "tools/call", {{"name", "version"}}))))["result"]
                  .toObject();
        QVERIFY(!r.contains("structuredContent"));
        QVERIFY(r["content"].toArray().size() >= 2);
    }

    void progressGoesOutOnlyWithAToken()
    {
        QList<QJsonObject> seen;
        McpServer server(commands::CommandRegistry::builtin(),
            [](const QString&, const QJsonObject&, const commands::ProgressFn& progress) {
                if (progress) {
                    progress(0.25, "a");
                    progress(0.25, "same"); // not increasing: dropped
                    progress(1.0, "done");
                }
                return commands::CommandResult::success({}, "ok");
            });
        server.setNotifier([&](const QByteArray& m) { seen << parse(m); });
        (void)server.handle(json(request(1, "initialize", {})));
        (void)server.handle(json(request(2, "tools/call", {{"name", "render"}})));
        QVERIFY(seen.isEmpty());
        const J r = parse(server.handle(json(request(3, "tools/call",
            {{"name", "render"}, {"_meta", QJsonObject {{"progressToken", "tok"}}}}))));
        QVERIFY(!r["result"]["isError"].toBool());
        QCOMPARE(seen.size(), 2);
        QCOMPARE(seen[0]["method"].toString(), QString("notifications/progress"));
        QCOMPARE(J(seen[0])["params"]["progressToken"].toString(), QString("tok"));
        QCOMPARE(J(seen[0])["params"]["progress"].toDouble(), 0.25);
        QCOMPARE(J(seen[1])["params"]["progress"].toDouble(), 1.0);
        QVERIFY(!seen[0].contains("id"));
    }

    void aFrameComesBackAsImageContent()
    {
        const J r = call("frame_view", {{"at", 0}});
        QVERIFY2(!r["isError"].toBool(), json(r).constData());
        const QJsonArray content = r["content"].toArray();
        QJsonObject image;
        for (const QJsonValue& c : content) {
            if (c["type"].toString() == "image")
                image = c.toObject();
        }
        QCOMPARE(image["mimeType"].toString(), QString("image/png"));
        const QImage picture
            = QImage::fromData(QByteArray::fromBase64(image["data"].toString().toLatin1()));
        QVERIFY(!picture.isNull());
        QCOMPARE(picture.width(), 640); // never wider than the 640 px timeline
        QCOMPARE(picture.height(), 360);
        QCOMPARE(r["structuredContent"]["width"].toInt(), 640);
        // A narrower picture keeps the aspect ratio; the base64 never lands in the JSON text.
        const J small = call("frame_view", {{"at", "00:00:00:12"}, {"width", 320}});
        for (const QJsonValue& c : small["content"].toArray()) {
            if (c["type"].toString() == "image") {
                const QImage p
                    = QImage::fromData(QByteArray::fromBase64(c["data"].toString().toLatin1()));
                QCOMPARE(p.size(), QSize(320, 180));
            } else {
                QVERIFY(!c["text"].toString().contains("iVBOR"));
            }
        }
        QVERIFY(call("frame_view", {{"at", 100000}})["isError"].toBool());
    }
};

QTEST_MAIN(TestMcp)
#include "test_mcp.moc"
