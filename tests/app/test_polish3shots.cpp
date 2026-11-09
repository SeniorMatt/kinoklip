// SPDX-License-Identifier: GPL-3.0-or-later
// Polish round 3 in the real main window (docs/archive/polish-v3.md, Round 3), in the default
// Kinoklip colour scheme: the Edit Index in Edits and Markers mode, the on-screen Transform
// controls (scale, rotation, anchor) and the Crop controls on a clip, and the source
// timecode on timeline clips. Checks that each state appears; with KK_SCREENSHOT_DIR set each
// one is also saved as a PNG for review.
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
#include "ui/editindex/EditIndexPanel.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>
#include <QTest>

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

class TestPolish3Shots : public QObject
{
    Q_OBJECT

    QString m_skip;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_polish3shots"));
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
        // Markers on the ruler and on a clip.
        app.project.timeline()->setPlayhead(20);
        tc->addMarkerAtPlayhead();
        app.project.beginEdit(QStringLiteral("Markers"));
        TimelineData& d = app.project.timeline()->mutableData();
        d.markers[0].name = QStringLiteral("Opening");
        d.markers[0].note = QStringLiteral("music starts");
        d.markers.push_back({140, QStringLiteral("Cut here"), QColor(0xe0, 0x4a, 0x4a), {}});
        d.videoTracks[0].clips[1].markers.push_back(
            {10, QStringLiteral("Good take"), QColor(0x4c, 0xb8, 0x4c), QStringLiteral("use")});
        app.project.endEdit();
        app.timeline()->zoomToFit();

        // Source timecode on clips (Timeline View Options).
        app.page()->timeline()->setShowSourceTimecode(true);
        app.settle(800);
        QVERIFY(app.timeline()->showSourceTimecode());
        shot(app.page()->timeline(), "clip-source-timecode");

        // Edit Index, both modes.
        app.page()->setPanelVisible(PanelId::EditIndex, true);
        app.settle();
        EditIndexPanel* index = app.page()->editIndex();
        QVERIFY(index->isVisible());
        QVERIFY(index->table()->model()->rowCount() >= 4);
        index->table()->selectRow(1);
        app.settle();
        shot(app.window.get(), "editindex-edits");
        index->setMode(EditIndexModel::Mode::Markers);
        app.settle();
        QCOMPARE(index->table()->model()->rowCount(), 3);
        shot(app.window.get(), "editindex-markers");
        app.page()->setPanelVisible(PanelId::EditIndex, false);

        // Transform controls on a zoomed, rotated clip; then Crop.
        const quint64 clip = app.project.timeline()->data().videoTracks[0].clips[1].id;
        app.project.beginEdit(QStringLiteral("Look"));
        ClipData* c = editops::clipById(app.project.timeline()->mutableData(), clip);
        c->transform.setBase(keys::ZoomX, 0.6);
        c->transform.setBase(keys::ZoomY, 0.6);
        c->transform.setBase(keys::Rotation, 8);
        c->transform.setBase(keys::CropL, 120);
        c->transform.setBase(keys::CropB, 60);
        app.project.endEdit();
        app.project.timeline()->setPlayhead(c->start + 5);
        app.selection.setClips({clip});
        app.actions.action(QStringLiteral("view.onscreen_transform"))->setChecked(true);
        app.settle(800);
        TransformOverlay* overlay = app.page()->timelineViewer()->transformOverlay();
        QVERIFY(overlay && overlay->isEditing());
        shot(app.page()->timelineViewer(), "transform-scale-rotate-handles");
        app.actions.action(QStringLiteral("view.onscreen_crop"))->setChecked(true);
        app.settle();
        QVERIFY(overlay->isCropMode() && overlay->isEditing());
        shot(app.page()->timelineViewer(), "transform-crop-handles");
        shot(app.window.get(), "transform-crop-window");
        app.actions.action(QStringLiteral("view.onscreen_crop"))->setChecked(false);
    }
};

QTEST_MAIN(TestPolish3Shots)
#include "test_polish3shots.moc"
