// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the clip.stabilize command and clip.set --stabilization (spec section 58): the
// measurement of a shaky PNG-sequence clip through the command layer, progress, the stored
// result in the saved project, the options, settings-only, errors, and switching it off.
#include "../compositing/ShakyFootage.h"
#include "app/commands/Command.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Stabilization.h"
#include "core/Timeline.h"

#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::commands;
using namespace kk::test;
namespace st = kk::stabilize;

namespace {

CommandResult call(const QString& name, QJsonObject args, const ProgressFn& progress = {})
{
    return CommandRegistry::builtin().execute(name, args, progress);
}

} // namespace

class TestStabilizeCommand : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_project;
    QString m_sequence;
    QString m_clipId;
    std::unique_ptr<Project> m_loaded;

    const ClipData& clip()
    {
        m_loaded = std::make_unique<Project>();
        QString error;
        if (!m_loaded->load(m_project, &error))
            qWarning() << error;
        return m_loaded->timeline()->data().videoTracks[0].clips[0];
    }
    CommandResult ok(const QString& name, QJsonObject args, const ProgressFn& progress = {})
    {
        args.insert(QStringLiteral("project"), m_project);
        const CommandResult r = call(name, args, progress);
        [&] { QVERIFY2(r.ok, qPrintable(name + ": " + r.errorCode + " " + r.error)); }();
        return r;
    }
    void bad(const QString& name, QJsonObject args, const QString& code)
    {
        args.insert(QStringLiteral("project"), m_project);
        const CommandResult r = call(name, args);
        QVERIFY2(!r.ok, qPrintable(name + " should fail"));
        QCOMPARE(r.errorCode, code);
        QVERIFY(!r.error.isEmpty());
    }

