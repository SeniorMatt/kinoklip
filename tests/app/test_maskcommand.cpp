// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the mask.follow command (spec section 188): a mask of a clip follows the textured
// subject under it, through the command layer (also the command line and the MCP server), with
// progress, one undo-able result in the saved project, and the errors.
#include "app/commands/Command.h"
#include "core/Mask.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::commands;

namespace {

CommandResult call(const QString& name, QJsonObject args, const ProgressFn& progress = {})
{
    return CommandRegistry::builtin().execute(name, args, progress);
}

} // namespace

class TestMaskCommand : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_footage;
    QString m_project;
    quint64 m_clipId = 0;
    quint64 m_maskId = 0;

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
        // A busy texture that moves 3 px right and 2 px down per frame (40 frames, lossless).
        m_footage = m_dir.filePath(QStringLiteral("plane.mkv"));
        const QString tex = QStringLiteral(
            "128+40*sin(0.35*(X-3*N)+0.2*(Y-2*N))+30*sin(0.21*(X-3*N)-0.43*(Y-2*N))"
            "+25*sin(0.67*(X-3*N)+0.11*(Y-2*N))+20*sin(0.13*(X-3*N)+0.71*(Y-2*N))");
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"), QStringLiteral("color=c=black:s=320x180:r=25"),
                QStringLiteral("-vf"), QStringLiteral("geq=lum='%1':cb=128:cr=128").arg(tex),
                QStringLiteral("-frames:v"), QStringLiteral("40"), QStringLiteral("-c:v"),
                QStringLiteral("ffv1"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                m_footage});
        QVERIFY(ff.waitForFinished(120000) && ff.exitCode() == 0);
    }

    void init()
    {
        m_project = m_dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(call(QStringLiteral("project.new"),
            {{"project", m_project}, {"force", true}, {"width", 320}, {"height", 180}, {"fps", 25}})
                    .ok);
        const CommandResult imported = ok("media.import", {{"file", QJsonArray {m_footage}}});
        const QString mediaId
            = imported.data["imported"].toArray().first().toObject()["id"].toString();
        const CommandResult added = ok("clip.add", {{"media", mediaId}});
        m_clipId = quint64(added.data["clips"].toArray().first().toObject()["id"].toInteger());
        // Draw an ellipse mask on the clip (the Inspector's Add Mask) and save.
        Project p;
        QString error;
        QVERIFY2(p.load(m_project, &error), qPrintable(error));
        TimelineData& d = p.timeline()->mutableData();
        m_maskId = d.allocateId();
        MaskData m = makeMask(MaskShape::Ellipse, m_maskId, QSizeF(90, 70));
        m.params.setBase(maskkeys::PosX, -20);
        m.params.setBase(maskkeys::PosY, 10);
        d.videoTracks[0].clips[0].masks.append(m);
        QVERIFY2(p.save(m_project, &error), qPrintable(error));
    }

    void commandIsListed()
    {
        const Command* c = CommandRegistry::builtin().find(QStringLiteral("mask.follow"));
        QVERIFY(c);
        QStringList names;
        for (const ParamSpec& p : CommandRegistry::fullParams(c->spec))
            names << p.name;
        for (const char* expected : {"clip", "mask", "at", "project"})
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
        QVERIFY(CommandRegistry::describe(c->spec)["modifiesProject"].toBool());
    }

    void clipListShowsTheMasks()
    {
        const QJsonArray masks
            = ok("clip.list", {}).data["clips"].toArray().first().toObject()["masks"].toArray();
        QCOMPARE(masks.size(), 1);
        QCOMPARE(masks.first().toObject()["id"].toInteger(), qint64(m_maskId));
        QCOMPARE(masks.first().toObject()["shape"].toString(), QStringLiteral("ellipse"));
        QVERIFY(!masks.first().toObject()["animated"].toBool());
    }

    void aMaskFollowsItsSubject()
    {
        double last = 0;
        int calls = 0;
        const CommandResult r = ok("mask.follow",
            {{"clip", qint64(m_clipId)}, {"mask", qint64(m_maskId)}, {"at", 20}},
            [&](double fraction, const QString& message) {
                ++calls;
                last = fraction;
                QVERIFY(!message.isEmpty());
            });
        QVERIFY(calls > 5);
        QVERIFY(last > 0.0);
        QVERIFY(r.data["frames"].toInteger() >= 39);
        Project p;
        QString error;
        QVERIFY2(p.load(m_project, &error), qPrintable(error));
        const MaskData& m = p.timeline()->data().videoTracks[0].clips[0].masks[0];
        QVERIFY(m.params.values.value(maskkeys::PosX).isAnimated());
        for (const int f : {0, 10, 20, 30, 39}) {
            QVERIFY2(std::abs(m.value(maskkeys::PosX, f) - (-20 + 3.0 * (f - 20))) < 2.5,
                qPrintable(QStringLiteral("x at %1: %2").arg(f).arg(m.value(maskkeys::PosX, f))));
            QVERIFY2(std::abs(m.value(maskkeys::PosY, f) - (10 + 2.0 * (f - 20))) < 2.5,
                qPrintable(QStringLiteral("y at %1: %2").arg(f).arg(m.value(maskkeys::PosY, f))));
        }
    }

    void refusals()
    {
        bad("mask.follow", {{"clip", 999999}, {"mask", qint64(m_maskId)}},
            QStringLiteral("unknown_clip"));
        bad("mask.follow", {{"clip", qint64(m_clipId)}, {"mask", 999999}},
            QStringLiteral("unknown_mask"));
        bad("mask.follow", {{"clip", qint64(m_clipId)}, {"mask", qint64(m_maskId)}, {"at", 400}},
            QStringLiteral("out_of_range"));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TestMaskCommand test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_maskcommand.moc"
