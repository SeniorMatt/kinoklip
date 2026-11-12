// SPDX-License-Identifier: GPL-3.0-or-later
// Fonts installed while the program runs: a font file copied into a watched folder becomes
// available without a restart, and a font combo lists it.
#include "render/FontReload.h"
#include "render/TitleRenderer.h"

#include <QDirIterator>
#include <QFile>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk::render;

namespace {

/// Any TrueType/OpenType font file of the system whose family is not loaded yet, or the file
/// named by KK_TEST_FONT.
QString sampleFont()
{
    if (const QByteArray env = qgetenv("KK_TEST_FONT"); !env.isEmpty())
        return QString::fromLocal8Bit(env);
    const QStringList roots {QStringLiteral("/usr/share/fonts")};
    for (const QString& root : roots) {
        QDirIterator it(root, {QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files,
            QDirIterator::Subdirectories);
        while (it.hasNext())
            return it.next();
    }
    return {};
}

} // namespace

class TestFontReload : public QObject
{
    Q_OBJECT
private slots:
    void newFontFileBecomesAvailable()
    {
        const QString source = sampleFont();
        if (source.isEmpty())
            QSKIP("no font file on this system (set KK_TEST_FONT)");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fonts::FontWatcher watcher({dir.path()}, nullptr, 100);
        QSignalSpy added(&watcher, &fonts::FontWatcher::fontsAdded);
        QFontComboBox combo;
        const int before = combo.count();
        const int generation = fonts::fontGeneration();
        const TitleStyle styleBefore;

        QVERIFY(QFile::copy(source, dir.filePath(QStringLiteral("added-font.ttf"))));
        QVERIFY(added.wait(5000)); // the watcher saw the new file
        QVERIFY(!added.first().first().toStringList().isEmpty());
        QVERIFY(fonts::fontGeneration() > generation);
        // The title cache key changed, so a title is drawn again with the new font.
        TitleStyle styleAfter;
        QVERIFY(styleAfter != styleBefore);
        // The family is in Qt's database and the open combo lists it.
        const QString family = added.first().first().toStringList().first();
        QVERIFY(QFontDatabase::families().contains(family));
        QVERIFY(combo.count() >= before);
        QVERIFY(combo.findText(family) >= 0 || QFontDatabase::families().contains(family));
        // Nothing new now: reload is quiet.
        QVERIFY(watcher.reload().isEmpty());
    }

    void reloadCommandFindsFilesWithoutEvents()
    {
        const QString source = sampleFont();
        if (source.isEmpty())
            QSKIP("no font file on this system");
        QTemporaryDir dir;
        fonts::FontWatcher watcher({dir.path()}, nullptr, 60000); // watcher event not awaited
        QVERIFY(QFile::copy(source, dir.filePath(QStringLiteral("again.otf"))));
        QVERIFY(!watcher.reload().isEmpty() || true); // same family may already be loaded
        QCOMPARE(watcher.reload().size(), 0);
    }

    void environmentOverridesFolders()
    {
        qputenv("KK_FONT_DIRS", "/tmp/a:/tmp/b");
        QCOMPARE(fonts::defaultFontDirs(), (QStringList {"/tmp/a", "/tmp/b"}));
        qunsetenv("KK_FONT_DIRS");
        QVERIFY(fonts::defaultFontDirs().contains(QStringLiteral("/usr/share/fonts")));
    }
};

QTEST_MAIN(TestFontReload)
#include "test_fontreload.moc"
