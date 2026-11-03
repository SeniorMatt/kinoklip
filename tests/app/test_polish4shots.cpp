// SPDX-License-Identifier: GPL-3.0-or-later
// Polish round 4 in the real main window (docs/polish-v3.md, Round 4), in the default
// Kinoklip colour scheme: ranged markers on the ruler and on a clip, the Normalize Audio
// Levels dialog, the History window and the Edit Index column menu. Checks that each state
// appears; with KK_SCREENSHOT_DIR set each one is also saved as a PNG for review.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Page.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/HistoryWindow.h"
#include "ui/dialogs/NormalizeAudioDialog.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>
#include <QTest>
#include <QUndoStack>
#include <QUndoView>

using namespace kk;
using namespace kk::test;

namespace {

struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    App()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        playback = std::make_unique<PlaybackEngine>(std::move(options));
        playback->setMediaPool(project.mediaPool());
        playback->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
    EditPage* page() const { return window->editPage(); }
    TimelineWidget* timeline() const { return page()->timeline()->timelineWidget(); }
    void settle(int ms = 400) { QTest::qWait(ms); }
};

/// Saves @p widget as <KK_SCREENSHOT_DIR>/<name>.png when the variable is set.
void shot(QWidget* widget, const char* name)
{
    const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QCoreApplication::processEvents();
    widget->grab().save(QDir(dir).filePath(QLatin1String(name) + QStringLiteral(".png")));
}

} // namespace

class TestPolish4Shots : public QObject
{
    Q_OBJECT

    QString m_skip;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_polish4shots"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp); // the default Kinoklip scheme
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
    }

    void editPageStates()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        App app;
        const QString a = app.project.importMedia({fixture("h264_25fps.mp4")}).importedIds.value(0);
        const QString b = app.project.importMedia({fixture("h264_2997.mp4")}).importedIds.value(0);
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        TimelineController* tc = app.page()->timeline()->controller();
        tc->editSources({{a, 0, 0}, {b, 0, 0}, {a, 2'000'000, 0}}, SourceEditMode::AppendAtEnd);
        const quint64 title
            = effectactions::addGeneratorClip(app.project, QStringLiteral("text_title"), 30, 100);
        QVERIFY(title);

        // Ranged markers on the ruler and on a clip (spec 18.11), plus a single one.
        app.project.beginEdit(QStringLiteral("Markers"));
        TimelineData& d = app.project.timeline()->mutableData();
        Marker scene;
        scene.frame = 20;
        scene.duration = 60;
        scene.name = QStringLiteral("Opening scene");
        scene.color = QColor(0x4c, 0x9b, 0xe8);
        Marker cut;
        cut.frame = 140;
        cut.name = QStringLiteral("Cut here");
        cut.color = QColor(0xe0, 0x4a, 0x4a);
        Marker outro;
        outro.frame = 170;
        outro.duration = 45;
        outro.name = QStringLiteral("Music swell");
        outro.color = QColor(0xe8, 0xc8, 0x3c);
        d.markers = {scene, cut, outro};
        Marker take;
        take.frame = 10;
        take.duration = 40;
        take.name = QStringLiteral("Good take");
        take.color = QColor(0x4c, 0xb8, 0x4c);
        d.videoTracks[0].clips[1].markers = {take};
        app.project.endEdit();
        app.timeline()->zoomToFit();
        app.settle(800);
        shot(app.page()->timeline(), "ranged-markers-timeline");
        shot(app.window.get(), "ranged-markers-window");

        // Edit Index: markers with their durations, then the column menu (spec 18.10).
        app.page()->setPanelVisible(PanelId::EditIndex, true);
        app.settle();
        EditIndexPanel* index = app.page()->editIndex();
        index->setMode(EditIndexModel::Mode::Markers);
        app.settle();
        QCOMPARE(index->table()->model()->rowCount(), 4);
        shot(app.window.get(), "editindex-ranged-markers");
        index->setMode(EditIndexModel::Mode::Edits);
        index->setColumnVisible(EditIndexModel::EditReel, false);
        app.settle();
        QMenu* menu = index->createColumnMenu(app.window.get());
        const QHeaderView* header = index->table()->horizontalHeader();
        menu->popup(header->mapToGlobal(QPoint(header->sectionViewportPosition(2) + 20, 10)));
        app.settle();
        QVERIFY(menu->isVisible());
        shot(menu, "editindex-column-menu");
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            // The menu is its own window: draw it into the main window's picture.
            QPixmap composed = app.window->grab();
            QPainter p(&composed);
            p.drawPixmap(app.window->mapFromGlobal(menu->pos()), menu->grab());
            p.end();
            composed.save(QDir(dir).filePath(QStringLiteral("editindex-column-menu-window.png")));
        }
        menu->close();
        menu->deleteLater();
        index->setColumnVisible(EditIndexModel::EditReel, true);
        app.page()->setPanelVisible(PanelId::EditIndex, false);

        // History window (spec 17.6) over a few named steps.
        app.selection.setClips({app.project.timeline()->data().videoTracks[0].clips[0].id});
        app.project.timeline()->setPlayhead(60);
        tc->splitAtPlayhead();
        tc->addMarker();
        app.actions.action(QStringLiteral("edit.history_window"))->trigger();
        app.settle();
        auto* history = app.window->findChild<HistoryWindow*>();
        QVERIFY(history && history->isVisible());
        QVERIFY(history->view()->model()->rowCount() >= 4);
        shot(history, "history-window");
        history->close();

        // Normalize Audio Levels on the audio of the first two clips (spec 37.35).
        const TimelineData& now = app.project.timeline()->data();
        app.selection.setClips({now.audioTracks[0].clips[0].id, now.audioTracks[0].clips[1].id});
        const QVector<quint64> targets = normalizeTargets(app.project, app.selection);
        QCOMPARE(targets.size(), 2);
        NormalizeAudioDialog dialog(&app.project, targets, app.window.get());
        dialog.setMode(audionorm::Mode::Loudness);
        dialog.show();
        app.settle();
        shot(&dialog, "normalize-dialog");
        dialog.start();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 15000);
        QCOMPARE(app.project.undoStack()->undoText(), QStringLiteral("Normalize Audio Levels"));
        app.project.undoStack()->setClean(); // demo edits: closing must not ask to save them
    }
};

QTEST_MAIN(TestPolish4Shots)
#include "test_polish4shots.moc"
