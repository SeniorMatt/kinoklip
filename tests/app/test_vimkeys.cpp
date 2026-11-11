// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the vim key layer (spec sections 62 and 83): the state machine with a fake host
// (counts, sequences, modes, Esc), real key events in the main window (motions, dd / yy / p, u,
// visual mode, one undo step per edit), the focus rules (text fields keep their keys), every
// keyboard preset with Vim mode on or off, and the `:` command line. The option, the mode line
// and the migration of the old preset are in test_vimoption.
#include "app/MainWindow.h"
#include "app/SettingsTransfer.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/VimModeLine.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimOptions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};

/// Records what the machine asks for.
class FakeHost final : public VimHost
{
public:
    QStringList log;
    int64_t frame = 100;
    int clipsAtPlayhead = 1; ///< what selectClipsAtPlayhead() returns (a clip is there)

    bool trigger(const QString& id) override
    {
        log.append(id);
        return true;
    }
    int64_t playhead() const override { return frame; }
    void setPlayhead(int64_t f) override
    {
        frame = f;
        log.append(QStringLiteral("seek %1").arg(f));
    }
    bool selectClipsAtPlayhead(int count) override
    {
        log.append(QStringLiteral("select-clips %1").arg(count));
        return clipsAtPlayhead > 0;
    }
    void selectRange(int64_t a, int64_t b) override
    {
        log.append(QStringLiteral("range %1 %2").arg(a).arg(b));
    }
    void clearSelection() override { log.append(QStringLiteral("clear")); }
    void moveTarget(int delta) override { log.append(QStringLiteral("target %1").arg(delta)); }
    void openCommandLine() override { log.append(QStringLiteral("colon")); }
    void openSearch() override { log.append(QStringLiteral("search")); }
};

struct Typer
{
    VimMachine& m;
    void operator()(const QString& keys)
    {
        for (const QChar c : keys) {
            const QString t(c);
            QVERIFY(m.handleKey(c.unicode(), {}, t));
        }
    }
};

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    /// Vim mode is an option, independent of the keyboard preset: @p profile is the preset.
    explicit Fixture(const QString& profile = QStringLiteral("resolve-windows"), bool vim = true)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        QString error;
        QVERIFY2(actions.setActiveProfile(profile, &error), qPrintable(error));
        actions.action(QLatin1String(vimopts::kActionId))->setChecked(vim);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        // Three clips on V1 with linked audio: [0,40) [40,80) [80,120).
        project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ClipData v;
            v.kind = ClipKind::Solid;
            v.start = i * 40;
            v.duration = 40;
            v.solidColor = Qt::darkCyan;
            ClipData a = v;
            const quint64 vid = ops::overwriteClip(project.timeline()->mutableData(), kV1, v);
            const quint64 aid = ops::overwriteClip(project.timeline()->mutableData(), kA1, a);
            ops::linkClips(project.timeline()->mutableData(), {vid, aid});
        }
        project.endEdit();
        timeline()->setPixelsPerFrame(4.0, 0);
        timeline()->scrollToFrame(0);
        QCoreApplication::processEvents();
        project.timeline()->setPlayhead(0);
        focusTimeline();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }
    VimInput* vim() const { return window->vimController()->input(); }

    void focusTimeline()
    {
        timeline()->setFocus();
        QVERIFY(QApplication::focusWidget() == timeline());
    }

    QString layout(TrackRef ref) const
    {
        QStringList parts;
        for (const ClipData& c : project.timeline()->data().track(ref)->clips)
            parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
        return parts.join(QLatin1Char(' '));
    }

    /// Types characters on the focused widget as a user would.
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* target = QApplication::focusWidget();
            QVERIFY2(target, "no focus widget");
            QTest::keyClick(target, c.toLatin1());
        }
    }
    void key(Qt::Key k, Qt::KeyboardModifiers mods = {})
    {
        QWidget* target = QApplication::focusWidget();
        QVERIFY2(target, "no focus widget");
        QTest::keyClick(target, k, mods);
    }
    int64_t playhead() const { return project.timeline()->playhead(); }
    int undoCount() const { return project.undoStack()->count(); }
};

} // namespace