private slots:
    void initTestCase()
    {
        ShakeSettings shake;
        shake.frames = 24;
        shake.jitterPx = 3.0;
        const QImage big = texturedPicture(900, 560, 5);
        std::vector<QImage> frames;
        for (const CameraPose& p : shakyPath(shake))
            frames.push_back(shakyFrame(big, 320, 180, p));
        const QString pattern = writeSequence(m_dir.filePath(QStringLiteral("seq")), frames);
        // A real video file (lossless), as a user's clip would be.
        m_sequence = m_dir.filePath(QStringLiteral("shaky.mkv"));
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-y"), QStringLiteral("-framerate"), QStringLiteral("25"),
                QStringLiteral("-i"), pattern.section(QLatin1Char('#'), 0, 0),
                QStringLiteral("-c:v"), QStringLiteral("ffv1"), m_sequence});
        QVERIFY(ff.waitForFinished(60000) && ff.exitCode() == 0);
    }

    void init()
    {
        m_project = m_dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(call(QStringLiteral("project.new"),
            {{"project", m_project}, {"force", true}, {"width", 320}, {"height", 180}, {"fps", 25}})
                    .ok);
        const CommandResult imported = ok("media.import", {{"file", QJsonArray {m_sequence}}});
        const QString mediaId
            = imported.data["imported"].toArray().first().toObject()["id"].toString();
        QVERIFY(!mediaId.isEmpty());
        const CommandResult added = ok("clip.add", {{"media", mediaId}});
        m_clipId
            = QString::number(added.data["clips"].toArray().first().toObject()["id"].toInteger());
    }

    void commandIsListed()
    {
        const Command* c = CommandRegistry::builtin().find(QStringLiteral("clip.stabilize"));
        QVERIFY(c);
        QStringList names;
        for (const ParamSpec& p : CommandRegistry::fullParams(c->spec))
            names << p.name;
        for (const char* expected : {"clip", "mode", "smooth", "strength", "cropping-ratio", "zoom",
                 "camera-lock", "settings-only", "project"})
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
        QVERIFY(CommandRegistry::describe(c->spec)["modifiesProject"].toBool());
    }

    // Measures the clip, reports progress, stores settings and data in the project file.
    void stabilizesAClip()
    {
        int calls = 0;
        double lastFraction = 0;
        const CommandResult r = ok("clip.stabilize",
            {{"clip", m_clipId}, {"mode", "translation"}, {"smooth", 0.5}, {"strength", 0.8},
                {"cropping-ratio", 0.5}, {"zoom", true}, {"camera-lock", false}},
            [&](double fraction, const QString& message) {
                ++calls;
                QVERIFY(fraction >= lastFraction && fraction <= 1.0);
                QVERIFY(!message.isEmpty());
                lastFraction = fraction;
            });
        QCOMPARE(calls, 24);
        QCOMPARE(lastFraction, 1.0);
        QCOMPARE(r.data["frames"].toInteger(), 24);
        QCOMPARE(r.data["lostPairs"].toInt(), 0);
        QVERIFY(r.text.contains(QStringLiteral("24 frames")));
        const ClipData& c = clip();
        QVERIFY(c.stabilization.settings.enabled);
        QCOMPARE(c.stabilization.settings.mode, st::Mode::Translation);
        QCOMPARE(c.stabilization.settings.smooth, 0.5);
        QCOMPARE(c.stabilization.settings.strength, 0.8);
        QCOMPARE(c.stabilization.settings.croppingRatio, 0.5);
        QVERIFY(c.stabilization.settings.zoom);
        QVERIFY(!c.stabilization.settings.cameraLock);
        QVERIFY(c.stabilization.data);
        QCOMPARE(c.stabilization.data->frames(), int64_t(24));
        QVERIFY(st::isActive(c.stabilization));
        // clip.list reports it.
        const QJsonObject listed = ok("clip.list", {})
                                       .data["clips"]
                                       .toArray()
                                       .first()
                                       .toObject()["stabilization"]
                                       .toObject();
        QVERIFY(listed["analysed"].toBool());
        QCOMPARE(listed["mode"].toString(), QStringLiteral("translation"));
    }

    // Options not given keep the clip's settings; settings-only changes them without measuring.
    void settingsOnlyKeepsTheMeasurement()
    {
        ok("clip.stabilize", {{"clip", m_clipId}, {"mode", "similarity"}, {"smooth", 0.9}});
        const auto data = clip().stabilization.data;
        QVERIFY(data);
        int calls = 0;
        ok("clip.stabilize",
            {{"clip", m_clipId}, {"settings-only", true}, {"camera-lock", true},
                {"strength", -0.5}},
            [&](double, const QString&) { ++calls; });
        QCOMPARE(calls, 0);
        const ClipData& c = clip();
        QVERIFY(c.stabilization.settings.cameraLock);
        QCOMPARE(c.stabilization.settings.strength, -0.5);
        QCOMPARE(c.stabilization.settings.smooth, 0.9); // kept
        QCOMPARE(c.stabilization.settings.mode, st::Mode::Similarity);
        QVERIFY(*c.stabilization.data == *data);
        // A different mode has no measurement to reuse.
        bad("clip.stabilize",
            {{"clip", m_clipId}, {"settings-only", true}, {"mode", "perspective"}},
            QStringLiteral("not_analysed"));
    }

    // clip.set --stabilization off keeps the data and makes the clip render as before; on again.
    void clipSetSwitchesItOff()
    {
        ok("clip.stabilize", {{"clip", m_clipId}, {"mode", "translation"}});
        QVERIFY(st::isActive(clip().stabilization));
        ok("clip.set", {{"clip", m_clipId}, {"stabilization", "off"}});
        QVERIFY(!clip().stabilization.settings.enabled);
        QVERIFY(clip().stabilization.data);
        QVERIFY(!st::isActive(clip().stabilization));
        ok("clip.set", {{"clip", m_clipId}, {"stabilization", "on"}});
        QVERIFY(st::isActive(clip().stabilization));
        bad("clip.set", {{"clip", m_clipId}, {"stabilization", "maybe"}},
            QStringLiteral("bad_value"));
    }

    void errors()
    {
        bad("clip.stabilize", {{"clip", 999999}}, QStringLiteral("unknown_clip"));
        bad("clip.stabilize", {{"clip", m_clipId}, {"mode", "wobble"}},
            QStringLiteral("bad_value"));
        bad("clip.stabilize", {{"clip", m_clipId}, {"smooth", 2.0}},
            QStringLiteral("out_of_range"));
        bad("clip.stabilize", {{"clip", m_clipId}, {"strength", -1.5}},
            QStringLiteral("out_of_range"));
        bad("clip.stabilize", {{"clip", m_clipId}, {"cropping-ratio", 0.1}},
            QStringLiteral("out_of_range"));
        bad("clip.stabilize", {{"clip", m_clipId}, {"settings-only", true}},
            QStringLiteral("not_analysed"));
        QVERIFY(clip().stabilization.isDefault()); // nothing was changed by the failures
        // A title clip is not a video file.
        ok("title.add", {{"text", "Hello"}});
        const QJsonArray all = ok("clip.list", {}).data["clips"].toArray();
        qint64 titleId = 0;
        for (const QJsonValue& v : all) {
            if (v.toObject()["kind"].toString() == QLatin1String("title"))
                titleId = v.toObject()["id"].toInteger();
        }
        QVERIFY(titleId != 0);
        bad("clip.stabilize", {{"clip", titleId}}, QStringLiteral("cannot_stabilize"));
    }

    // A freeze frame has no movement to measure: refused with a message.
    void freezeFrameIsRefused()
    {
        Project p;
        QString error;
        QVERIFY(p.load(m_project, &error));
        p.beginEdit(QStringLiteral("Freeze"));
        editops::clipById(p.timeline()->mutableData(), quint64(m_clipId.toLongLong()))->speed = 0.0;
        p.endEdit();
        QVERIFY(p.save(m_project));
        const QString projectCopy = m_project;
        QJsonObject args {{"project", projectCopy}, {"clip", m_clipId}};
        const CommandResult r = call(QStringLiteral("clip.stabilize"), args);
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QStringLiteral("cannot_stabilize"));
        QVERIFY(r.error.contains(QStringLiteral("freeze")));
    }
};

QTEST_MAIN(TestStabilizeCommand)
#include "test_stabilizecommand.moc"
