// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of TimelineCommandSuspension (docs/behavior-spec.md 42b.26): the Media and
// Deliver pages have no editable timeline, so the Edit page's timeline keys do nothing there
// and work again on the Edit page; transport, undo and the page keys stay; the Cut and Audio
// pages keep them; two helpers holding one command share it.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/MediaPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/TimelineCommandSuspension.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    quint64 clipId = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.name = QStringLiteral("Plate");
        clip.start = 0;
        clip.duration = 50;
        project.beginEdit(QStringLiteral("Add Clip"));
        clipId = editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->activateWindow();
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    void page(const char* actionId)
    {
        action(actionId)->trigger();
        QCoreApplication::processEvents();
    }
    [[nodiscard]] int clipCount() const
    {
        int n = 0;
        for (const TrackData& t : project.timeline()->data().videoTracks)
            n += int(t.clips.size());
        return n;
    }
    [[nodiscard]] bool hasClip() const
    {
        return editops::clipById(project.timeline()->data(), clipId) != nullptr;
    }
};

/// The timeline commands the tests press (Delete keys, blade at the playhead, select all).
constexpr const char* kTimelineCommands[] = {"edit.delete", "edit.ripple_delete", "edit.split",
    "edit.copy", "edit.paste", "edit.select_all", "tool.blade", "trim.nudge_left",
    "clip.toggle_enabled", "multicam.angle_1", "timeline.add_video_track"};
/// Commands every page keeps (undo / redo too, but they follow the undo stack).
constexpr const char* kKept[]
    = {"transport.play_pause", "transport.next_frame", "edit.history_window", "view.page_edit",
        "view.page_media", "view.page_deliver", "edit.keyboard_customization"};

} // namespace

