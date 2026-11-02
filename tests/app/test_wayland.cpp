// SPDX-License-Identifier: GPL-3.0-or-later
// Wayland integration test: the main window under a real compositor (weston, via
// scripts/wayland_session.sh) driven with real pointer input (xdotool on the X screen weston
// runs on). Covers what synthetic QTest events cannot: wl_data_device drags that need the
// press serial (media pool -> timeline, effects library -> clip, source viewer picture ->
// timeline, into a new track), Qt Wayland's "only popups may grab" rule (splitter drags,
// menu bar), and the Keyframes panel floating into a decorated top-level.
//
// Skips unless QT_QPA_PLATFORM is wayland and xdotool + $DISPLAY are available: ctest runs it
// as test_wayland_session in the Arch container (CMakeLists.txt), elsewhere it skips.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QImage>
#include <QListView>
#include <QMenuBar>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include <memory>

using namespace kk;

namespace {

QStringList g_grabWarnings;
QtMessageHandler g_previousHandler = nullptr;

void captureGrabWarnings(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    if (text.contains(QLatin1String("grabbing the mouse"))
        || text.contains(QLatin1String("grabbing the keyboard")))
        g_grabWarnings.append(text);
    if (g_previousHandler)
        g_previousHandler(type, context, text);
}

/// Runs an xdotool script in the background (the app must keep processing events while the
/// pointer moves: a drag runs QDrag::exec()'s nested event loop until the drop). Steps are
/// separated by short sleeps like a hand would be.
void runPointer(const QString& script)
{
    QProcess::startDetached(QStringLiteral("sh"), {QStringLiteral("-c"), script});
}

QString dragScript(QPoint from, QPoint to, int steps = 15)
{
    QString s
        = QStringLiteral("xdotool mousemove %1 %2; sleep 0.3; xdotool mousedown 1; sleep 0.2;")
              .arg(from.x())
              .arg(from.y());
    for (int i = 1; i <= steps; ++i) {
        const QPoint p = from + (to - from) * i / steps;
        s += QStringLiteral(" xdotool mousemove %1 %2; sleep 0.04;").arg(p.x()).arg(p.y());
    }
    s += QStringLiteral(" sleep 0.4;");
    // Debugging aid: KK_TEST_SCREENSHOT_DIR=<dir> saves the X screen just before each release.
    const QString shots = qEnvironmentVariable("KK_TEST_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        static int n = 0;
        s += QStringLiteral(" import -window root %1/drag-%2.png;").arg(shots).arg(++n);
    }
    s += QStringLiteral(" xdotool mouseup 1");
    return s;
}

} // namespace

class TestWayland : public QObject
{
    Q_OBJECT

    Project m_project;
    SelectionModel m_selection;
    ActionRegistry m_actions;
    AppContext m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QTemporaryDir m_dir;
    QString m_mediaId;

    EditPage* page() const { return m_window->editPage(); }
    TimelineWidget* timeline() const { return page()->timeline()->timelineWidget(); }
    const TimelineData& data() const { return m_project.timeline()->data(); }

    /// Screen position of @p local in @p w: the window is full screen at the output's origin,
    /// and weston's output is the X screen xdotool drives.
    QPoint screen(const QWidget* w, QPoint local) const { return w->mapTo(m_window.get(), local); }
    QPoint trackPoint(TrackRef ref, double frame) const
    {
        const TimelineLayout& L = timeline()->layout();
        const QRect row = L.rowRect(L.rowOf(ref));
        return screen(timeline()->viewport(), QPoint(int(L.frameToX(frame)), row.center().y()));
    }
    QPoint poolItemPoint() const
    {
        // Find the imported still by id: the pool also lists the project's timeline(s).
        // The visible list view: the panel also has a (hidden) strip view since the Cut page.
        QListView* grid = nullptr;
        for (QListView* view : page()->mediaPool()->findChildren<QListView*>()) {
            if (view->isVisible())
                grid = view;
        }
        Q_ASSERT(grid);
        const QAbstractItemModel* model = grid->model();
        QModelIndex index = model->index(0, 0);
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex candidate = model->index(row, 0);
            if (candidate.data(MediaPoolModel::IdRole).toString() == m_mediaId) {
                index = candidate;
                break;
            }
        }
        return screen(grid->viewport(), grid->visualRect(index).center());
    }

