// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the command line v2 and the pickers of Vim mode (spec section 83): the `:` line with
// completion and history, every built-in ex command, and the <leader>f pickers with fuzzy
// filtering, <C-j> / <C-k> and applying.
#include "app/MainWindow.h"
#include "app/PageBar.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/VimEx.h"
#include "app/VimModeLine.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaItem.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/common/VimCommandLine.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimOptions.h"
#include "ui/common/VimPicker.h"
#include "ui/help/Guide.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QUrl>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        actions.action(QLatin1String(vimopts::kActionId))->setChecked(true);
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
        project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ClipData v;
            v.kind = ClipKind::Solid;
            v.start = i * 40;
            v.duration = 40;
            v.solidColor = Qt::darkCyan;
            ops::overwriteClip(project.timeline()->mutableData(), kV1, v);
        }
        project.endEdit();
        timeline()->setPixelsPerFrame(4.0, 0);
        QCoreApplication::processEvents();
        project.timeline()->setPlayhead(0);
        timeline()->setFocus();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }
    VimController* vim() const { return window->vimController(); }
    VimModeLine* modeLine() const { return vim()->modeLine(); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            if (c == QLatin1Char(' '))
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
            else
                QTest::keyClick(QApplication::focusWidget(), c.toLatin1());
        }
    }
    /// Runs a line through the real `:` line.
    void ex(const QString& line)
    {
        timeline()->setFocus();
        type(QStringLiteral(":"));
        auto* cmd = vim()->commandLine();
        QTest::keyClicks(cmd, line);
        QTest::keyClick(cmd, Qt::Key_Return);
    }
    QString run(const QString& line, bool* ok = nullptr) { return vim()->runCommand(line, ok); }
    QString layout() const
    {
        QStringList parts;
        for (const ClipData& c : project.timeline()->data().videoTracks.front().clips)
            parts << QStringLiteral("%1:%2").arg(c.start).arg(c.duration);
        return parts.join(QLatin1Char(' '));
    }
};

} // namespace

