// SPDX-License-Identifier: GPL-3.0-or-later
// test_lutpacking — LUT files of grade nodes and input LUTs inside a single-file project
// (spec 175): Pack copies them in, the runtime document points at the stored copies, Save copies
// a LUT chosen later, Unpack writes them back, Compact keeps them.
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "render/CubeLut.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::pack;

namespace {

const char* kCube = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// A project document that uses a grade node LUT and a clip's input LUT.
QByteArray document(const QString& nodeLut, const QString& inputLut)
{
    const QJsonObject root {{QStringLiteral("mediaPool"),
                                QJsonObject {{QStringLiteral("items"), QJsonArray {}}}},
        {QStringLiteral("timelines"),
            QJsonArray {QJsonObject {{QStringLiteral("nodes"),
                QJsonArray {QJsonObject {{QStringLiteral("lut"), nodeLut}}}}}}},
        {QStringLiteral("inputLut"), inputLut}};
    return QJsonDocument(root).toJson();
}

QString nodeLutOf(const QByteArray& json, const QString& key = QStringLiteral("lut"))
{
    return QJsonDocument::fromJson(json)
        .object()
        .value(QStringLiteral("timelines"))
        .toArray()[0]
        .toObject()
        .value(QStringLiteral("nodes"))
        .toArray()[0]
        .toObject()
        .value(key)
        .toString();
}

QString inputLutOf(const QByteArray& json)
{
    return QJsonDocument::fromJson(json).object().value(QStringLiteral("inputLut")).toString();
}

} // namespace

class TestLutPacking : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString path(const QString& name) const { return m_dir.filePath(name); }