class TestPageCommands : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_pagecommands"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void mediaPageSuspendsTimelineCommands()
    {
        Fixture f;
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        for (const char* id : kTimelineCommands)
            QVERIFY2(f.action(id)->isEnabled(), id);
        f.project.timeline()->setPlayhead(20);
        f.selection.setClips({f.clipId});

        f.page("view.page_media");
        QCOMPARE(f.window->currentPageId(), QStringLiteral("media"));
        for (const char* id : kTimelineCommands)
            QVERIFY2(!f.action(id)->isEnabled(), id);
        for (const char* id : kKept)
            QVERIFY2(f.action(id)->isEnabled(), id);
        // The media pool's own Clip menu commands stay.
        for (const char* id :
            {"mediapool.new_multicam", "mediapool.sync_waveform", "clip.attributes"})
            QVERIFY2(f.action(id)->isEnabled(), id);

        // Delete, Backspace and blade (Ctrl+\) on the Media page leave the timeline alone.
        QWidget* pool = f.window->mediaPage()->mediaPool();
        pool->setFocus();
        QTest::keyClick(pool, Qt::Key_Delete);
        QTest::keyClick(pool, Qt::Key_Backspace);
        QTest::keyClick(pool, Qt::Key_Backslash, Qt::ControlModifier);
        f.action("edit.ripple_delete")->trigger(); // disabled: no-op
        f.action("edit.split")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(f.hasClip());
        QCOMPARE(f.clipCount(), 1);

        // Back on the Edit page the same commands work again.
        f.page("view.page_edit");
        for (const char* id : kTimelineCommands)
            QVERIFY2(f.action(id)->isEnabled(), id);
        f.action("edit.split")->trigger();
        QCOMPARE(f.clipCount(), 2);
        f.project.undo();
        QCOMPARE(f.clipCount(), 1);
        f.selection.setClips({f.clipId});
        f.action("edit.ripple_delete")->trigger();
        QVERIFY(!f.hasClip());
    }

    void deliverPageSuspendsEditingButKeepsMarks()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(10);
        f.page("view.page_deliver");
        QCOMPARE(f.window->currentPageId(), QStringLiteral("deliver"));
        for (const char* id : kTimelineCommands)
            QVERIFY2(!f.action(id)->isEnabled(), id);
        for (const char* id : kKept)
            QVERIFY2(f.action(id)->isEnabled(), id);
        // I / O set the render range there.
        for (const char* id : {"mark.in", "mark.out", "mark.clear_both", "mark.go_to_in"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        f.action("mark.in")->trigger();
        QCOMPARE(f.project.timeline()->data().markIn, 10);
        f.action("edit.split")->trigger();
        QCOMPARE(f.clipCount(), 1);
        f.page("view.page_edit");
        QVERIFY(f.action("edit.split")->isEnabled());
    }

    void cutAndAudioPagesKeepTimelineCommands()
    {
        // Both pages have a timeline that edits the same one: nothing is suspended.
        Fixture f;
        for (const char* page : {"view.page_cut", "view.page_audio"}) {
            f.page(page);
            for (const char* id : {"edit.delete", "edit.ripple_delete", "edit.split", "edit.undo",
                     "transport.play_pause"})
                QVERIFY2(f.action(id)->isEnabled(), qPrintable(QLatin1String(page) + u' ' + id));
        }
    }

    void switchingBetweenSuspendingPagesKeepsCommandsOff()
    {
        // Media -> Compositing -> Deliver -> Edit: off all the way, on again at the end,
        // whatever order the stack sends hide and show in.
        Fixture f;
        f.page("view.page_media");
        f.page("view.page_compositing");
        QVERIFY(!f.action("edit.split")->isEnabled());
        QVERIFY(!f.action("edit.delete")->isEnabled());
        f.page("view.page_color");
        QVERIFY(!f.action("edit.split")->isEnabled());
        f.page("view.page_deliver");
        QVERIFY(!f.action("edit.split")->isEnabled());
        f.page("view.page_media");
        QVERIFY(!f.action("mark.in")->isEnabled()); // Deliver keeps it, Media does not
        f.page("view.page_edit");
        for (const char* id : kTimelineCommands)
            QVERIFY2(f.action(id)->isEnabled(), id);
        QVERIFY(f.action("mark.in")->isEnabled());
    }

    void colorPageSuspendsTimelineButKeepsItsCommands()
    {
        // Spec 44b.7: the Color page grades clips, it does not edit the timeline.
        Fixture f;
        f.page("view.page_color");
        QCOMPARE(f.window->currentPageId(), QStringLiteral("color"));
        for (const char* id : kTimelineCommands)
            QVERIFY2(!f.action(id)->isEnabled(), id);
        for (const char* id : kKept)
            QVERIFY2(f.action(id)->isEnabled(), id);
        for (const char* id : {"color.add_serial", "color.bypass_node", "color.grab_still"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        f.page("view.page_edit");
        for (const char* id : kTimelineCommands)
            QVERIFY2(f.action(id)->isEnabled(), id);
        QVERIFY(!f.action("color.add_serial")->isEnabled());
    }

    void helpersShareACommand()
    {
        // Two helpers holding one command: it comes back with the last one; a command its
        // owner disabled is never enabled by a helper.
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        QWidget a, b;
        auto* sa = new TimelineCommandSuspension(&a, &actions);
        auto* sb = new TimelineCommandSuspension(&b, &actions);
        QAction* split = actions.action(QStringLiteral("edit.split"));
        QAction* join = actions.action(QStringLiteral("edit.join"));
        join->setEnabled(false); // its owner's state
        QVERIFY(sa->commandIds().contains(QStringLiteral("edit.split")));
        for (const char* id : {"edit.undo", "edit.redo", "transport.play_pause", "view.page_edit"})
            QVERIFY2(!sa->commandIds().contains(QLatin1String(id)), id);
        sa->suspend();
        sb->suspend(); // shown before the other one hides
        QVERIFY(!split->isEnabled());
        sa->resume();
        QVERIFY(!split->isEnabled());
        sb->resume();
        QVERIFY(split->isEnabled());
        QVERIFY(!join->isEnabled());
        // Show / hide events of the watched widget drive it; deleting it restores.
        a.show();
        QVERIFY(sa->isSuspended());
        QVERIFY(!split->isEnabled());
        a.hide();
        QVERIFY(!sa->isSuspended());
        QVERIFY(split->isEnabled());
        a.show();
        delete sa;
        QVERIFY(split->isEnabled());
    }
};

QTEST_MAIN(TestPageCommands)
#include "test_pagecommands.moc"
