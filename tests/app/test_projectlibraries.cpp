// SPDX-License-Identifier: GPL-3.0-or-later
// More than one project library and Open in a New Window in the Project Manager (spec 198).
#include "../projects/ProjectTestUtil.h"
#include "projects/ProjectLibrary.h"
#include "ui/common/Resources.h"
#include "ui/projects/ProjectLibraryModel.h"
#include "ui/projects/ProjectManagerWindow.h"

#include <QApplication>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;

namespace {
QString g_xdgRoot;

QStringList shownNames(const QAbstractItemModel* m)
{
    QStringList out;
    for (int r = 0; r < m->rowCount(); ++r)
        out << m->index(r, 0).data(Qt::EditRole).toString();
    out.sort();
    return out;
}
} // namespace

class TestProjectLibraries : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_main;
    QString m_second;

    QString project(const QString& root, const QString& name)
    {
        const QString path = root + QLatin1Char('/') + name + QStringLiteral(".kkproj");
        if (!test::writeProject(path))
            qFatal("cannot write %s", qPrintable(path));
        return path;
    }

private slots:
    void init()
    {
        QSettings().clear();
        m_dir = std::make_unique<QTemporaryDir>(g_xdgRoot + QStringLiteral("/pl-XXXXXX"));
        m_main = m_dir->path() + QStringLiteral("/Main");
        m_second = m_dir->path() + QStringLiteral("/Client Work");
        QDir().mkpath(m_main);
        QDir().mkpath(m_second);
    }
    void cleanup() { m_dir.reset(); }

    void severalLibrariesAreShownOneAtATime()
    {
        project(m_main, QStringLiteral("Wedding"));
        project(m_second, QStringLiteral("Ad Spot"));
        project(m_second, QStringLiteral("Jingle"));
        ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
        ProjectManagerWindow window(&library);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.libraryCount(), 1);
        QCOMPARE(shownNames(window.proxy()), QStringList {QStringLiteral("Wedding")});

        const int index = window.addLibrary(m_second);
        QCOMPARE(index, 1);
        QCOMPARE(window.libraryCount(), 2);
        QCOMPARE(window.addLibrary(m_second), 1); // the same folder stays one library
        QCOMPARE(window.addLibrary(m_dir->path() + QStringLiteral("/missing")), -1);
        auto* places = window.findChild<QListWidget*>(QStringLiteral("ProjectPlaces"));
        QVERIFY(places);
        QCOMPARE(places->count(), 3); // Project Library, Client Work, Recent Projects
        QCOMPARE(places->item(1)->text(), QStringLiteral("Client Work"));
        QCOMPARE(places->item(1)->toolTip(), QDir::toNativeSeparators(m_second));

        window.selectLibrary(1);
        QCOMPARE(window.currentLibraryIndex(), 1);
        QCOMPARE(shownNames(window.proxy()),
            (QStringList {QStringLiteral("Ad Spot"), QStringLiteral("Jingle")}));
        QCOMPARE(window.model()->library()->root(), QDir::cleanPath(m_second));
        // Operations act in the shown library: a new project lands in its folder.
        const QString made
            = window.createProject(QStringLiteral("Teaser"), QStringLiteral("kkproj"));
        QVERIFY2(made.startsWith(QDir::cleanPath(m_second)), qPrintable(made));
        window.selectLibrary(0);
        QCOMPARE(shownNames(window.proxy()), QStringList {QStringLiteral("Wedding")});

        // Recent Projects is the last place, whatever the number of libraries.
        window.showRecent();
        QCOMPARE(places->currentRow(), 2);
    }

    void theListIsRemembered()
    {
        QStringList second;
        {
            ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
            ProjectManagerWindow window(&library);
            QCOMPARE(window.addLibrary(m_second), 1);
            QVERIFY(!window.removeLibrary(0)); // the main library stays
        } // saved when the window goes
        ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
        ProjectManagerWindow window(&library);
        QCOMPARE(window.libraryCount(), 2);
        QCOMPARE(window.libraryAt(1)->root(), QDir::cleanPath(m_second));
        window.selectLibrary(1);
        QVERIFY(window.removeLibrary(1));
        QCOMPARE(window.libraryCount(), 1);
        QCOMPARE(window.currentLibraryIndex(), 0); // fell back to the main library
        QVERIFY(QDir(m_second).exists()); // the folder is not touched
    }

    void aLibraryThatIsGoneIsSkipped()
    {
        {
            ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
            ProjectManagerWindow window(&library);
            window.addLibrary(m_second);
        }
        QVERIFY(QDir(m_second).removeRecursively());
        ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
        ProjectManagerWindow window(&library);
        QCOMPARE(window.libraryCount(), 1);
    }

    void openInANewWindowStartsAnotherProgram()
    {
        const QString path = project(m_main, QStringLiteral("Wedding"));
        ProjectLibrary library(m_main, m_dir->path() + QStringLiteral("/cache"));
        ProjectManagerWindow window(&library);
        QStringList started;
        window.setNewWindowLauncher([&](const QString& p) {
            started << p;
            return true;
        });
        QVERIFY(window.openInNewWindow(path));
        QCOMPARE(started, QStringList {QDir::cleanPath(path)});
        // A folder or a missing file is not a project: nothing starts.
        QVERIFY(!window.openInNewWindow(m_main));
        QVERIFY(!window.openInNewWindow(m_main + QStringLiteral("/nothing.kkproj")));
        QCOMPARE(started.size(), 1);
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir xdg(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath()
        + QStringLiteral("/projectlibraries-XXXXXX"));
    g_xdgRoot = xdg.path();
    test::isolateXdg(g_xdgRoot);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
    QCoreApplication::setApplicationName(QStringLiteral("projectlibraries"));
    kk::initResources();
    TestProjectLibraries test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_projectlibraries.moc"
