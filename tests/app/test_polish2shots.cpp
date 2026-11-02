// SPDX-License-Identifier: GPL-3.0-or-later
// Polish round 2 in the real main window (docs/polish-v3.md, Round 2), in the default
// Kinoklip colour scheme: offline media in the timeline and viewer, multi-clip "Mixed"
// fields, the on-screen Transform controls on a title, the Timeline View Options and track
// header menus, the viewer context menu, the Cut page's smart edit overlay and the Audio
// page headers. Checks that each state appears; with KK_SCREENSHOT_DIR set each one is also
// saved as a PNG for review.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/cut/CutViewer.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ValueField.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/EditOverlay.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

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

QString shotDir()
{
    return qEnvironmentVariable("KK_SCREENSHOT_DIR");
}

/// Saves @p widget as <KK_SCREENSHOT_DIR>/<name>.png when the variable is set.
void shot(QWidget* widget, const char* name)
{
    if (shotDir().isEmpty())
        return;
    QDir().mkpath(shotDir());
    QCoreApplication::processEvents();
    widget->grab().save(QDir(shotDir()).filePath(QLatin1String(name) + QStringLiteral(".png")));
}

/// The window with @p menu composited at its screen position (popups are separate windows).
void shotWithMenu(QWidget* window, QMenu* menu, const char* name)
{
    if (shotDir().isEmpty())
        return;
    QPixmap base = window->grab();
    QPainter p(&base);
    p.drawPixmap(window->mapFromGlobal(menu->pos()), menu->grab());
    p.end();
    QDir().mkpath(shotDir());
    base.save(QDir(shotDir()).filePath(QLatin1String(name) + QStringLiteral(".png")));
}

/// Opens a context menu with @p open, shoots it over @p window and closes it.
QStringList captureMenu(QWidget* window, const char* name, const std::function<void()>& open)
{
    QStringList texts;
    QTimer::singleShot(150, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions())
                if (!a->isSeparator())
                    texts.append(a->text().remove(QLatin1Char('&')));
            shotWithMenu(window, menu, name);
            menu->close();
        }
    });
    open();
    QTest::qWait(300); // popup() menus (the viewer's) return at once; exec() ones block
    return texts;
}

} // namespace

class TestPolish2Shots : public QObject
{
    Q_OBJECT

    QString m_skip;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_polish2shots"));
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
        // A copy of the fixture that is moved away later (offline media).
        QTemporaryDir dir;
        const QString media = dir.filePath(QStringLiteral("interview.mp4"));
        QVERIFY(QFile::copy(fixture("h264_25fps.mp4"), media));
        App app;
        const QString id = app.project.importMedia({media}).importedIds.value(0);
        const QString other
            = app.project.importMedia({fixture("h264_60fps.mp4")}).importedIds.value(0);
        TimelineController* tc = app.page()->timeline()->controller();
        tc->editSources({{id, 0, 0}, {other, 0, 0}}, SourceEditMode::AppendAtEnd);
        const quint64 title
            = effectactions::addGeneratorClip(app.project, QStringLiteral("text_title"), 30, 100);
        QVERIFY(title);
        app.timeline()->zoomToFit();
        app.settle(800);

        // Transform controls on the title.
        app.project.timeline()->setPlayhead(60);
        app.selection.setClips({title});
        app.actions.action(QStringLiteral("view.onscreen_transform"))->setChecked(true);
        app.settle();
        TransformOverlay* overlay = app.page()->timelineViewer()->transformOverlay();
        QVERIFY(overlay && overlay->isEditing());
        shot(app.window.get(), "transform-controls");
        app.actions.action(QStringLiteral("view.onscreen_transform"))->setChecked(false);

        // Multi-clip edit: two media clips selected, different zoom: "Mixed".
        const TimelineData& d = app.project.timeline()->data();
        const quint64 a = d.videoTracks[0].clips[0].id;
        const quint64 b = d.videoTracks[0].clips[1].id;
        app.project.beginEdit(QStringLiteral("setup"));
        editops::clipById(app.project.timeline()->mutableData(), a)
            ->transform.animated(QStringLiteral("zoomX"), 1.0)
            .setValue(0, 1.4);
        app.project.endEdit();
        app.selection.setClips({a, b});
        app.settle();
        auto* zoom = app.page()->inspector()->findChild<ValueField*>(QStringLiteral("value:zoomX"));
        QVERIFY(zoom && zoom->isMixed());
        shot(app.window.get(), "inspector-mixed");