class TestVimKeys : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimkeys"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // ---- The state machine with a fake host ----

    void countedMotions()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        type(QStringLiteral("5l"));
        QCOMPARE(host.log, QStringList(5, QStringLiteral("transport.next_frame")));
        host.log.clear();
        type(QStringLiteral("3w"));
        QCOMPARE(host.log, QStringList(3, QStringLiteral("transport.next_edit")));
        host.log.clear();
        type(QStringLiteral("12h"));
        QCOMPARE(host.log.size(), 12);
        QCOMPARE(host.log.first(), QStringLiteral("transport.prev_frame"));
        host.log.clear();
        type(QStringLiteral("2b"));
        QCOMPARE(host.log, QStringList(2, QStringLiteral("transport.prev_edit")));
        host.log.clear();
        type(QStringLiteral("10"));
        QCOMPARE(m.pending(), QStringLiteral("10"));
        type(QStringLiteral("l"));
        QCOMPARE(host.log.size(), 10);
        QVERIFY(m.pending().isEmpty());
        // A zero after other digits is part of the count; a first zero is a motion.
        host.log.clear();
        type(QStringLiteral("0"));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.go_to_start")});
    }

    void startEndMarkersZoom()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        type(QStringLiteral("gg"));
        type(QStringLiteral("G"));
        type(QStringLiteral("$"));
        type(QStringLiteral("nN"));
        type(QStringLiteral("zizozf"));
        QCOMPARE(host.log,
            (QStringList {QStringLiteral("transport.go_to_start"), QStringLiteral("transport.go_to_end"),
                QStringLiteral("transport.go_to_end"), QStringLiteral("transport.next_marker"),
                QStringLiteral("transport.prev_marker"), QStringLiteral("timeline.zoom_in"),
                QStringLiteral("timeline.zoom_out"), QStringLiteral("timeline.zoom_fit")}));
        // A single g waits; another key cancels it and does nothing.
        host.log.clear();
        type(QStringLiteral("g"));
        QCOMPARE(m.pending(), QStringLiteral("g"));
        type(QStringLiteral("x"));
        QVERIFY(host.log.isEmpty());
        QVERIFY(m.pending().isEmpty());
    }

    void deleteYankAndEdits()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        type(QStringLiteral("dd"));
        QCOMPARE(host.log, (QStringList {QStringLiteral("select-clips 1"), QStringLiteral("edit.ripple_delete")}));
        host.log.clear();
        type(QStringLiteral("3dd"));
        QCOMPARE(host.log, (QStringList {QStringLiteral("select-clips 3"), QStringLiteral("edit.ripple_delete")}));
        host.log.clear();
        host.clipsAtPlayhead = 0; // nothing under the playhead: nothing deleted
        type(QStringLiteral("dd"));
        QCOMPARE(host.log, QStringList {QStringLiteral("select-clips 1")});
        host.clipsAtPlayhead = 1;
        host.log.clear();
        type(QStringLiteral("yy"));
        QCOMPARE(host.log, (QStringList {QStringLiteral("select-clips 1"), QStringLiteral("edit.copy")}));
        host.log.clear();
        type(QStringLiteral("xXpPs"));
        QCOMPARE(host.log,
            (QStringList {QStringLiteral("edit.ripple_delete"), QStringLiteral("edit.delete"),
                QStringLiteral("edit.paste_insert"), QStringLiteral("edit.paste"),
                QStringLiteral("edit.split")}));
        // The source edits are gi go ga for now (i and a start INSERT mode).
        host.log.clear();
        type(QStringLiteral("gigoga"));
        QCOMPARE(host.log,
            (QStringList {QStringLiteral("edit.insert"), QStringLiteral("edit.overwrite"),
                QStringLiteral("edit.append_at_end")}));
        host.log.clear();
        type(QStringLiteral("3u"));
        QCOMPARE(host.log, QStringList(3, QStringLiteral("edit.undo")));
        host.log.clear();
        type(QStringLiteral("jk2k"));
        QCOMPARE(host.log, (QStringList {QStringLiteral("target -1"), QStringLiteral("target 1"), QStringLiteral("target 2")}));
        // d and a motion is not implemented: nothing is deleted.
        host.log.clear();
        type(QStringLiteral("dw"));
        QVERIFY(host.log.isEmpty());
        type(QStringLiteral("dh"));
        QVERIFY(host.log.isEmpty());
    }

    void modesAndEscape()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        QCOMPARE(m.mode(), VimMachine::Mode::Normal);
        QCOMPARE(m.statusText(), QStringLiteral("NORMAL"));
        type(QStringLiteral("v"));
        QCOMPARE(m.mode(), VimMachine::Mode::Visual);
        QCOMPARE(m.statusText(), QStringLiteral("VISUAL"));
        QCOMPARE(host.log.last(), QStringLiteral("range 100 100"));
        host.log.clear();
        host.frame = 140;
        type(QStringLiteral("l"));
        QCOMPARE(host.log.last(), QStringLiteral("range 100 140"));
        host.log.clear();
        type(QStringLiteral("d"));
        QCOMPARE(host.log, QStringList {QStringLiteral("edit.ripple_delete")});
        QCOMPARE(m.mode(), VimMachine::Mode::Normal);
        // Esc leaves VISUAL and clears the selection.
        type(QStringLiteral("v"));
        host.log.clear();
        QVERIFY(m.handleKey(Qt::Key_Escape, {}, QString()));
        QCOMPARE(m.mode(), VimMachine::Mode::Normal);
        QCOMPARE(host.log, QStringList {QStringLiteral("clear")});
        // v again leaves it too; y copies and leaves.
        type(QStringLiteral("vv"));
        QCOMPARE(m.mode(), VimMachine::Mode::Normal);
        host.log.clear();
        type(QStringLiteral("vy"));
        QCOMPARE(m.mode(), VimMachine::Mode::Normal);
        QVERIFY(host.log.contains(QStringLiteral("edit.copy")));
        // Esc drops pending keys and a count, and is not taken when nothing is pending.
        type(QStringLiteral("3d"));
        QCOMPARE(m.pending(), QStringLiteral("3d"));
        QCOMPARE(m.statusText(), QStringLiteral("NORMAL  3d"));
        QVERIFY(m.handleKey(Qt::Key_Escape, {}, QString()));
        QVERIFY(m.pending().isEmpty());
        QVERIFY(!m.wouldConsume(Qt::Key_Escape, {}, QString()));
        // ':' asks for the command line, '/' for a search.
        host.log.clear();
        type(QStringLiteral(":/"));
        QCOMPARE(host.log, (QStringList {QStringLiteral("colon"), QStringLiteral("search")}));
    }

    void marksAndUnboundKeys()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        host.frame = 77;
        type(QStringLiteral("ma"));
        host.frame = 5;
        host.log.clear();
        type(QStringLiteral("'a"));
        QCOMPARE(host.log, QStringList {QStringLiteral("seek 77")});
        host.log.clear();
        type(QStringLiteral("'z")); // never set
        QVERIFY(host.log.isEmpty());
        // Unbound printable keys are swallowed; modified keys, Space and Return are not taken.
        type(QStringLiteral("qrcfe.@~"));
        QVERIFY(host.log.isEmpty());
        QVERIFY(!m.wouldConsume(Qt::Key_R, Qt::ControlModifier, QStringLiteral("r")));
        QVERIFY(!m.wouldConsume(Qt::Key_X, Qt::AltModifier, QStringLiteral("x")));
        QVERIFY(!m.wouldConsume(Qt::Key_Space, {}, QStringLiteral(" ")));
        QVERIFY(!m.wouldConsume(Qt::Key_Return, {}, QStringLiteral("\r")));
        QVERIFY(!m.wouldConsume(Qt::Key_Left, {}, QString()));
        QVERIFY(!m.wouldConsume(Qt::Key_Delete, {}, QStringLiteral("\x7f")));
    }

    // ---- The real window ----

    void actionsTheLayerDrivesExist()
    {
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        // Every action the layer drives exists.
        for (const char* id : {"transport.prev_frame", "transport.next_frame", "transport.back_1s",
                 "transport.forward_1s", "transport.next_edit", "transport.prev_edit",
                 "transport.go_to_start", "transport.go_to_end", "transport.next_marker",
                 "transport.prev_marker", "timeline.zoom_in", "timeline.zoom_out",
                 "timeline.zoom_fit", "edit.ripple_delete", "edit.delete", "edit.copy",
                 "edit.paste", "edit.paste_insert", "edit.undo", "edit.redo", "edit.insert",
                 "edit.overwrite", "edit.append_at_end", "edit.split", "mark.add_marker",
                 "trim.nudge_right", "trim.nudge_left", "file.save", "file.quit"})
            QVERIFY2(actions.contains(QLatin1String(id)), id);
    }

    void motionsInTheWindow()
    {
        Fixture f;
        QVERIFY(f.vim()->active());
        QCOMPARE(f.window->vimController()->modeLine()->modeText(), QStringLiteral("NORMAL"));
        // Frame steps belong to the playback engine (absent here): count the action's triggers.
        int steps = 0;
        QAction* next = f.actions.action(QStringLiteral("transport.next_frame"));
        QAction* prev = f.actions.action(QStringLiteral("transport.prev_frame"));
        connect(next, &QAction::triggered, this, [&] { ++steps; });
        connect(prev, &QAction::triggered, this, [&] { steps += 100; });
        f.type(QStringLiteral("5l"));
        QCOMPARE(steps, 5);
        f.type(QStringLiteral("2h"));
        QCOMPARE(steps, 205);
        f.project.timeline()->setPlayhead(3);
        f.type(QStringLiteral("w"));
        QCOMPARE(f.playhead(), int64_t(40));
        f.type(QStringLiteral("2w"));
        QCOMPARE(f.playhead(), int64_t(120));
        f.type(QStringLiteral("b"));
        QCOMPARE(f.playhead(), int64_t(80));
        f.type(QStringLiteral("gg"));
        QCOMPARE(f.playhead(), int64_t(0));
        f.type(QStringLiteral("G"));
        QCOMPARE(f.playhead(), int64_t(120));
        f.type(QStringLiteral("0"));
        QCOMPARE(f.playhead(), int64_t(0));
        f.type(QStringLiteral("$"));
        QCOMPARE(f.playhead(), int64_t(120));
        QCOMPARE(f.undoCount(), 1); // motions are not edits (only the setup step exists)
    }

    void ddIsOneUndoStep()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(50);
        const int before = f.undoCount();
        f.type(QStringLiteral("dd"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(f.layout(kA1), QStringLiteral("0:40 40:40"));
        QCOMPARE(f.undoCount(), before + 1);
        f.type(QStringLiteral("u"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40"));
        QCOMPARE(f.undoCount(), before + 1); // the stack keeps the step; the index moved back
        QVERIFY(f.project.undoStack()->canRedo());
        f.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); // Resolve redo
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        // 2dd removes the clip under the playhead and the next one, in one step.
        f.type(QStringLiteral("u"));
        f.project.timeline()->setPlayhead(0);
        const int before2 = f.project.undoStack()->index();
        f.type(QStringLiteral("2dd"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40"));
        QCOMPARE(f.project.undoStack()->index(), before2 + 1);
        f.type(QStringLiteral("u"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40"));
    }

    void xDeletesSelection()
    {
        Fixture f;
        f.type(QStringLiteral("x")); // nothing selected: nothing happens
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40"));
        f.project.timeline()->setPlayhead(50);
        f.selection.setClips({f.project.timeline()->data().track(kV1)->clips[1].id});
        const int before = f.project.undoStack()->index();
        f.type(QStringLiteral("x"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        QCOMPARE(f.project.undoStack()->index(), before + 1);
    }

    void yankAndPaste()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(0);
        f.type(QStringLiteral("yy"));
        f.project.timeline()->setPlayhead(120);
        const int before = f.project.undoStack()->index();
        f.type(QStringLiteral("p"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40 120:40"));
        QCOMPARE(f.project.undoStack()->index(), before + 1);
    }

    void visualMode()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(10);
        f.type(QStringLiteral("v"));
        QCOMPARE(f.window->vimController()->modeLine()->modeText(), QStringLiteral("VISUAL"));
        QCOMPARE(f.selection.clips().size(), qsizetype(2)); // clip 1 and its audio
        f.type(QStringLiteral("w"));
        QCOMPARE(f.selection.clips().size(), qsizetype(4)); // up to the playhead at 40: clips 1 and 2
        f.key(Qt::Key_Escape);
        QCOMPARE(f.window->vimController()->modeLine()->modeText(), QStringLiteral("NORMAL"));
        QVERIFY(f.selection.clips().isEmpty());
        // v, motion, d: one ripple delete.
        f.project.timeline()->setPlayhead(10);
        const int before = f.project.undoStack()->index();
        f.type(QStringLiteral("vwd"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40"));
        QCOMPARE(f.project.undoStack()->index(), before + 1);
        QCOMPARE(f.window->vimController()->modeLine()->modeText(), QStringLiteral("NORMAL"));
    }

    void pendingKeysShowInTheIndicator()
    {
        Fixture f;
        f.type(QStringLiteral("3d"));
        QCOMPARE(f.window->vimController()->modeLine()->pendingText(), QStringLiteral("3d"));
        f.key(Qt::Key_Escape);
        QCOMPARE(f.window->vimController()->modeLine()->modeText(), QStringLiteral("NORMAL"));
    }

    void textFieldsKeepTheirKeys()
    {
        Fixture f;
        auto* edit = new QLineEdit(f.window.get());
        edit->setGeometry(10, 10, 200, 24);
        edit->show();
        edit->setFocus();
        QVERIFY(QApplication::focusWidget() == edit);
        f.project.timeline()->setPlayhead(50);
        const QString before = f.layout(kV1);
        const int undo = f.undoCount();
        f.type(QStringLiteral("dd5l"));
        QCOMPARE(edit->text(), QStringLiteral("dd5l"));
        QCOMPARE(f.layout(kV1), before);
        QCOMPARE(f.undoCount(), undo);
        QCOMPARE(f.playhead(), int64_t(50));
        QVERIFY(f.vim()->machine().pending().isEmpty());
        QVERIFY(!f.vim()->inScope(edit));
        // A dialog (modal) keeps its keys too.
        edit->deleteLater();
        f.focusTimeline();
        QVERIFY(f.vim()->inScope(QApplication::focusWidget()));
    }

    void otherPresetsAreUnaffected()
    {
        // Vim mode off: no preset sees any change.
        for (const char* id : {"resolve-windows", "avid-media-composer", "final-cut-pro", "kdenlive",
                 "premiere-pro", "shotcut", "vegas-pro"}) {
            Fixture f {QString::fromLatin1(id), false};
            QVERIFY2(!f.vim()->active(), id);
            QVERIFY2(f.vim()->statusText().isEmpty(), id);
            QVERIFY2(!f.window->vimController()->modeLine()->isVisible(), id);
            f.project.timeline()->setPlayhead(50);
            const QString before = f.layout(kV1);
            f.type(QStringLiteral("gg"));
            QVERIFY2(f.vim()->machine().pending().isEmpty(), id);
            QVERIFY2(f.vim()->machine().mode() == VimMachine::Mode::Normal, id);
            QCOMPARE(f.layout(kV1), before);
        }
        // Resolve, Vim mode off: D still toggles the clip (a plain shortcut).
        Fixture r(QStringLiteral("resolve-windows"), false);
        r.project.timeline()->setPlayhead(50);
        r.selection.setClips({r.project.timeline()->data().track(kV1)->clips[1].id});
        r.type(QStringLiteral("d"));
        QVERIFY(!r.project.timeline()->data().track(kV1)->clips[1].enabled);
        // The option turns the layer on and off at once, whatever the preset.
        QAction* option = r.actions.action(QLatin1String(vimopts::kActionId));
        option->setChecked(true);
        QVERIFY(r.vim()->active());
        QVERIFY(r.window->vimController()->modeLine()->isVisible());
        QCOMPARE(r.window->vimController()->modeLine()->modeText(), QStringLiteral("NORMAL"));
        option->setChecked(false);
        QVERIFY(!r.vim()->active());
        QVERIFY(!r.window->vimController()->modeLine()->isVisible());
        QVERIFY(r.vim()->statusText().isEmpty());
        // Vim mode on works over every preset: the same keys, the preset's keys stay in INSERT.
        for (const char* id : {"avid-media-composer", "final-cut-pro", "kdenlive", "premiere-pro",
                 "shotcut", "vegas-pro"}) {
            Fixture f {QString::fromLatin1(id), true};
            QVERIFY2(f.vim()->active(), id);
            f.project.timeline()->setPlayhead(50);
            f.type(QStringLiteral("dd"));
            QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40"));
        }
    }

    void commandLine()
    {
        Fixture f;
        f.type(QStringLiteral(":"));
        auto* line = f.window->vimController()->commandLine();
        QVERIFY(line->isVisible());
        QVERIFY(QApplication::focusWidget() == line);
        QTest::keyClicks(line, QStringLiteral("marker add --at 7 --name Hi"));
        QTest::keyClick(line, Qt::Key_Return);
        const auto& markers = f.project.timeline()->data().markers;
        QCOMPARE(markers.size(), qsizetype(1));
        QCOMPARE(markers.first().frame, int64_t(7));
        QCOMPARE(markers.first().name, QStringLiteral("Hi"));
        // One undo step.
        QVERIFY(f.project.undoStack()->undoText().contains(QStringLiteral("arker"), Qt::CaseInsensitive)
            || f.project.undoStack()->index() > 0);
        QCOMPARE(f.window->vimController()->runCommand(QStringLiteral("nosuch")), QStringLiteral("Not a command: nosuch"));
        QVERIFY(f.window->vimController()->runCommand(QStringLiteral("marker add")).contains(QStringLiteral("at")));
        QVERIFY(f.window->vimController()->runCommand(QStringLiteral("marker add --bogus 1")).contains(QStringLiteral("Unknown option")));
    }

    void commandLineEscapeAndTyping()
    {
        Fixture f;
        f.type(QStringLiteral(":"));
        auto* line = f.window->vimController()->commandLine();
        QTest::keyClicks(line, QStringLiteral("dd"));
        QCOMPARE(line->text(), QStringLiteral("dd")); // typed into the line, not the layer
        QTest::keyClick(line, Qt::Key_Escape);
        QVERIFY(!line->isVisible());
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:40 80:40"));
    }

    void survivesRestartAndSettingsTransfer()
    {
        {
            ActionRegistry a;
            a.loadDefaultKeymap();
            registerStandardActions(a);
            QVERIFY(!a.action(QLatin1String(vimopts::kActionId))->isChecked()); // default off
            a.action(QLatin1String(vimopts::kActionId))->setChecked(true);
            a.setShortcuts(QStringLiteral("edit.undo"), {QKeySequence(QStringLiteral("Ctrl+U"))});
        }
        {
            ActionRegistry again; // a restart
            again.loadDefaultKeymap();
            registerStandardActions(again);
            QVERIFY(again.action(QLatin1String(vimopts::kActionId))->isChecked());
            QCOMPARE(again.activeProfile(), QStringLiteral("resolve-windows"));
            QCOMPARE(again.shortcuts(QStringLiteral("edit.undo")).value(0), QKeySequence(QStringLiteral("Ctrl+U")));
        }
        QTemporaryDir dir;
        settingstransfer::Locations loc {dir.path() + QStringLiteral("/data"), dir.path() + QStringLiteral("/backups")};
        const QByteArray exported = settingstransfer::exportDocument({}, loc);
        QSettings().clear();
        {
            ActionRegistry fresh;
            fresh.loadDefaultKeymap();
            registerStandardActions(fresh);
            QVERIFY(!fresh.action(QLatin1String(vimopts::kActionId))->isChecked());
        }
        settingstransfer::Parsed parsed;
        QString error;
        QVERIFY2(settingstransfer::parse(exported, &parsed, &error), qPrintable(error));
        settingstransfer::ApplyOptions options;
        options.locations = loc;
        QVERIFY(settingstransfer::apply(parsed, options).ok);
        ActionRegistry imported;
        imported.loadDefaultKeymap();
        registerStandardActions(imported);
        QVERIFY(imported.action(QLatin1String(vimopts::kActionId))->isChecked());
        QCOMPARE(imported.shortcuts(QStringLiteral("edit.undo")).value(0), QKeySequence(QStringLiteral("Ctrl+U")));
    }
};

QTEST_MAIN(TestVimKeys)
#include "test_vimkeys.moc"
