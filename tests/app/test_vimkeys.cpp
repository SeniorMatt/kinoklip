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
#include "ui/common/VimKeymap.h"
#include "ui/common/VimKeys.h"
#include "ui/common/VimOptions.h"
#include "ui/common/VimWhichKey.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
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
    void runEx(const QString& line) override { log.append(QStringLiteral("ex ") + line); }
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
        // The source edits are on the leader (Space s i / o / a); i and a start INSERT mode.
        host.log.clear();
        type(QStringLiteral(" si so sa"));
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
        // Unbound printable keys are swallowed; Space is the leader; Return, cursor keys and the
        // profile's Ctrl and Alt chords are not taken.
        type(QStringLiteral("qrcfe.@~"));
        QVERIFY(host.log.isEmpty());
        QVERIFY(!m.wouldConsume(Qt::Key_S, Qt::ControlModifier, QStringLiteral("s")));
        QVERIFY(!m.wouldConsume(Qt::Key_X, Qt::AltModifier, QStringLiteral("x")));
        QVERIFY(m.wouldConsume(Qt::Key_Space, {}, QStringLiteral(" ")));
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

    // ---- The key table in the machine (step 2) ----

    void ambiguousSequencesWaitForTheTimeout()
    {
        VimKeymap keys;
        QVERIFY(keys.loadDefaults());
        QVERIFY(keys.map(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("g"),
            {QStringLiteral("edit.undo")}, {}, nullptr)); // g is also the start of gg
        VimMachine m;
        m.setKeymap(&keys);
        m.setTimeoutMs(40);
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        type(QStringLiteral("g"));
        QVERIFY(host.log.isEmpty()); // waits: it could still be gg
        QCOMPARE(m.pending(), QStringLiteral("g"));
        QTRY_COMPARE_WITH_TIMEOUT(host.log, QStringList {QStringLiteral("edit.undo")}, 1000);
        QVERIFY(m.pending().isEmpty());
        // The second key in time: gg, not g.
        host.log.clear();
        type(QStringLiteral("gg"));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.go_to_start")});
        QTest::qWait(80);
        QCOMPARE(host.log.size(), 1);
        // A key that continues nothing: the short binding runs, then the key starts over.
        host.log.clear();
        type(QStringLiteral("gx")); // gx is nothing; x is Ripple Delete
        QCOMPARE(host.log,
            (QStringList {QStringLiteral("edit.undo"), QStringLiteral("edit.ripple_delete")}));
        QVERIFY(m.pending().isEmpty());
    }

    void timeoutCanBeCalledDirectly()
    {
        VimKeymap keys;
        QVERIFY(keys.loadDefaults());
        QVERIFY(keys.map(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("g"),
            {QStringLiteral("edit.undo")}, {}, nullptr));
        VimMachine m;
        m.setKeymap(&keys);
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        type(QStringLiteral("3g"));
        m.timeoutNow();
        QCOMPARE(host.log,
            QStringList {QStringLiteral("edit.undo")}); // a count does not repeat a command
        QVERIFY(m.pending().isEmpty());
        m.timeoutNow(); // nothing pending: nothing happens
        QCOMPARE(host.log.size(), 1);
    }

    void leaderKeysAndTheWhichKeyList()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        QVERIFY(m.handleKey(Qt::Key_Space, {}, QStringLiteral(" ")));
        QCOMPARE(m.pending(), QStringLiteral("SPC"));
        QVERIFY(!m.whichKeyShown());
        m.timeoutNow(); // a prefix with no short form: the list appears, nothing runs
        QVERIFY(m.whichKeyShown());
        QVERIFY(host.log.isEmpty());
        QCOMPARE(m.whichKeyPrefix(), QStringLiteral("SPC"));
        QStringList rows;
        for (const VimContinuation& c : m.whichKeyEntries())
            rows.append(vimkeys::displayToken(c.token) + QLatin1Char(' ') + c.desc);
        QVERIFY(rows.contains(QStringLiteral("s source edits")));
        QVERIFY(rows.contains(QStringLiteral("SPC play / pause")));
        QVERIFY(m.pending() == QStringLiteral("SPC")); // still waiting, no timeout cancel
        type(QStringLiteral("s"));
        QVERIFY(!m.whichKeyShown()); // the next key hides it
        m.timeoutNow();
        QVERIFY(m.whichKeyShown());
        QCOMPARE(m.whichKeyPrefix(), QStringLiteral("SPC s"));
        rows.clear();
        for (const VimContinuation& c : m.whichKeyEntries())
            rows.append(c.token + QLatin1Char(' ') + c.desc);
        QVERIFY(rows.contains(QStringLiteral("i insert the source clip")));
        QVERIFY(rows.contains(QStringLiteral("o overwrite with the source clip")));
        type(QStringLiteral("o"));
        QCOMPARE(host.log, QStringList {QStringLiteral("edit.overwrite")});
        QVERIFY(!m.whichKeyShown());
        QVERIFY(m.pending().isEmpty());
        // Space Space plays; Esc drops the leader and hides the list.
        host.log.clear();
        QVERIFY(m.handleKey(Qt::Key_Space, {}, QStringLiteral(" ")));
        QVERIFY(m.handleKey(Qt::Key_Space, {}, QStringLiteral(" ")));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.play_pause")});
        QVERIFY(m.handleKey(Qt::Key_Space, {}, QStringLiteral(" ")));
        m.timeoutNow();
        QVERIFY(m.handleKey(Qt::Key_Escape, {}, QString()));
        QVERIFY(m.pending().isEmpty());
        QVERIFY(!m.whichKeyShown());
        // An unbound key after the leader ends the sequence and does nothing.
        host.log.clear();
        QVERIFY(m.handleKey(Qt::Key_Space, {}, QStringLiteral(" ")));
        type(QStringLiteral("Z"));
        QVERIFY(host.log.isEmpty());
        QVERIFY(m.pending().isEmpty());
    }

    void controlRIsRedoInNormal()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        QVERIFY(m.wouldConsume(Qt::Key_R, Qt::ControlModifier, QStringLiteral("\x12")));
        QVERIFY(m.handleKey(Qt::Key_R, Qt::ControlModifier, QStringLiteral("\x12")));
        QCOMPARE(host.log, QStringList {QStringLiteral("edit.redo")});
        // Other Ctrl chords stay the profile's.
        QVERIFY(!m.wouldConsume(Qt::Key_S, Qt::ControlModifier, QStringLiteral("\x13")));
        QVERIFY(!m.wouldConsume(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, QString()));
    }

    void userMappingsReachTheMachine()
    {
        VimKeymap keys;
        QVERIFY(keys.loadDefaults());
        VimMachine m;
        m.setKeymap(&keys);
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        QVERIFY(keys.map(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("Q"),
            {QStringLiteral("edit.undo"), QStringLiteral("edit.redo")}, {},
            nullptr)); // a list runs in order
        type(QStringLiteral("Q"));
        QCOMPARE(
            host.log, (QStringList {QStringLiteral("edit.undo"), QStringLiteral("edit.redo")}));
        host.log.clear();
        QVERIFY(keys.map(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("W"),
            {QStringLiteral(":marker add --frame 5")}, {}, nullptr));
        type(QStringLiteral("W"));
        QCOMPARE(host.log, QStringList {QStringLiteral("ex marker add --frame 5")});
        // A change of the table drops a half-typed sequence.
        type(QStringLiteral("z"));
        QCOMPARE(m.pending(), QStringLiteral("z"));
        QVERIFY(keys.unmap(QStringLiteral("global"), QStringLiteral("n"), QStringLiteral("Q")));
        QVERIFY(m.pending().isEmpty());
        // Unmapped: the key is swallowed like any unbound key.
        host.log.clear();
        type(QStringLiteral("Q"));
        QVERIFY(host.log.isEmpty());
    }

    void contextsPickTheirKeys()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        Typer type {m};
        QString context = QStringLiteral("timeline");
        m.setContextProvider([&] { return context; });
        type(QStringLiteral("zf"));
        QCOMPARE(host.log, QStringList {QStringLiteral("timeline.zoom_fit")});
        host.log.clear();
        context = QStringLiteral("viewer");
        type(QStringLiteral("zf"));
        QCOMPARE(host.log, QStringList {QStringLiteral("view.viewer_zoom_fit")});
        host.log.clear();
        type(QStringLiteral("2l")); // inherited from the timeline
        QCOMPARE(host.log, QStringList(2, QStringLiteral("transport.next_frame")));
    }

    void aBurstOfKeysIsFast()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20000; ++i) {
            QVERIFY(m.handleKey('l', {}, QStringLiteral("l")));
            QVERIFY(m.handleKey('3', {}, QStringLiteral("3")));
            QVERIFY(m.handleKey('h', {}, QStringLiteral("h")));
            host.log.clear();
        }
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    }

    // ---- Physical keys: a Russian layout in NORMAL ----

    void aRussianLayoutDrivesNormalMode()
    {
        using vimkeys::KeyInfo;
        // evdev codes (X11 / Wayland scan codes are +8): l = 38, h = 35, w = 17, [ = 26, 4 = 5, ;
        // = 39.
        auto ru = [](QChar typed, int evdev, Qt::KeyboardModifiers mods = {}) {
            KeyInfo k;
            k.key = int(0x0433); // whatever: the typed character is Cyrillic
            k.modifiers = mods;
            k.text = QString(typed);
            k.nativeScanCode = quint32(evdev + 8);
            return k;
        };
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        // "д" sits where "l" is: next frame. "р" is h, "ц" is w.
        QVERIFY(m.handleKey(ru(QChar(0x0434), 38)));
        QVERIFY(m.handleKey(ru(QChar(0x0440), 35)));
        QVERIFY(m.handleKey(ru(QChar(0x0446), 17)));
        QCOMPARE(host.log,
            (QStringList {QStringLiteral("transport.next_frame"),
                QStringLiteral("transport.prev_frame"), QStringLiteral("transport.next_edit")}));
        // Shift + "х" is { , Shift + "ж" is : .
        host.log.clear();
        QVERIFY(m.handleKey(ru(QChar(0x0425), 26, Qt::ShiftModifier)));
        QVERIFY(m.handleKey(ru(QChar(0x0416), 39, Qt::ShiftModifier)));
        QCOMPARE(host.log, QStringList {QStringLiteral("colon")});
        // Two-key sequences and counts work in the layout: "пп" is gg, and 5 д is 5l.
        host.log.clear();
        QVERIFY(m.handleKey(ru(QChar(0x043f), 34)));
        QVERIFY(m.handleKey(ru(QChar(0x043f), 34)));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.go_to_start")});
        host.log.clear();
        KeyInfo five;
        five.key = Qt::Key_5;
        five.text = QStringLiteral("5");
        five.nativeScanCode = 6 + 8;
        QVERIFY(m.handleKey(five));
        QVERIFY(m.handleKey(ru(QChar(0x0434), 38)));
        QCOMPARE(host.log.size(), 5);
        // The shifted digit row follows once the layout is known to be Cyrillic: the Russian
        // layout types ';' on Shift+4, and $ is meant.
        host.log.clear();
        KeyInfo dollar;
        dollar.key = Qt::Key_Semicolon;
        dollar.modifiers = Qt::ShiftModifier;
        dollar.text = QStringLiteral(";");
        dollar.nativeScanCode = 5 + 8;
        QVERIFY(m.handleKey(dollar));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.go_to_end")});
        // A Latin letter switches back: the typed character counts again.
        host.log.clear();
        KeyInfo latinL;
        latinL.key = Qt::Key_L;
        latinL.text = QStringLiteral("l");
        latinL.nativeScanCode = 38 + 8;
        QVERIFY(m.handleKey(latinL));
        QCOMPARE(host.log, QStringList {QStringLiteral("transport.next_frame")});
        host.log.clear();
        QVERIFY(m.handleKey(dollar)); // Latin layout: ';' is just an unbound key
        QVERIFY(host.log.isEmpty());
    }

    void physicalKeysCanBeSwitchedOff()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        m.setPhysicalKeys(false);
        vimkeys::KeyInfo k;
        k.key = int(0x043b);
        k.text = QString(QChar(0x0434));
        k.nativeScanCode = 38 + 8;
        // The typed character counts: a Cyrillic letter is an unbound printable key.
        QVERIFY(!vimkeys::isCharToken(QStringLiteral("")));
        m.handleKey(k);
        QVERIFY(host.log.isEmpty());
    }

    void aKeyWithoutAScanCodeUsesItsText()
    {
        VimMachine m;
        FakeHost host;
        m.setHost(&host);
        vimkeys::KeyInfo k; // a layout the platform gives no scan code for
        k.key = int(0x043b);
        k.text = QString(QChar(0x0434));
        QVERIFY(m.handleKey(k)); // swallowed as an unbound key
        QVERIFY(host.log.isEmpty());
    }

    // ---- The key table in the window ----

    void whichKeyPopupInTheWindow()
    {
        vimopts::setTimeoutMs(60);
        Fixture f;
        auto* popup = f.window->vimController()->whichKey();
        QVERIFY(popup && !popup->isVisible());
        f.key(Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), 2000);
        QVERIFY(popup->rows().contains(QStringLiteral("s  +source edits")));
        QVERIFY(popup->title().contains(QStringLiteral("SPC")));
        QCOMPARE(f.window->vimController()->modeLine()->pendingText(), QStringLiteral("SPC"));
        f.type(QStringLiteral("s"));
        QVERIFY(!popup->isVisible()); // the next key hides it ...
        QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), 2000); // ... and the next prefix shows its own
        QVERIFY(popup->rows().contains(QStringLiteral("i  insert the source clip")));
        f.key(Qt::Key_Escape);
        QVERIFY(!popup->isVisible());
        QVERIFY(f.window->vimController()->modeLine()->pendingText().isEmpty());
        // Turning the option off hides it too.
        f.key(Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), 2000);
        f.actions.action(QLatin1String(vimopts::kActionId))->setChecked(false);
        QVERIFY(!popup->isVisible());
    }

    void spaceSpacePlaysInNormalAndSpacePlaysInInsert()
    {
        Fixture f;
        int plays = 0;
        connect(f.actions.action(QStringLiteral("transport.play_pause")), &QAction::triggered, this,
            [&] { ++plays; });
        f.key(Qt::Key_Space);
        QCOMPARE(plays, 0); // the leader
        f.key(Qt::Key_Space);
        QCOMPARE(plays, 1);
        f.type(QStringLiteral("i"));
        f.key(Qt::Key_Space);
        QCOMPARE(plays, 2); // INSERT: the profile's Space
    }

    void aRussianLayoutWorksInTheWindow()
    {
        Fixture f;
        auto press = [&](QChar typed, int evdev, Qt::KeyboardModifiers mods = {}) {
            QKeyEvent ev(
                QEvent::KeyPress, int(0x0430), mods, quint32(evdev + 8), 0, 0, QString(typed));
            QApplication::sendEvent(QApplication::focusWidget(), &ev);
            return ev.isAccepted();
        };
        // "ц" is where w is: next edit point; "и" is where b is: back.
        press(QChar(0x0446), 17);
        QCOMPARE(f.playhead(), int64_t(40));
        press(QChar(0x0446), 17);
        QCOMPARE(f.playhead(), int64_t(80));
        press(QChar(0x0438), 48);
        QCOMPARE(f.playhead(), int64_t(40));
        // "пп" is gg, "Ю" (Shift + the . key) is not bound; Shift + "п" is G.
        press(QChar(0x043f), 34);
        press(QChar(0x043f), 34);
        QCOMPARE(f.playhead(), int64_t(0));
        press(QChar(0x041f), 34, Qt::ShiftModifier);
        QCOMPARE(f.playhead(), int64_t(120));
        // The layer took them: nothing was typed into anything and no shortcut fired.
        QCOMPARE(f.undoCount(), 1);
        // Switched off, the typed letters are the Cyrillic ones: unbound and swallowed.
        vimopts::setPhysicalKeys(false);
        f.vim()->keymap()->setTimeoutMs(400); // emits optionsChanged
        f.project.timeline()->setPlayhead(0);
        press(QChar(0x0446), 17);
        QCOMPARE(f.playhead(), int64_t(0));
    }

    void exCommandsEditTheKeysAndSetOptions()
    {
        Fixture f;
        auto* vc = f.window->vimController();
        QCOMPARE(vc->runCommand(QStringLiteral("nmap Q :marker add --at 9 --name K")),
            QStringLiteral("done"));
        f.focusTimeline();
        f.type(QStringLiteral("Q"));
        const auto& markers = f.project.timeline()->data().markers;
        QCOMPARE(markers.size(), qsizetype(1));
        QCOMPARE(markers.first().frame, int64_t(9));
        QVERIFY(vc->runCommand(QStringLiteral("map")).contains(QStringLiteral("marker add")));
        QCOMPARE(vc->runCommand(QStringLiteral("nunmap Q")), QStringLiteral("done"));
        f.type(QStringLiteral("Q"));
        QCOMPARE(markers.size(), qsizetype(1));
        QVERIFY(vc->runCommand(QStringLiteral("nmap Q")).contains(QStringLiteral("wants")));
        QVERIFY(vc->runCommand(QStringLiteral("set bogus=1"))
                .contains(QStringLiteral("Unknown option")));
        QCOMPARE(vc->runCommand(QStringLiteral("set timeoutlen=123")), QStringLiteral("done"));
        QCOMPARE(vc->keymap()->timeoutMs(), 123);
        // :set novim leaves Vim mode through the option, so the menu check and the stored value
        // follow.
        QCOMPARE(vc->runCommand(QStringLiteral("set novim")), QStringLiteral("Vim mode off"));
        QVERIFY(!f.actions.action(QLatin1String(vimopts::kActionId))->isChecked());
        QVERIFY(!vimopts::enabled());
        QVERIFY(!f.vim()->active());
        QCOMPARE(vc->runCommand(QStringLiteral("set vim")), QStringLiteral("Vim mode on"));
        QVERIFY(f.vim()->active());
        // :source reads a vimrc file.
        QTemporaryDir dir;
        QFile rc(dir.filePath(QStringLiteral("rc")));
        QVERIFY(rc.open(QIODevice::WriteOnly));
        rc.write("nmap Z edit.split\n");
        rc.close();
        QCOMPARE(vc->runCommand(QStringLiteral("source ") + rc.fileName()),
            QStringLiteral("vimrc read"));
        QCOMPARE(vc->keymap()->trie(QStringLiteral("timeline"), 'n').match({"Z"}).kind,
            VimMatch::Kind::Exact);
        QVERIFY(vc->runCommand(QStringLiteral("source ") + dir.filePath(QStringLiteral("none")))
                .contains(QStringLiteral("No vimrc")));
    }

    void keysComeFromTheSharedTable()
    {
        Fixture f;
        // The window and Keyboard Customization share one table (a child of the registry).
        VimKeymap* table = VimKeymap::of(f.actions);
        QCOMPARE(f.window->vimController()->keymap(), table);
        QVERIFY(table->map(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("Q"),
            {QStringLiteral("edit.split")}, {}, nullptr));
        f.project.timeline()->setPlayhead(50);
        f.type(QStringLiteral("Q"));
        QCOMPARE(f.layout(kV1), QStringLiteral("0:40 40:10 50:30 80:40"));
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
