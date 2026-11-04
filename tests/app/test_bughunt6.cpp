// SPDX-License-Identifier: GPL-3.0-or-later
// Sixth bug-hunting round, in the real main window where it matters: one test per fix, named
// after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CompositingPage.h"
#include "app/pages/EditPage.h"
#include "core/ClipColors.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, with @p clipCount solid clips of 50 frames on V1.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 2)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = QColor(40 + 60 * i, 90, 200);
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    const ClipData* clip(int i) const
    {
        return editops::clipById(project.timeline()->data(), clips[i]);
    }
    TimelineController* controller() const { return window->editPage()->timeline()->controller(); }
    /// Double-clicks the Effects library entry named @p name on the Edit page.
    void doubleClickEffect(const QString& name) const
    {
        auto* library = window->editPage()->findChild<EffectsLibraryPanel*>();
        QVERIFY(library);
        for (QTreeWidget* tree : library->findChildren<QTreeWidget*>()) {
            const QList<QTreeWidgetItem*> items
                = tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
            if (!items.isEmpty()) {
                emit tree->itemDoubleClicked(items.front(), 0);
                return;
            }
        }
        QFAIL("no such library entry");
    }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt6 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt6"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 42b.31: the node editor's keys work right after switching to the Compositing page
    // with the keyboard (Shift+5, then 1 views the selected node), without clicking into the
    // page first: the node editor takes the focus when the page shows.
    void spec42b_31_pageKeysWorkAfterSwitchingPages()
    {
        Fixture f;
        f.show();
        f.project.timeline()->setPlayhead(10);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QWidget* edit = f.window->findChild<TimelineWidget*>();
        QVERIFY(edit);
        edit->setFocus();
        QCoreApplication::processEvents();
        f.action("view.page_compositing")->trigger();
        QTest::qWait(20);
        CompositingPage* page = f.window->compositingPage();
        QWidget* focus = QApplication::focusWidget();
        QVERIFY(focus);
        QCOMPARE(focus, static_cast<QWidget*>(page->nodeEditor()));
        CompSession* s = page->session();
        QVERIFY(s->clipId());
        QVERIFY(!s->composition().nodes.isEmpty());
        const quint64 node = s->composition().nodes.front().id;
        s->setSelection({node});
        QTest::keyClick(focus, Qt::Key_2);
        QCoreApplication::processEvents();
        QCOMPARE(s->viewerNode(1), node);
    }

    // Spec 13.18: double-clicking a transition in the Effects library puts it on the selected
    // edit point, or with nothing selected on the edit nearest the playhead (like Ctrl+T, with
    // the chosen type); an edit that already has a transition gets the new type and keeps its
    // length and alignment. One undo step each.
    void spec13_18_libraryDoubleClickAddsTransitionAtEdit()
    {
        Fixture f;
        f.show();
        f.project.timeline()->setPlayhead(47);
        int before = f.project.undoStack()->index();
        f.doubleClickEffect(QStringLiteral("Wipe"));
        QVERIFY(f.clip(1)->transitionIn);
        QCOMPARE(f.clip(1)->transitionIn->type, QStringLiteral("wipe"));
        QCOMPARE(f.project.undoStack()->index(), before + 1);
        const int64_t length = f.clip(1)->transitionIn->duration;
        // The selected edit point: the type changes, the length stays.
        f.controller()->setSelectedEdit({f.clips[0], f.clips[1]});
        f.project.timeline()->setPlayhead(0);
        before = f.project.undoStack()->index();
        f.doubleClickEffect(QStringLiteral("Dip to Color"));
        QCOMPARE(f.clip(1)->transitionIn->type, QStringLiteral("dip_to_color"));
        QCOMPARE(f.clip(1)->transitionIn->duration, length);
        QCOMPARE(f.project.undoStack()->index(), before + 1);
        QVERIFY(!f.clip(0)->transitionIn);
        // Ctrl+T keeps an existing transition.
        f.action("timeline.add_transition")->trigger();
        QCOMPARE(f.clip(1)->transitionIn->type, QStringLiteral("dip_to_color"));
    }

    // Spec 18.17: the timeline tool bar's marker button has a drop-down of the sixteen marker
    // colours; the colour picked there (the current one checked) colours the timeline and clip
    // markers added from then on, and is kept in the settings.
    void spec18_17_markerButtonPicksTheColourOfNewMarkers()
    {
        Fixture f;
        f.show();
        auto* menu = f.window->editPage()->findChild<QMenu*>(QStringLiteral("MarkerColorMenu"));
        QVERIFY(menu);
        auto* button = qobject_cast<QToolButton*>(menu->parent());
        QVERIFY(button);
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);
        QCOMPARE(menu->actions().size(), int(markerColorPalette().size()));
        // Blue by default.
        f.project.timeline()->setPlayhead(5);
        f.action("mark.add_marker")->trigger();
        QCOMPARE(f.project.timeline()->data().markers.value(0).color,
            QColor(markerColorPalette()[0].rgb));
        QAction* red = nullptr;
        for (QAction* a : menu->actions())
            if (a->data().toString() == QStringLiteral("red"))
                red = a;
        QVERIFY(red);
        red->trigger();
        emit menu->aboutToShow();
        QVERIFY(red->isChecked());
        f.project.timeline()->setPlayhead(20);
        f.action("mark.add_marker")->trigger();
        const QColor redColour = QColor(nearestMarkerColor(QColor(0xe0, 0x4a, 0x4a)).rgb);
        QCOMPARE(f.project.timeline()->data().markers.value(1).color, redColour);
        // A clip marker too.
        f.selection.setClips({f.clips[1]});
        f.project.timeline()->setPlayhead(60);
        f.action("mark.add_marker")->trigger();
        QCOMPARE(f.clip(1)->markers.value(0).color, redColour);
    }

    // Spec 33.36: the timeline tool bar's flag button (Clip > Flag Clip) flags the media of the
    // selected clips with the flag picked in its drop-down (Blue by default); a second click
    // removes it again. One undo step each.
    void spec33_36_flagButtonFlagsTheSelectedClipsMedia()
    {
        Fixture f(0);
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/shot.mp4");
        item.name = QStringLiteral("shot.mp4");
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.durationUs = 10'000'000;
        const QString mediaId = f.project.mediaPool()->addItem(item);
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = mediaId;
        clip.duration = 50;
        clip.sourceDurationUs = 10'000'000;
        f.project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 id = editops::overwriteClip(
            f.project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        f.project.endEdit();
        f.show();
        auto* menu = f.window->editPage()->findChild<QMenu*>(QStringLiteral("FlagColorMenu"));
        QVERIFY(menu);
        QCOMPARE(
            qobject_cast<QToolButton*>(menu->parent())->popupMode(), QToolButton::MenuButtonPopup);
        auto flags = [&] { return f.project.mediaPool()->item(mediaId)->flags; };
        f.selection.setClips({id});
        const int before = f.project.undoStack()->index();
        f.action("clip.flag")->trigger();
        QCOMPARE(flags(), 1u << flagIndex(QStringLiteral("blue")));
        QCOMPARE(f.project.undoStack()->index(), before + 1);
        for (QAction* a : menu->actions())
            if (a->data().toString() == QStringLiteral("green"))
                a->trigger();
        f.action("clip.flag")->trigger();
        QCOMPARE(flags(),
            (1u << flagIndex(QStringLiteral("blue"))) | (1u << flagIndex(QStringLiteral("green"))));
        f.action("clip.flag")->trigger(); // toggles off
        QCOMPARE(flags(), 1u << flagIndex(QStringLiteral("blue")));
        // Nothing selected: nothing happens.
        f.selection.clear();
        const int steps = f.project.undoStack()->index();
        f.action("clip.flag")->trigger();
        QCOMPARE(f.project.undoStack()->index(), steps);
    }
};

QTEST_MAIN(TestBughunt6)
#include "test_bughunt6.moc"
