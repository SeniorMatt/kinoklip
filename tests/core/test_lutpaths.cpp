// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/LutPaths (spec 168): the LUT files of grade nodes and input LUTs are stored with
// a path relative to the project file too, so a project moved with its LUT folder keeps them.
#include "core/LutPaths.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

namespace {

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

class TestLutPaths : public QObject
{
    Q_OBJECT
private slots:
    void jsonStoresBoth()
    {
        const QDir dir(QStringLiteral("/proj/a"));
        const QJsonObject in {{QStringLiteral("nodes"),
                                  QJsonArray {QJsonObject {{QStringLiteral("lut"),
                                      QStringLiteral("/proj/a/luts/x.cube")}}}},
            {QStringLiteral("inputLut"), QStringLiteral("/proj/shared/y.cube")},
            {QStringLiteral("other"), QStringLiteral("/not/a/lut")}};
        const QJsonObject out = lutpaths::addRelative(in, dir);
        QCOMPARE(out.value(QStringLiteral("nodes")).toArray()[0].toObject().value(
                     QStringLiteral("lutRelative")).toString(),
            QStringLiteral("luts/x.cube"));
        QCOMPARE(out.value(QStringLiteral("inputLutRelative")).toString(),
            QStringLiteral("../shared/y.cube"));
        QVERIFY(!out.contains(QStringLiteral("otherRelative")));
    }

    void projectMovesWithItsLuts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(QStringLiteral("a"));
        const QString lut = folder + QStringLiteral("/luts/look.cube");
        const QString input = folder + QStringLiteral("/luts/input.cube");
        const QString outside = dir.filePath(QStringLiteral("elsewhere/far.cube"));
        QVERIFY(writeFile(lut, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"));
        QVERIFY(writeFile(input, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"));
        QVERIFY(writeFile(outside, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"));

        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        ClipData clip;
        clip.id = tl.allocateId();
        clip.kind = ClipKind::Solid;
        clip.duration = 10;
        GradeNode node;
        node.id = clip.grade.allocateId();
        node.type = GradeNodeType::Corrector;
        node.lutPath = lut;
        clip.grade.nodes.push_back(node);
        GradeNode far = node;
        far.id = clip.grade.allocateId();
        far.lutPath = outside;
        clip.grade.nodes.push_back(far);
        clip.grade.output = far.id;
        tl.videoTracks[0].clips.push_back(clip);
        project.timeline()->notifyChanged();
        MediaItem item;
        item.path = folder + QStringLiteral("/m.mov");
        item.name = QStringLiteral("m.mov");
        item.attributes.inputLut = input;
        project.beginEdit(QStringLiteral("add"));
        const QString mediaId = project.mediaPool()->addItem(item);
        project.endEdit();

        QString error;
        QVERIFY2(project.save(folder + QStringLiteral("/p.kinoklip"), &error), qPrintable(error));
        // The in-memory values stay absolute.
        QCOMPARE(project.timeline()->data().videoTracks[0].clips[0].grade.nodes[0].lutPath, lut);

        // Move the whole project folder; the LUT outside it stays where it is.
        const QString moved = dir.filePath(QStringLiteral("b"));
        QVERIFY(QDir().rename(folder, moved));
        Project loaded;
        QVERIFY2(loaded.load(moved + QStringLiteral("/p.kinoklip"), &error), qPrintable(error));
        const ClipGrade& grade = loaded.timeline()->data().videoTracks[0].clips[0].grade;
        QCOMPARE(grade.nodes[0].lutPath, moved + QStringLiteral("/luts/look.cube"));
        QCOMPARE(grade.nodes[1].lutPath, outside);
        const MediaItem* back = loaded.mediaPool()->item(mediaId);
        QVERIFY(back);
        QCOMPARE(back->attributes.inputLut, moved + QStringLiteral("/luts/input.cube"));

        // A LUT that exists at its stored path is kept even when the relative one differs.
        Project again;
        QVERIFY2(again.load(moved + QStringLiteral("/p.kinoklip"), &error), qPrintable(error));
        QCOMPARE(again.timeline()->data().videoTracks[0].clips[0].grade.nodes[0].lutPath,
            moved + QStringLiteral("/luts/look.cube"));
    }
};

QTEST_MAIN(TestLutPaths)
#include "test_lutpaths.moc"