private slots:
    void initTestCase()
    {
        if (QGuiApplication::platformName() != QLatin1String("wayland"))
            QSKIP("needs a Wayland session (scripts/wayland_session.sh)");
        if (qEnvironmentVariableIsEmpty("DISPLAY")
            || QStandardPaths::findExecutable(QStringLiteral("xdotool")).isEmpty())
            QSKIP("needs xdotool and the X screen weston runs on");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_wayland"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        g_previousHandler = qInstallMessageHandler(captureGrabWarnings);

        m_actions.loadDefaultKeymap();
        registerStandardActions(m_actions);
        m_ctx.project = &m_project;
        m_ctx.selection = &m_selection;
        m_ctx.actions = &m_actions;
        // A still (no ffmpeg needed): a video clip without audio.
        QImage image(320, 180, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        const QString path = m_dir.filePath(QStringLiteral("still.png"));
        QVERIFY(image.save(path));
        const ImportResult imported = m_project.importMedia({path});
        QCOMPARE(imported.importedIds.size(), 1);
        m_mediaId = imported.importedIds.front();

        MainWindowOptions options;
        options.autosave = false;
        m_window = std::make_unique<MainWindow>(m_ctx, options);
        m_window->setCurrentPage(QStringLiteral("edit"));
        m_window->showFullScreen();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QTest::qWait(1000); // fullscreen configure + layout
        QVERIFY(!page()->mediaPool()->visibleMediaIds().isEmpty());
        m_project.timeline()->setPlayhead(0);
    }

    void mediaPoolToTimeline()
    {
        runPointer(dragScript(poolItemPoint(), trackPoint({TrackType::Video, 0}, 20)));
        QTRY_COMPARE_WITH_TIMEOUT(int(data().videoTracks[0].clips.size()), 1, 8000);
        QVERIFY(data().videoTracks[0].clips[0].mediaId == m_mediaId);
    }

    void mediaPoolToEmptySpaceCreatesTrack()
    {
        const TimelineLayout& L = timeline()->layout();
        const QRect top = L.rowRect(0);
        if (top.top() - timelinemetrics::kRulerHeight < 12)
            QSKIP("no empty space above the top track in this layout");
        const int tracks = int(data().videoTracks.size());
        const QPoint target = screen(timeline()->viewport(),
            QPoint(int(L.frameToX(200)), (top.top() + timelinemetrics::kRulerHeight) / 2));
        runPointer(dragScript(poolItemPoint(), target));
        QTRY_COMPARE_WITH_TIMEOUT(int(data().videoTracks.size()), tracks + 1, 8000);
        QCOMPARE(int(data().videoTracks[size_t(tracks)].clips.size()), 1);
    }

    void effectsLibraryToClip()
    {
        // The library alone in the left column (sharing it with the media pool leaves the effect
        // list a few pixels wide at this window size).
        page()->setPanelVisible(PanelId::MediaPool, false);
        page()->setPanelVisible(PanelId::Effects, true);
        QTest::qWait(400);
        QTreeWidget* tree = nullptr;
        for (auto* t : page()->effectsLibrary()->findChildren<QTreeWidget*>()) {
            if (t->dragEnabled() && t->isVisible())
                tree = t;
        }
        QVERIFY(tree);
        QTreeWidgetItem* blur = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("gaussian_blur"))
                blur = *it;
        }
        QVERIFY(blur);
        if (blur->parent())
            blur->parent()->setExpanded(true);
        tree->scrollToItem(blur);
        QTest::qWait(200);
        QVERIFY(!tree->visualItemRect(blur).isEmpty());
        // Inside the visible part of the row (the list can be narrower than the row's text).
        const QRect row = tree->visualItemRect(blur);
        const QPoint from = screen(tree->viewport(),
            QPoint(std::min(row.left() + 12, tree->viewport()->width() - 4), row.center().y()));
        const quint64 clipId = data().videoTracks[0].clips[0].id;
        runPointer(dragScript(from, trackPoint({TrackType::Video, 0}, 30)));
        QTRY_VERIFY_WITH_TIMEOUT(!data().videoTracks[0].clips[0].effects.isEmpty(), 8000);
        QCOMPARE(data().videoTracks[0].clips[0].id, clipId);
        page()->setPanelVisible(PanelId::Effects, false);
        page()->setPanelVisible(PanelId::MediaPool, true);
        QTest::qWait(300);
    }

    void sourceViewerToTimeline()
    {
        ViewerPanel* source = page()->sourceViewer();
        if (!source->isVisible())
            QSKIP("source viewer hidden in this layout");
        source->setSourceMedia(m_mediaId);
        QTest::qWait(500);
        QWidget* surface = source->surface()->widget();
        const QPoint from = screen(surface, source->surface()->pictureRect().center());
        const int before = int(data().videoTracks[1].clips.size());
        runPointer(dragScript(from, trackPoint({TrackType::Video, 1}, 400)));
        QTRY_COMPARE_WITH_TIMEOUT(int(data().videoTracks[1].clips.size()), before + 1, 8000);
    }

    void splitterAndMenuBarDoNotGrab()
    {
        g_grabWarnings.clear();
        // Every page splitter handle between panels: hover, then drag 30 px and back.
        QString script;
        for (auto* splitter : page()->findChildren<PanelSplitter*>()) {
            if (!splitter->isVisible() || splitter->count() < 2)
                continue;
            QSplitterHandle* handle = splitter->handle(1);
            if (!handle->isVisible() || handle->width() < 3 || handle->height() < 3)
                continue;
            const QPoint c = screen(handle, handle->rect().center());
            const QPoint d
                = splitter->orientation() == Qt::Horizontal ? QPoint(30, 0) : QPoint(0, 30);
            // Hover onto the handle (Breeze's proxy would appear now), then drag and back.
            script += QStringLiteral("xdotool mousemove %1 %2; sleep 0.2; xdotool mousemove %3 %4; "
                                     "sleep 0.4; ")
                          .arg(c.x() - d.x())
                          .arg(c.y() - d.y())
                          .arg(c.x())
                          .arg(c.y());
            script += dragScript(c, c + d, 5) + QStringLiteral("; sleep 0.3; ")
                + dragScript(c + d, c, 5) + QStringLiteral("; sleep 0.3; ");
        }
        QVERIFY(!script.isEmpty());
        // Menu bar: open File, click its title again to close it.
        QMenuBar* bar = m_window->menuBar();
        const QPoint file = screen(bar, bar->actionGeometry(bar->actions().constFirst()).center());
        script += QStringLiteral("xdotool mousemove %1 %2 click 1; sleep 0.6; xdotool click 1; "
                                 "sleep 0.4; touch %3")
                      .arg(file.x())
                      .arg(file.y())
                      .arg(m_dir.filePath(QStringLiteral("done")));
        runPointer(script);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(m_dir.filePath(QStringLiteral("done"))), 60000);
        QTest::qWait(300);
        QVERIFY2(g_grabWarnings.isEmpty(), qPrintable(g_grabWarnings.join(QLatin1Char('\n'))));
    }

    void keyframesHeaderDoubleClickFloats()
    {
        page()->setPanelVisible(PanelId::Keyframes, true);
        QTest::qWait(400);
        KeyframesPanel* panel = page()->keyframes();
        QVERIFY(panel->isVisible());
        QVERIFY(!page()->isKeyframesFloating());
        QCOMPARE(panel->floatButton()->text(), QStringLiteral("Float"));
        QWidget* header = panel->titleBar();
        const QPoint p = screen(header, QPoint(header->width() / 2, header->height() / 2));
        runPointer(QStringLiteral("xdotool mousemove %1 %2; sleep 0.3; xdotool click --repeat 2 "
                                  "--delay 80 1")
                       .arg(p.x())
                       .arg(p.y()));
        QTRY_VERIFY_WITH_TIMEOUT(page()->isKeyframesFloating(), 5000);
        QWidget* window = page()->keyframesWindow();
        QVERIFY(window);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(window->isWindow());
        QVERIFY(window->windowTitle().startsWith(QLatin1String("Keyframes")));
        QCOMPARE(panel->floatButton()->text(), QStringLiteral("Dock"));
        page()->setKeyframesFloating(false);
        QVERIFY(!window->isVisible());
        page()->setPanelVisible(PanelId::Keyframes, false);
    }

    void cleanupTestCase()
    {
        m_window.reset();
        if (g_previousHandler)
            qInstallMessageHandler(g_previousHandler);
    }
};

QTEST_MAIN(TestWayland)
#include "test_wayland.moc"