        // Menus: Timeline View Options, the track header, the viewer.
        auto* viewOptions = app.page()->timeline()->findChild<QToolButton*>(
            QStringLiteral("TimelineViewOptions"));
        QVERIFY(viewOptions);
        const QStringList opts = captureMenu(
            app.window.get(), "timeline-view-options", [viewOptions] { viewOptions->showMenu(); });
        QVERIFY(opts.contains(QStringLiteral("Head and Tail Frames")));
        const TimelineLayout& L = app.timeline()->layout();
        const QPoint header(60, L.rowRect(L.rowOf({TrackType::Video, 0})).center().y());
        const QStringList hdr = captureMenu(app.window.get(), "track-header-menu", [&] {
            QContextMenuEvent e(
                QContextMenuEvent::Mouse, header, app.timeline()->viewport()->mapToGlobal(header));
            QCoreApplication::sendEvent(app.timeline()->viewport(), &e);
        });
        QVERIFY(hdr.contains(QStringLiteral("Move Track Up")));
        QWidget* surface
            = app.page()->timelineViewer()->findChild<TransformOverlay*>()->parentWidget();
        const QStringList vmenu = captureMenu(app.window.get(), "viewer-context-menu", [&] {
            const QPoint c = surface->rect().center();
            QContextMenuEvent e(QContextMenuEvent::Mouse, c, surface->mapToGlobal(c));
            QCoreApplication::sendEvent(surface, &e);
        });
        QVERIFY(vmenu.contains(QStringLiteral("Mark In")));

        // Head and tail thumbnails.
        app.page()->timeline()->setClipThumbnails(ClipThumbnails::HeadTail);
        app.settle(800);
        shot(app.page()->timeline(), "timeline-head-tail");
        app.page()->timeline()->setClipThumbnails(ClipThumbnails::Filmstrip);

        // Offline media: the file is moved away.
        QVERIFY(QFile::rename(media, dir.filePath(QStringLiteral("moved.mp4"))));
        app.project.mediaPool()->recheckOffline();
        app.selection.setClips({});
        app.project.timeline()->setPlayhead(10);
        app.settle();
        QLabel* notice = app.page()->timelineViewer()->offlineNotice();
        QVERIFY(notice && notice->isVisible());
        shot(app.window.get(), "media-offline");
    }

    void cutAndAudioPages()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        App app;
        const QString id
            = app.project.importMedia({fixture("h264_25fps.mp4")}).importedIds.value(0);
        app.page()->timeline()->controller()->editSources(
            {{id, 0, 0}}, SourceEditMode::AppendAtEnd);
        // Cut page: media dragged over the timeline viewer.
        app.actions.action(QStringLiteral("view.page_cut"))->trigger();
        app.settle(600);
        ViewerPanel* program = app.window->cutPage()->viewer()->timelineViewer();
        app.window->cutPage()->viewer()->setMode(CutViewer::Mode::Timeline);
        app.settle();
        QMimeData mime;
        mime.setData(QLatin1String(mime::MediaItems), id.toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        const QPointF at = QRectF(program->rect()).center();
#else
        const QPoint at = program->rect().center();
#endif
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(program, &enter);
        QVERIFY(program->editOverlay()->isVisible());
        shot(app.window.get(), "cut-page-smart-edit-overlay");
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(program, &leave);
        // Audio page headers.
        app.actions.action(QStringLiteral("view.page_audio"))->trigger();
        app.settle(800);
        shot(app.window.get(), "audio-page-headers");
        const TimelineLayout& L = app.window->audioPage()->timeline()->layout();
        QVERIFY(L.twoLineHeader(L.rowOf({TrackType::Audio, 0})));
    }
};

QTEST_MAIN(TestPolish2Shots)
#include "test_polish2shots.moc"