private Q_SLOTS:
    void packEmbedsLuts()
    {
        QVERIFY(m_dir.isValid());
        const QString lutA = path(QStringLiteral("luts/a.cube"));
        const QString lutB = path(QStringLiteral("other/b.cube"));
        QVERIFY(writeFile(lutA, kCube));
        QVERIFY(writeFile(lutB, QByteArray(kCube) + "# second\n"));
        QVERIFY(writeFile(path(QStringLiteral("p/film.kkproj")), document(lutA, lutB)));

        const QString container = path(QStringLiteral("film.kinoklip"));
        const PackResult packed = packProject(path(QStringLiteral("p/film.kkproj")), container);
        QVERIFY2(packed.status == Status::Ok, qPrintable(packed.error));
        QCOMPARE(packed.embedded, 2);

        QString error;
        auto package = ProjectPackage::open(container, false, &error);
        QVERIFY2(package, qPrintable(error));
        const QByteArray stored = package->storedDocument();
        const QString entry = nodeLutOf(stored);
        QVERIFY(entry.startsWith(QLatin1String("media/")));
        QCOMPARE(nodeLutOf(stored, QStringLiteral("lutEmbedded")), entry);
        QCOMPARE(nodeLutOf(stored, QStringLiteral("lutRelative")), entry);
        QVERIFY(inputLutOf(stored).startsWith(QLatin1String("media/")));
        QCOMPARE(package->unusedMedia(), QStringList());

        // Runtime form: embedded paths, and the LUT loads from them.
        const QByteArray runtime = package->document(&error);
        const QString runtimeLut = nodeLutOf(runtime);
        QCOMPARE(runtimeLut, embeddedPath(container, entry));
        QVERIFY(nodeLutOf(runtime, QStringLiteral("lutEmbedded")).isEmpty());
        QString why;
        const auto lut = render::loadCubeLut(runtimeLut, &why);
        QVERIFY2(lut, qPrintable(why));
        // Storing the runtime form again gives the stored form back.
        QCOMPARE(QJsonDocument::fromJson(documentToStorage(runtime, container)),
            QJsonDocument::fromJson(stored));
        QVERIFY(verifyContainer(container).ok);
    }

    void packedProjectDoesNotNeedTheOriginals()
    {
        QVERIFY(QFile::remove(path(QStringLiteral("luts/a.cube"))));
        QString error;
        auto package = ProjectPackage::open(path(QStringLiteral("film.kinoklip")), false, &error);
        QVERIFY2(package, qPrintable(error));
        QString why;
        QVERIFY2(render::loadCubeLut(nodeLutOf(package->document()), &why), qPrintable(why));
    }

    void saveCopiesALutChosenLater()
    {
        const QString container = path(QStringLiteral("film.kinoklip"));
        QString error;
        auto package = ProjectPackage::open(container, true, &error);
        QVERIFY2(package, qPrintable(error));
        const QString lutC = path(QStringLiteral("late/c.cube"));
        QVERIFY(writeFile(lutC, QByteArray(kCube) + "# third\n"));
        QJsonObject root = QJsonDocument::fromJson(package->document()).object();
        root[QStringLiteral("inputLut")] = lutC;
        QVERIFY2(package->saveDocument(QJsonDocument(root).toJson(), &error), qPrintable(error));
        // Saved twice: the copy is made once.
        QVERIFY2(package->saveDocument(QJsonDocument(root).toJson(), &error), qPrintable(error));
        const QByteArray stored = package->storedDocument();
        QVERIFY(inputLutOf(stored).startsWith(QLatin1String("media/")));
        QVERIFY(inputLutOf(stored).endsWith(QLatin1String("c.cube")));
        int copies = 0;
        for (const ContainerEntry& e : package->container().entries())
            copies += e.name.endsWith(QLatin1String("/c.cube"));
        QCOMPARE(copies, 1);
        QCOMPARE(package->unusedMedia().size(), 1); // b.cube is not used any more
    }

    void compactKeepsUsedLuts()
    {
        const QString container = path(QStringLiteral("film.kinoklip"));
        const CompactResult compacted = compactProject(container);
        QVERIFY2(compacted.status == Status::Ok, qPrintable(compacted.error));
        QCOMPARE(compacted.removedMedia.size(), 1);
        QVERIFY(compacted.removedMedia.front().endsWith(QLatin1String("b.cube")));
        QString error;
        auto package = ProjectPackage::open(container, false, &error);
        QVERIFY2(package, qPrintable(error));
        QString why;
        QVERIFY2(render::loadCubeLut(nodeLutOf(package->document()), &why), qPrintable(why));
        QVERIFY2(render::loadCubeLut(inputLutOf(package->document()), &why), qPrintable(why));
    }

    void unpackWritesTheLuts()
    {
        const QString container = path(QStringLiteral("film.kinoklip"));
        const QString folder = path(QStringLiteral("unpacked"));
        const UnpackResult result = unpackProject(container, folder);
        QVERIFY2(result.status == Status::Ok, qPrintable(result.error));
        QCOMPARE(result.extracted, 2);
        const QByteArray doc = readFile(result.projectFile);
        const QString entry = nodeLutOf(doc);
        QVERIFY(QFileInfo(QDir(folder).absoluteFilePath(entry)).isFile());
        // The relinking helper used by "Save As" a linked project.
        const auto package = ProjectPackage::open(container, false, nullptr);
        QVERIFY(package);
        const QByteArray linked = lutsToFolder(package->document(), container, folder);
        QCOMPARE(nodeLutOf(linked), QDir(folder).absoluteFilePath(entry));
        QVERIFY(QFileInfo(nodeLutOf(linked)).isFile());
    }

    void missingLutStaysLinked()
    {
        QVERIFY(writeFile(path(QStringLiteral("q/p.kkproj")),
            document(path(QStringLiteral("gone/x.cube")), QString())));
        const PackResult packed
            = packProject(path(QStringLiteral("q/p.kkproj")), path(QStringLiteral("q.kinoklip")));
        QVERIFY2(packed.status == Status::Ok, qPrintable(packed.error));
        QCOMPARE(packed.embedded, 0);
        QCOMPARE(packed.skipped.size(), 1);
        QVERIFY(packed.skipped.front().contains(QLatin1String("x.cube")));
    }
};

QTEST_MAIN(TestLutPacking)
#include "test_lutpacking.moc"