class TestVimCmdline : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimcmdline"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // ---- Parsing and completion (no widgets) ----

    void abbreviationsResolveToCommands()
    {
        using vimex::parse;
        QCOMPARE(parse(QStringLiteral("w")).name, QStringLiteral("write"));
        QCOMPARE(parse(QStringLiteral("wr")).name, QStringLiteral("write"));
        QCOMPARE(parse(QStringLiteral("q!")).name, QStringLiteral("quit"));
        QVERIFY(parse(QStringLiteral("q!")).bang);
        QCOMPARE(parse(QStringLiteral("wq")).name, QStringLiteral("wq"));
        QCOMPARE(parse(QStringLiteral("x")).name, QStringLiteral("xit"));
        QCOMPARE(parse(QStringLiteral("e foo")).name, QStringLiteral("edit"));
        QCOMPARE(parse(QStringLiteral("e foo")).arg, QStringLiteral("foo"));
        QCOMPARE(parse(QStringLiteral("bn")).name, QStringLiteral("bnext"));
        QCOMPARE(parse(QStringLiteral("bp")).name, QStringLiteral("bprevious"));
        QCOMPARE(parse(QStringLiteral("bd")).name, QStringLiteral("bdelete"));
        QCOMPARE(parse(QStringLiteral("b 2")).name, QStringLiteral("buffer"));
        QCOMPARE(parse(QStringLiteral("ls")).name, QStringLiteral("ls"));
        QCOMPARE(parse(QStringLiteral("pa edit")).name, QStringLiteral("page"));
        QCOMPARE(parse(QStringLiteral("u")).name, QStringLiteral("undo"));
        QCOMPARE(parse(QStringLiteral("red")).name, QStringLiteral("redo"));
        QCOMPARE(parse(QStringLiteral("se snap")).name, QStringLiteral("set"));
        QCOMPARE(parse(QStringLiteral("noh")).name, QStringLiteral("nohlsearch"));
        QCOMPARE(parse(QStringLiteral("nm Q x")).name, QStringLiteral("nmap"));
        QCOMPARE(parse(QStringLiteral(":h")).name, QStringLiteral("help"));
        // A registry command is not a built-in.
        QVERIFY(parse(QStringLiteral("marker add --frame 3")).name.isEmpty());
        QVERIFY(parse(QStringLiteral("clip")).name.isEmpty());
        // Go-to-time lines.
        for (const char* t : {"120", "+10", "-5", "$", "00:00:01:10"}) {
            QVERIFY2(parse(QLatin1String(t)).isGoto, t);
        }
        QVERIFY(!parse(QStringLiteral("w")).isGoto);
    }

    void completionOffersCommandsAndArguments()
    {
        vimex::Sources sources;
        sources.actionIds = [] { return QStringList {"edit.undo", "edit.redo", "file.save"}; };
        sources.registryCommands
            = [] { return QStringList {"marker add", "marker list", "clip trim"}; };
        sources.timelines = [] { return QStringList {"Main", "Trailer"}; };
        sources.timecodes = [] { return QStringList {"01:00:00:00", "01:00:05:00"}; };
        const auto names = [&](const QString& before) {
            QStringList l = vimex::complete(before, sources).candidates;
            l.sort();
            return l;
        };
        QVERIFY(names(QStringLiteral("w")).contains(QStringLiteral("write")));
        QVERIFY(names(QStringLiteral("w")).contains(QStringLiteral("wq")));
        QVERIFY(names(QStringLiteral("ma")).contains(QStringLiteral("map")));
        QVERIFY(names(QStringLiteral("ma")).contains(QStringLiteral("marker")));
        QCOMPARE(vimex::complete(QStringLiteral("ma"), sources).start, 0);
        QCOMPARE(names(QStringLiteral("marker ")), (QStringList {"add", "list"}));
        QCOMPARE(vimex::complete(QStringLiteral("marker a"), sources).start, 7);
        QCOMPARE(names(QStringLiteral("marker a")), (QStringList {"add"}));
        QCOMPARE(names(QStringLiteral("page e")), (QStringList {"edit"}));
        QVERIFY(names(QStringLiteral("page ")).contains(QStringLiteral("deliver")));
        QVERIFY(names(QStringLiteral("page ")).contains(QStringLiteral("next")));
        QCOMPARE(names(QStringLiteral("action edit.u")), (QStringList {"edit.undo"}));
        QCOMPARE(names(QStringLiteral("e T")), (QStringList {"Trailer"}));
        QCOMPARE(names(QStringLiteral("set sn")), (QStringList {"snap"}));
        QVERIFY(names(QStringLiteral("set ")).contains(QStringLiteral("leader=")));
        QCOMPARE(names(QStringLiteral("export ")), (QStringList {"edl", "markers", "otio"}));
        // A number completes to the timecodes that start with it.
        QCOMPARE(names(QStringLiteral("01:00:05")), (QStringList {"01:00:05:00"}));
        QVERIFY(names(QStringLiteral("zzz")).isEmpty());
    }

    // ---- The line widget: history and completion ----

    void historyKeepsOrderAndDropsDuplicates()
    {
        VimCommandLine line;
        line.addHistory(QStringLiteral("w"));
        line.addHistory(QStringLiteral("set snap"));
        line.addHistory(QStringLiteral("  "));
        line.addHistory(QStringLiteral("w"));
        QCOMPARE(line.history(), (QStringList {"set snap", "w"}));
        for (int i = 0; i < 300; ++i)
            line.addHistory(QString::number(i));
        QCOMPARE(line.history().size(), qsizetype(VimCommandLine::kMaxHistory));
        QCOMPARE(line.history().last(), QStringLiteral("299"));
    }

    void upAndDownRecallWithPrefixAndPlainKeysWithout()
    {
        VimCommandLine line;
        line.setHistory({"write", "set snap", "set loop", "page edit"});
        line.show();
        line.begin();
        QTest::keyClick(&line, Qt::Key_Up);
        QCOMPARE(line.text(), QStringLiteral("page edit")); // no prefix: the newest
        QTest::keyClick(&line, Qt::Key_Up);
        QCOMPARE(line.text(), QStringLiteral("set loop"));
        QTest::keyClick(&line, Qt::Key_Down);
        QCOMPARE(line.text(), QStringLiteral("page edit"));
        QTest::keyClick(&line, Qt::Key_Down);
        QCOMPARE(line.text(), QString()); // back to what was typed
        line.begin();
        QTest::keyClicks(&line, QStringLiteral("set"));
        QTest::keyClick(&line, Qt::Key_Up);
        QCOMPARE(line.text(), QStringLiteral("set loop")); // prefix filter
        QTest::keyClick(&line, Qt::Key_Up);
        QCOMPARE(line.text(), QStringLiteral("set snap"));
        QTest::keyClick(&line, Qt::Key_Up);
        QCOMPARE(line.text(), QStringLiteral("set snap")); // the oldest match: stays
        QTest::keyClick(&line, Qt::Key_Down);
        QTest::keyClick(&line, Qt::Key_Down);
        QCOMPARE(line.text(), QStringLiteral("set")); // the typed text again
        // <C-p> <C-n> and <C-k> <C-j> ignore the prefix.
        QTest::keyClick(&line, Qt::Key_P, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("page edit"));
        QTest::keyClick(&line, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("set loop"));
        QTest::keyClick(&line, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("page edit"));
        QTest::keyClick(&line, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("set"));
    }

    void editingKeysOfTheLine()
    {
        VimCommandLine line;
        line.show();
        line.begin(QStringLiteral("set snap now"));
        QTest::keyClick(&line, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(line.cursorPosition(), 0);
        QTest::keyClick(&line, Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(line.cursorPosition(), 12);
        QTest::keyClick(&line, Qt::Key_H, Qt::ControlModifier);
        QCOMPARE(line.cursorPosition(), 11);
        QTest::keyClick(&line, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(line.cursorPosition(), 12);
        QTest::keyClick(&line, Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("set snap "));
        QTest::keyClick(&line, Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(line.text(), QStringLiteral("set "));
        QTest::keyClick(&line, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(line.text(), QString());
        // Backspace on an empty line closes it.
        QSignalSpy cancelled(&line, &VimCommandLine::cancelled);
        QTest::keyClick(&line, Qt::Key_Backspace);
        QCOMPARE(cancelled.count(), 1);
        QTest::keyClick(&line, Qt::Key_Escape);
        QTest::keyClick(&line, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&line, Qt::Key_BracketLeft, Qt::ControlModifier);
        QCOMPARE(cancelled.count(), 4);
    }

    void tabCompletesFillsExtendsAndCycles()
    {
        VimCommandLine line;
        line.setCompleter([](const QString& before, int) {
            vimex::Sources s;
            s.timelines = [] { return QStringList {"Trailer", "Trailer 2", "Main"}; };
            return vimex::complete(before, s);
        });
        line.show();
        line.begin();
        QTest::keyClicks(&line, QStringLiteral("page ed"));
        QTest::keyClick(&line, Qt::Key_Tab);
        QCOMPARE(line.text(), QStringLiteral("page edit")); // one match: filled in
        line.begin();
        QTest::keyClicks(&line, QStringLiteral("e Tr"));
        QSignalSpy lists(&line, &VimCommandLine::candidatesChanged);
        QTest::keyClick(&line, Qt::Key_Tab);
        QCOMPARE(line.text(), QStringLiteral("e Trailer")); // extended to what they share
        QCOMPARE(line.candidates(), (QStringList {"Trailer", "Trailer 2"}));
        QTest::keyClick(&line, Qt::Key_Tab);
        QCOMPARE(line.text(), QStringLiteral("e Trailer"));
        QCOMPARE(line.candidateIndex(), 0);
        QTest::keyClick(&line, Qt::Key_Tab);
        QCOMPARE(line.text(), QStringLiteral("e Trailer 2"));
        QTest::keyClick(&line, Qt::Key_Backtab);
        QCOMPARE(line.text(), QStringLiteral("e Trailer"));
        QTest::keyClick(&line, Qt::Key_N, Qt::ControlModifier); // steps while a list is up
        QCOMPARE(line.text(), QStringLiteral("e Trailer 2"));
        // Typing drops the list.
        QTest::keyClick(&line, Qt::Key_X);
        QVERIFY(line.candidates().isEmpty());
        QVERIFY(lists.count() >= 3);
    }

    // ---- The `:` line in the window ----

    void historyIsKeptAcrossLinesAndSessions()
    {
        {
            Fixture f;
            f.ex(QStringLiteral("set snap?"));
            f.ex(QStringLiteral("nosuchcommand"));
            f.ex(QStringLiteral("set snap?"));
            QCOMPARE(f.vim()->history(), (QStringList {"nosuchcommand", "set snap?"}));
            f.timeline()->setFocus();
            f.type(QStringLiteral(":"));
            auto* cmd = f.vim()->commandLine();
            QTest::keyClick(cmd, Qt::Key_Up);
            QCOMPARE(cmd->text(), QStringLiteral("set snap?"));
            QTest::keyClick(cmd, Qt::Key_Up);
            QCOMPARE(cmd->text(), QStringLiteral("nosuchcommand"));
            QTest::keyClick(cmd, Qt::Key_Escape);
        }
        Fixture again; // a new window reads the stored history
        QCOMPARE(again.vim()->history(), (QStringList {"nosuchcommand", "set snap?"}));
    }

    void completionPopupShowsUnderTheLine()
    {
        Fixture f;
        f.timeline()->setFocus();
        f.type(QStringLiteral(":"));
        auto* cmd = f.vim()->commandLine();
        QTest::keyClicks(cmd, QStringLiteral("b"));
        QTest::keyClick(cmd, Qt::Key_Tab);
        QVERIFY(f.vim()->wildMenu()->isVisible());
        QVERIFY(cmd->candidates().contains(QStringLiteral("bnext")));
        QTest::keyClick(cmd, Qt::Key_Escape);
        QVERIFY(!f.vim()->wildMenu()->isVisible());
        QVERIFY(!cmd->isVisible());
    }

    void errorsShowInTheModeLine()
    {
        Fixture f;
        f.ex(QStringLiteral("nosuch"));
        QCOMPARE(f.modeLine()->messageText(), QStringLiteral("Not a command: nosuch"));
        QVERIFY(f.modeLine()->messageIsError());
        f.ex(QStringLiteral("set snap?"));
        QVERIFY(!f.modeLine()->messageIsError());
        bool ok = true;
        f.run(QStringLiteral("page nowhere"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("set bogus=1"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("page edit"), &ok);
        QVERIFY(ok);
    }

    void searchLineHasItsOwnHistory()
    {
        Fixture f;
        f.ex(QStringLiteral("set snap?"));
        f.timeline()->setFocus();
        f.type(QStringLiteral("/"));
        auto* cmd = f.vim()->commandLine();
        QTest::keyClick(cmd, Qt::Key_Up);
        QCOMPARE(cmd->text(), QString()); // the `:` lines are not offered to a search
        QTest::keyClicks(cmd, QStringLiteral("zzz"));
        QTest::keyClick(cmd, Qt::Key_Return);
        QVERIFY(f.modeLine()->messageIsError()); // no match
        f.type(QStringLiteral("/"));
        QTest::keyClick(cmd, Qt::Key_Up);
        QCOMPARE(cmd->text(), QStringLiteral("zzz"));
        QTest::keyClick(cmd, Qt::Key_Escape);
        QCOMPARE(f.vim()->history(), (QStringList {"set snap?"}));
    }

    void backwardSearchFollowsTheDirectionAndMacrosRecordLines()
    {
        Fixture f;
        f.project.timeline()->mutableData().markers = {Marker {20, QStringLiteral("zz1")},
            Marker {60, QStringLiteral("zz2")}, Marker {90, QStringLiteral("zz3")}};
        f.project.timeline()->setPlayhead(100);
        f.timeline()->setFocus();
        f.type(QStringLiteral("?"));
        auto* cmd = f.vim()->commandLine();
        QVERIFY(cmd->placeholderText().startsWith(QLatin1Char('?')));
        QTest::keyClicks(cmd, QStringLiteral("zz"));
        QTest::keyClick(cmd, Qt::Key_Return);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(90)); // backwards from 100
        f.type(QStringLiteral("n"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(60)); // n follows the ?
        f.type(QStringLiteral("N"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(90));
        f.type(QStringLiteral("/"));
        QVERIFY(cmd->placeholderText().startsWith(QLatin1Char('/')));
        QTest::keyClick(cmd, Qt::Key_Escape);
        f.timeline()->setFocus();

        // a macro with a command line in it runs the line without opening it
        f.type(QStringLiteral("qa"));
        QCOMPARE(f.modeLine()->recordingText(), QStringLiteral("recording @a"));
        f.ex(QStringLiteral("set snap?"));
        f.timeline()->setFocus();
        f.type(QStringLiteral("q"));
        QVERIFY(f.modeLine()->recordingText().isEmpty());
        f.modeLine()->clearMessage();
        f.type(QStringLiteral("@a"));
        QVERIFY(!cmd->isVisible());
        QVERIFY(!f.modeLine()->messageText().isEmpty());
        f.modeLine()->clearMessage();
        f.type(QStringLiteral("@:"));
        QVERIFY(!f.modeLine()->messageText().isEmpty());
    }

    // ---- The ex commands ----

    void goToFrameAndTimecode()
    {
        Fixture f;
        f.ex(QStringLiteral("80"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(80));
        f.ex(QStringLiteral("+10"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(90));
        f.ex(QStringLiteral("-30"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(60));
        f.ex(QStringLiteral("$"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(120));
        f.ex(QStringLiteral("0"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(0));
        f.ex(QStringLiteral("-5"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(0)); // never before the start
        const QString tc = timecode::formatRecord(50, f.project.timeline()->data());
        f.ex(tc);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(50));
        bool ok = true;
        f.run(QStringLiteral("99:99:99:99:99"), &ok);
        QVERIFY(!ok || f.project.timeline()->playhead() >= 0);
    }

    void pagesByNameNumberAndStep()
    {
        Fixture f;
        const auto checked
            = [&](const char* id) { return f.actions.action(QLatin1String(id))->isChecked(); };
        f.ex(QStringLiteral("page deliver"));
        QVERIFY(checked("view.page_deliver"));
        f.run(QStringLiteral("page 1"));
        QVERIFY(checked("view.page_media"));
        f.run(QStringLiteral("page next"));
        QVERIFY(checked("view.page_cut"));
        f.run(QStringLiteral("page prev"));
        f.run(QStringLiteral("page prev"));
        QVERIFY(checked("view.page_deliver")); // wraps round, as gT does
        f.run(QStringLiteral("page next"));
        QVERIFY(checked("view.page_media"));
        f.run(QStringLiteral("pa comp"));
        QVERIFY(checked("view.page_compositing"));
        f.run(QStringLiteral("page 3"));
        QVERIFY(checked("view.page_edit"));
        bool ok = true;
        f.run(QStringLiteral("page 9"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("deliver"));
        QVERIFY(checked("view.page_deliver"));
        f.run(QStringLiteral("render"));
        QVERIFY(checked("view.page_deliver"));
        f.run(QStringLiteral("page edit"));
    }

    void timelineTabsAreTheBuffers()
    {
        Fixture f;
        const quint64 first = f.project.activeTimelineUid();
        QVERIFY(f.run(QStringLiteral("ls")).contains(QStringLiteral("%a")));
        f.ex(QStringLiteral("enew Trailer"));
        const quint64 second = f.project.activeTimelineUid();
        QVERIFY(second != first);
        QCOMPARE(f.project.tabs().size(), qsizetype(2));
        const QString listing = f.run(QStringLiteral("ls"));
        QVERIFY(listing.contains(QStringLiteral("Trailer")));
        f.run(QStringLiteral("b 1"));
        QCOMPARE(f.project.activeTimelineUid(), first);
        f.run(QStringLiteral("bn"));
        QCOMPARE(f.project.activeTimelineUid(), second);
        f.run(QStringLiteral("bp"));
        QCOMPARE(f.project.activeTimelineUid(), first);
        f.run(QStringLiteral("e trailer")); // by name, any case
        QCOMPARE(f.project.activeTimelineUid(), second);
        f.run(QStringLiteral("b 1"));
        f.run(QStringLiteral("buffer trai")); // a part of the name
        QCOMPARE(f.project.activeTimelineUid(), second);
        bool ok = true;
        f.run(QStringLiteral("b 7"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("e nosuchfile"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("bd"));
        QCOMPARE(f.project.tabs().size(), qsizetype(1));
        QCOMPARE(f.project.activeTimelineUid(), first);
        f.run(QStringLiteral("new"));
        QCOMPARE(f.project.tabs().size(), qsizetype(2));
    }

    void undoRedoHistoryPlay()
    {
        Fixture f;
        f.run(QStringLiteral("marker add --at 5 --name A"));
        f.run(QStringLiteral("marker add --at 9 --name B"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(2));
        f.run(QStringLiteral("undo"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(1));
        f.run(QStringLiteral("u"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(0));
        f.run(QStringLiteral("redo 2"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(2));
        f.run(QStringLiteral("undo 2"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(0));
    }

    void setTogglesEditingOptionsAndAnswersQueries()
    {
        Fixture f;
        QAction* snap = f.actions.action(QStringLiteral("timeline.snapping"));
        const bool before = snap->isChecked();
        f.run(QStringLiteral("set nosnap"));
        QVERIFY(!snap->isChecked());
        f.run(QStringLiteral("set snap"));
        QVERIFY(snap->isChecked());
        QCOMPARE(f.run(QStringLiteral("set snap?")), QStringLiteral("snap"));
        f.run(QStringLiteral("set snap!"));
        QVERIFY(!snap->isChecked());
        QCOMPARE(f.run(QStringLiteral("set snap?")), QStringLiteral("nosnap"));
        f.run(QStringLiteral("set invsnap"));
        QVERIFY(snap->isChecked());
        Q_UNUSED(before);
        QAction* loop = f.actions.action(QStringLiteral("transport.loop"));
        f.run(QStringLiteral("set noloop"));
        f.run(QStringLiteral("set loop"));
        QVERIFY(loop->isChecked());
        f.run(QStringLiteral("set nolinked snap"));
        QVERIFY(!f.actions.action(QStringLiteral("timeline.linked_selection"))->isChecked());
        QCOMPARE(f.run(QStringLiteral("set vim?")), QStringLiteral("vim"));
        QVERIFY(f.run(QStringLiteral("set leader?")).contains(QStringLiteral("leader=")));
        QCOMPARE(f.run(QStringLiteral("se timeoutlen=222")), QStringLiteral("done"));
        QVERIFY(f.run(QStringLiteral("set timeoutlen?")).contains(QStringLiteral("222")));
        bool ok = true;
        f.run(QStringLiteral("set bogus"), &ok);
        QVERIFY(!ok);
        QCOMPARE(f.run(QStringLiteral("set novim")), QStringLiteral("Vim mode off"));
        QVERIFY(!vimopts::enabled());
    }

    void mappingsHelpActionAndRegistryCommands()
    {
        Fixture f;
        QCOMPARE(
            f.run(QStringLiteral("nm Q :marker add --at 9 --name K<CR>")), QStringLiteral("done"));
        QVERIFY(f.run(QStringLiteral("map")).contains(QStringLiteral("marker add")));
        QCOMPARE(f.run(QStringLiteral("nunmap Q")), QStringLiteral("done"));
        QVERIFY(f.run(QStringLiteral("noh")).isEmpty());
        // :action by id, by menu text, and a bare id.
        bool ok = true;
        f.run(QStringLiteral("marker add --at 3 --name Z"));
        f.run(QStringLiteral("action edit.undo"));
        QVERIFY(f.project.timeline()->data().markers.isEmpty());
        f.run(QStringLiteral("action edit.redo"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(1));
        f.run(QStringLiteral("edit.undo"));
        QVERIFY(f.project.timeline()->data().markers.isEmpty());
        // By the menu text (a text that does not change, unlike "Undo ...").
        const quint64 first = f.project.activeTimelineUid();
        f.run(QStringLiteral("enew Other"));
        QVERIFY(f.project.activeTimelineUid() != first);
        f.run(QStringLiteral("action next timeline tab"), &ok);
        QVERIFY(ok);
        QCOMPARE(f.project.activeTimelineUid(), first);
        f.run(QStringLiteral("action no.such"), &ok);
        QVERIFY(!ok);
        f.run(QStringLiteral("action"), &ok);
        QVERIFY(!ok);
        // :help and :cheat open the guide.
        QStringList opened;
        guide::setUrlOpenerForTesting([&](const QUrl& url) {
            opened << url.toString();
            return true;
        });
        QVERIFY(!f.run(QStringLiteral("help"), &ok).isNull() || ok);
        QVERIFY(ok);
        f.run(QStringLiteral("cheat"), &ok);
        QVERIFY(ok);
        QVERIFY(opened.size() >= 2);
        QVERIFY(opened.last().contains(QStringLiteral("vim")));
        guide::setUrlOpenerForTesting({});
    }

    void writeSavesAndQuitBangClosesWithoutAsking()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            Fixture f;
            QString error;
            QVERIFY2(f.project.save(dir.filePath(QStringLiteral("a.kkproj")), &error),
                qPrintable(error));
            f.run(QStringLiteral("marker add --at 5 --name A"));
            QVERIFY(f.project.isDirty());
            QCOMPARE(f.run(QStringLiteral("w")), QStringLiteral("saved"));
            QVERIFY(!f.project.isDirty());
            // :w with the action unavailable is an error, not a crash.
            f.actions.action(QStringLiteral("file.save"))->setEnabled(false);
            bool ok = true;
            f.run(QStringLiteral("write"), &ok);
            QVERIFY(!ok);
            f.actions.action(QStringLiteral("file.save"))->setEnabled(true);
        }
        {
            Fixture f; // :q! on unsaved changes closes without the question
            f.run(QStringLiteral("marker add --at 5 --name A"));
            QVERIFY(f.project.isDirty());
            QCOMPARE(f.run(QStringLiteral("q!")), QStringLiteral("closing"));
            QTRY_VERIFY(!f.window->isVisible());
        }
        {
            Fixture f; // :wq saves, then closes
            QString error;
            QVERIFY(f.project.save(dir.filePath(QStringLiteral("b.kkproj")), &error));
            f.run(QStringLiteral("marker add --at 5 --name A"));
            f.run(QStringLiteral("wq"));
            QTRY_VERIFY(!f.window->isVisible());
            QVERIFY(!f.project.isDirty());
        }
    }

    // ---- Pickers ----

    void fuzzyScoreRanksAndFilters()
    {
        QVERIFY(VimPicker::score(QStringLiteral("udo"), QStringLiteral("Undo")) > 0); // subsequence
        QVERIFY(VimPicker::score(QStringLiteral("xyz"), QStringLiteral("Undo")) < 0);
        QCOMPARE(VimPicker::score(QString(), QStringLiteral("anything")), 0);
        // A substring beats a scattered match; a word start beats the middle of a word.
        QVERIFY(VimPicker::score(QStringLiteral("und"), QStringLiteral("Undo"))
            > VimPicker::score(QStringLiteral("udo"), QStringLiteral("Undo")));
        QVERIFY(VimPicker::score(QStringLiteral("cut"), QStringLiteral("Cut Page"))
            > VimPicker::score(QStringLiteral("cut"), QStringLiteral("Execute Page")));
        // Every word must match; the order of the words does not matter.
        QVERIFY(VimPicker::score(QStringLiteral("page cut"), QStringLiteral("Cut Page")) > 0);
        QVERIFY(VimPicker::score(QStringLiteral("page zzz"), QStringLiteral("Cut Page")) < 0);
        // Smart case: a capital letter makes the word case sensitive.
        QVERIFY(VimPicker::score(QStringLiteral("cut"), QStringLiteral("Cut")) > 0);
        QVERIFY(VimPicker::score(QStringLiteral("CUT"), QStringLiteral("Cut")) < 0);
        QVERIFY(VimPicker::score(QStringLiteral("Cut"), QStringLiteral("Cut")) > 0);
    }

    void pickerFiltersMovesWrapsAndChooses()
    {
        QWidget host;
        host.resize(900, 600);
        host.show();
        VimPicker picker(&host);
        QVector<VimPickItem> items;
        for (const char* t : {"Alpha", "Beta", "Gamma", "Alphabet"})
            items.append({QLatin1String(t), QString(), QLatin1String(t), {}});
        QString chosen;
        bool alt = false;
        picker.open(QStringLiteral("Test"), items, [&](const VimPickItem& i, bool a) {
            chosen = i.id;
            alt = a;
        });
        QVERIFY(picker.isVisible());
        QCOMPARE(picker.visibleTexts(), (QStringList {"Alpha", "Beta", "Gamma", "Alphabet"}));
        QCOMPARE(picker.currentRow(), 0);
        QTest::keyClick(picker.filterField(), Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(picker.currentText(), QStringLiteral("Beta"));
        QTest::keyClick(picker.filterField(), Qt::Key_K, Qt::ControlModifier);
        QTest::keyClick(picker.filterField(), Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(picker.currentText(), QStringLiteral("Alphabet")); // wraps to the end
        QTest::keyClick(picker.filterField(), Qt::Key_Down);
        QCOMPARE(picker.currentText(), QStringLiteral("Alpha"));
        QTest::keyClick(picker.filterField(), Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(picker.currentText(), QStringLiteral("Beta"));
        QTest::keyClick(picker.filterField(), Qt::Key_P, Qt::ControlModifier);
        QCOMPARE(picker.currentText(), QStringLiteral("Alpha"));
        QTest::keyClicks(picker.filterField(), QStringLiteral("alph"));
        QCOMPARE(picker.visibleTexts(), (QStringList {"Alpha", "Alphabet"}));
        QCOMPARE(picker.currentRow(), 0); // the filter resets the selection
        QTest::keyClick(picker.filterField(), Qt::Key_J, Qt::ControlModifier);
        QTest::keyClick(picker.filterField(), Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(chosen, QStringLiteral("Alphabet"));
        QVERIFY(alt);
        QVERIFY(!picker.isVisible());
        // Esc closes without choosing.
        chosen.clear();
        picker.open(
            QStringLiteral("Test"), items, [&](const VimPickItem& i, bool) { chosen = i.id; });
        QTest::keyClick(picker.filterField(), Qt::Key_Escape);
        QVERIFY(!picker.isVisible());
        QVERIFY(chosen.isEmpty());
        picker.open(
            QStringLiteral("Test"), items, [&](const VimPickItem& i, bool) { chosen = i.id; });
        QTest::keyClick(picker.filterField(), Qt::Key_C, Qt::ControlModifier);
        QVERIFY(!picker.isVisible());
        // Nothing matches: Enter does nothing and the picker stays.
        picker.open(
            QStringLiteral("Test"), items, [&](const VimPickItem& i, bool) { chosen = i.id; });
        QTest::keyClicks(picker.filterField(), QStringLiteral("qqq"));
        QVERIFY(picker.visibleTexts().isEmpty());
        QTest::keyClick(picker.filterField(), Qt::Key_Return);
        QVERIFY(picker.isVisible());
        QVERIFY(chosen.isEmpty());
    }

    void leaderKeysOpenPickersAndTheModeLineSaysCommand()
    {
        Fixture f;
        f.timeline()->setFocus();
        f.type(QStringLiteral(" fc")); // <Space>fc
        QTRY_VERIFY(f.vim()->picker()->isOpen());
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("COMMAND"));
        QVERIFY(QApplication::focusWidget() == f.vim()->picker()->filterField());
        QVERIFY(f.vim()->picker()->visibleTexts().size() > 50); // the whole action list
        QTest::keyClicks(f.vim()->picker()->filterField(), QStringLiteral("undo"));
        QVERIFY(f.vim()->picker()->visibleTexts().first().contains(QStringLiteral("Undo")));
        QTest::keyClick(f.vim()->picker()->filterField(), Qt::Key_Escape);
        QVERIFY(!f.vim()->picker()->isOpen());
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("NORMAL"));
        QVERIFY(QApplication::focusWidget() == f.timeline()); // the focus comes back
    }

    void commandPickerRunsTheAction()
    {
        Fixture f;
        f.run(QStringLiteral("marker add --at 5 --name A"));
        QCOMPARE(f.project.timeline()->data().markers.size(), qsizetype(1));
        QVERIFY(f.vim()->openPicker(QStringLiteral("commands")));
        auto* picker = f.vim()->picker();
        picker->setFilter(QStringLiteral("edit.undo"));
        QVERIFY(
            picker->currentText().startsWith(QStringLiteral("Undo"))); // the text names the step
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QVERIFY(f.project.timeline()->data().markers.isEmpty());
        QVERIFY(!picker->isOpen());
    }

    void timelineAndMarkerPickers()
    {
        Fixture f;
        const quint64 first = f.project.activeTimelineUid();
        f.run(QStringLiteral("enew Trailer"));
        f.run(QStringLiteral("marker add --at 70 --name Beat"));
        f.run(QStringLiteral("marker add --at 20 --name Intro"));
        QVERIFY(f.vim()->openPicker(QStringLiteral("markers")));
        auto* picker = f.vim()->picker();
        QCOMPARE(picker->visibleTexts(), (QStringList {"Intro", "Beat"})); // in time order
        picker->setFilter(QStringLiteral("beat"));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(70));
        QVERIFY(f.vim()->openPicker(QStringLiteral("timelines")));
        QVERIFY(picker->visibleTexts().contains(QStringLiteral("Trailer")));
        QCOMPARE(picker->visibleTexts().size(), qsizetype(2));
        picker->setFilter(QString());
        // The first timeline is the other one.
        for (int i = 0; i < picker->visibleTexts().size(); ++i) {
            if (picker->currentText() != QStringLiteral("Trailer"))
                break;
            picker->moveSelection(1);
        }
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QCOMPARE(f.project.activeTimelineUid(), first);
    }

    void mediaAndBinPickers()
    {
        Fixture f;
        MediaPool* pool = f.project.mediaPool();
        const QString bin = pool->addBin(QStringLiteral("Interviews"));
        MediaItem a;
        a.name = QStringLiteral("rooftop.mp4");
        a.path = QStringLiteral("/nonexistent/rooftop.mp4");
        a.binId = bin;
        MediaItem b;
        b.name = QStringLiteral("harbour.mov");
        b.path = QStringLiteral("/nonexistent/harbour.mov");
        const QString idA = pool->addItem(a);
        pool->addItem(b);
        MediaPoolPanel* panel = nullptr;
        for (MediaPoolPanel* candidate : f.window->findChildren<MediaPoolPanel*>()) {
            if (candidate->isVisible())
                panel = candidate;
        }
        QVERIFY(panel);
        QSignalSpy opened(panel, &MediaPoolPanel::openInSourceViewer);
        QVERIFY(f.vim()->openPicker(QStringLiteral("media")));
        auto* picker = f.vim()->picker();
        QCOMPARE(picker->visibleTexts().size(), qsizetype(2));
        picker->setFilter(QStringLiteral("roof"));
        QCOMPARE(picker->visibleTexts(), (QStringList {"rooftop.mp4"}));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toString(), idA);
        // Bins.
        QVERIFY(f.vim()->openPicker(QStringLiteral("bins")));
        QVERIFY(picker->visibleTexts().contains(QStringLiteral("Interviews")));
        QVERIFY(picker->visibleTexts().contains(QStringLiteral("Master")));
        picker->setFilter(QStringLiteral("inter"));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QVERIFY(!picker->isOpen());
    }

    void effectPickerAppliesToTheClipUnderTheCursor()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(50); // in the second clip
        QVERIFY(f.vim()->openPicker(QStringLiteral("effects")));
        auto* picker = f.vim()->picker();
        picker->setFilter(QStringLiteral("Gaussian Blur"));
        QCOMPARE(picker->currentText(), QStringLiteral("Gaussian Blur"));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        const auto& clips = f.project.timeline()->data().videoTracks.front().clips;
        QCOMPARE(int(clips[0].effects.size()), 0);
        QCOMPARE(int(clips[1].effects.size()), 1);
        QCOMPARE(int(clips[2].effects.size()), 0);
        // Alt+Enter only looks.
        QVERIFY(f.vim()->openPicker(QStringLiteral("effects")));
        picker->setFilter(QStringLiteral("Gaussian Blur"));
        QTest::keyClick(picker->filterField(), Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(int(f.project.timeline()->data().videoTracks.front().clips[1].effects.size()), 1);
    }

    void transitionPickerWorksAtTheEditPoint()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(40); // the cut between clip 1 and 2
        QVERIFY(f.vim()->openPicker(QStringLiteral("effects")));
        auto* picker = f.vim()->picker();
        picker->setFilter(QStringLiteral("Cross Dissolve"));
        QVERIFY(picker->currentText().contains(QStringLiteral("Dissolve")));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        const auto& clips = f.project.timeline()->data().videoTracks.front().clips;
        QVERIFY(clips[1].transitionIn.has_value()); // the head of the clip that starts at the cut
        QCOMPARE(clips[1].transitionIn->type, QStringLiteral("cross_dissolve"));
        QVERIFY(!clips[0].transitionOut.has_value());
    }

    void helpKeymapsAndHistoryPickers()
    {
        Fixture f;
        QStringList opened;
        guide::setUrlOpenerForTesting([&](const QUrl& url) {
            opened << url.toString();
            return true;
        });
        QVERIFY(f.vim()->openPicker(QStringLiteral("help")));
        auto* picker = f.vim()->picker();
        QVERIFY(picker->visibleTexts().size() > 5);
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QCOMPARE(opened.size(), qsizetype(1));
        guide::setUrlOpenerForTesting({});
        // Vim keys: choosing one that is an action runs it.
        f.run(QStringLiteral("marker add --at 5 --name A"));
        QVERIFY(f.vim()->openPicker(QStringLiteral("keymaps")));
        QVERIFY(picker->visibleTexts().size() > 80);
        picker->setFilter(QStringLiteral("undo (count"));
        QVERIFY(picker->currentText().contains(QStringLiteral("undo")));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QVERIFY(f.project.timeline()->data().markers.isEmpty());
        // q: shows the history, newest first, and runs the line.
        f.ex(QStringLiteral("set snap?"));
        f.ex(QStringLiteral("page 1"));
        f.timeline()->setFocus();
        f.type(QStringLiteral("q:"));
        QTRY_VERIFY(picker->isOpen());
        QCOMPARE(picker->visibleTexts().first(), QStringLiteral("page 1"));
        QTest::keyClick(picker->filterField(), Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(picker->currentText(), QStringLiteral("set snap?"));
        QTest::keyClick(picker->filterField(), Qt::Key_Return);
        QCOMPARE(f.modeLine()->messageText().left(4), QStringLiteral("snap").left(4));
        QVERIFY(!f.vim()->openPicker(QStringLiteral("nonsense")));
    }

    void pickerKeysAreNotTakenByTheLayer()
    {
        Fixture f;
        QVERIFY(f.vim()->openPicker(QStringLiteral("commands")));
        auto* picker = f.vim()->picker();
        QTest::keyClicks(picker->filterField(), QStringLiteral("jjdd"));
        QCOMPARE(picker->filter(), QStringLiteral("jjdd")); // typed into the field, not the machine
        QCOMPARE(f.layout(), QStringLiteral("0:40 40:40 80:40"));
        QTest::keyClick(picker->filterField(), Qt::Key_Escape);
    }
};

QTEST_MAIN(TestVimCmdline)
#include "test_vimcmdline.moc"
