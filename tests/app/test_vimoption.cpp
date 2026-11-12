// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of Vim mode as an option (spec section 83): off by default and independent of the
// keyboard preset, the old Vim preset gone and its settings migrated, the mode line in the
// page bar on every page, INSERT mode (explicit and automatic in text fields), the Esc rule
// chain, the focused-panel frame and the Preferences check box.
#include "app/MainWindow.h"
#include "app/PageBar.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/VimModeLine.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimOptions.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QVBoxLayout>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

QAction* optionAction(ActionRegistry& actions)
{
    return actions.action(QLatin1String(vimopts::kActionId));
}

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    explicit Fixture(bool vim = true, const QString& profile = QStringLiteral("resolve-windows"))
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        QString error;
        QVERIFY2(actions.setActiveProfile(profile, &error), qPrintable(error));
        optionAction(actions)->setChecked(vim);
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
        focusTimeline();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }
    VimInput* vim() const { return window->vimController()->input(); }
    VimModeLine* line() const { return window->vimController()->modeLine(); }
    void focusTimeline()
    {
        timeline()->setFocus();
        QVERIFY(QApplication::focusWidget() == timeline());
    }
    void type(const QString& keys)
    {
        for (const QChar c : keys)
            QTest::keyClick(QApplication::focusWidget(), c.toLatin1());
    }
    void key(Qt::Key k, Qt::KeyboardModifiers mods = {})
    {
        QTest::keyClick(QApplication::focusWidget(), k, mods);
    }
};

void writeText(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

class TestVimOption : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimoption"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // ---- The option ----

    void offByDefaultAndIndependentOfThePreset()
    {
        QVERIFY(!vimopts::enabled());
        {
            ActionRegistry actions;
            actions.loadDefaultKeymap();
            registerStandardActions(actions);
            QAction* option = optionAction(actions);
            QVERIFY(option->isCheckable());
            QVERIFY(!option->isChecked());
            QVERIFY(option->shortcuts().isEmpty()); // no default key
            option->setChecked(true);
            QVERIFY(QSettings().value(QStringLiteral("vim/enabled")).toBool());
            QCOMPARE(actions.activeProfile(), QStringLiteral("resolve-windows"));
            // Switching the preset does not touch the option, and the other way round.
            QVERIFY(actions.setActiveProfile(QStringLiteral("kdenlive")));
            QVERIFY(option->isChecked());
            option->setChecked(false);
            QCOMPARE(actions.activeProfile(), QStringLiteral("kdenlive"));
            QVERIFY(!QSettings().value(QStringLiteral("vim/enabled")).toBool());
        }
    }

    void thePresetIsGone()
    {
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        QVERIFY(!actions.profile(QStringLiteral("vim")));
        for (const auto& p : actions.profiles())
            QVERIFY(p.id != QLatin1String("vim"));
        QVERIFY(!QFile::exists(QStringLiteral(":/keymaps/vim.json")));
        QVERIFY(!actions.setActiveProfile(QStringLiteral("vim")));
    }

    void theActionIsInTheKinoklipMenu()
    {
        ActionRegistry actions;
        registerStandardActions(actions);
        QVERIFY(actions.contains(QLatin1String(vimopts::kActionId)));
        QCOMPARE(actions.category(QLatin1String(vimopts::kActionId)), QStringLiteral("app"));
    }

    // ---- Migration ----

    void oldVimPresetBecomesTheOption()
    {
        QSettings s;
        s.setValue(QStringLiteral("keymap/profile"), QStringLiteral("vim"));
        s.setValue(QStringLiteral("shortcuts/vim/edit.redo"), QStringLiteral("Ctrl+R"));
        s.setValue(QStringLiteral("shortcuts/resolve-windows/edit.undo"), QStringLiteral("Ctrl+Y"));
        s.setValue(QStringLiteral("shortcuts/vim/edit.undo"), QStringLiteral("U"));
        QVERIFY(vimopts::migrateLegacyVimProfile({}));
        QCOMPARE(s.value(QStringLiteral("keymap/profile")).toString(),
            QStringLiteral("resolve-windows"));
        QVERIFY(s.value(QStringLiteral("vim/enabled")).toBool());
        QVERIFY(!s.contains(QStringLiteral("shortcuts/vim/edit.redo")));
        QCOMPARE(s.value(QStringLiteral("shortcuts/resolve-windows/edit.redo")).toString(),
            QStringLiteral("Ctrl+R"));
        // A key already changed under Resolve keeps its value.
        QCOMPARE(s.value(QStringLiteral("shortcuts/resolve-windows/edit.undo")).toString(),
            QStringLiteral("Ctrl+Y"));
        QVERIFY(s.value(QStringLiteral("vim/migrationNotice")).toBool());
        // Again: nothing left to do.
        QVERIFY(!vimopts::migrateLegacyVimProfile({}));
        QVERIFY(s.value(QStringLiteral("vim/enabled")).toBool());
    }

    void nothingOldMeansNothingChanges()
    {
        QSettings().setValue(QStringLiteral("keymap/profile"), QStringLiteral("kdenlive"));
        QVERIFY(!vimopts::migrateLegacyVimProfile({}));
        QCOMPARE(QSettings().value(QStringLiteral("keymap/profile")).toString(),
            QStringLiteral("kdenlive"));
        QVERIFY(!QSettings().contains(QStringLiteral("vim/enabled")));
    }

    void userProfilesBasedOnVimAreRebased()
    {
        QTemporaryDir dir;
        writeText(dir.filePath(QStringLiteral("mine.json")),
            R"({"formatVersion":1,"name":"Mine","basedOn":"vim","shortcuts":{"edit.undo":["Ctrl+U"]}})");
        writeText(dir.filePath(QStringLiteral("other.json")),
            R"({"formatVersion":1,"name":"Other","basedOn":"kdenlive","shortcuts":{}})");
        QSettings().setValue(QStringLiteral("keymap/profile"), QStringLiteral("user-mine"));
        QVERIFY(vimopts::migrateLegacyVimProfile(dir.path()));
        QFile f(dir.filePath(QStringLiteral("mine.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(
            root.value(QStringLiteral("basedOn")).toString(), QStringLiteral("resolve-windows"));
        QCOMPARE(root.value(QStringLiteral("shortcuts"))
                     .toObject()
                     .value(QStringLiteral("edit.undo"))
                     .toArray()
                     .at(0)
                     .toString(),
            QStringLiteral("Ctrl+U")); // the user's keys stay
        QVERIFY(QSettings().value(QStringLiteral("vim/enabled")).toBool());
        QCOMPARE(QSettings().value(QStringLiteral("keymap/profile")).toString(),
            QStringLiteral("user-mine"));
        QFile g(dir.filePath(QStringLiteral("other.json")));
        QVERIFY(g.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(g.readAll())
                     .object()
                     .value(QStringLiteral("basedOn"))
                     .toString(),
            QStringLiteral("kdenlive"));
        // The registry loads it on the default base.
        ActionRegistry actions;
        actions.setUserProfileDirectory(dir.path());
        actions.loadDefaultKeymap();
        QCOMPARE(actions.activeProfile(), QStringLiteral("user-mine"));
        QCOMPARE(actions.profile(QStringLiteral("user-mine"))->basedOn,
            QStringLiteral("resolve-windows"));
    }

    void anInactiveUserProfileDoesNotTurnVimOn()
    {
        QTemporaryDir dir;
        writeText(dir.filePath(QStringLiteral("mine.json")),
            R"({"formatVersion":1,"name":"Mine","basedOn":"vim","shortcuts":{}})");
        QSettings().setValue(QStringLiteral("keymap/profile"), QStringLiteral("premiere-pro"));
        QVERIFY(vimopts::migrateLegacyVimProfile(dir.path()));
        QVERIFY(!QSettings().contains(QStringLiteral("vim/enabled")));
    }

    void startUpMigratesBeforeTheKeymapLoads()
    {
        QSettings s;
        s.setValue(QStringLiteral("keymap/profile"), QStringLiteral("vim"));
        s.setValue(QStringLiteral("shortcuts/vim/edit.redo"), QStringLiteral("Ctrl+R"));
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        QCOMPARE(actions.activeProfile(), QStringLiteral("resolve-windows"));
        QVERIFY(optionAction(actions)->isChecked());
        QCOMPARE(actions.shortcuts(QStringLiteral("edit.redo")).value(0),
            QKeySequence(QStringLiteral("Ctrl+R")));
        // Resolve's own plain keys are back (they were moved to Alt under the old preset).
        QVERIFY(actions.shortcuts(QStringLiteral("mark.in"))
                .contains(QKeySequence(QStringLiteral("I"))));
    }

    void theWindowShowsTheMigrationNoticeOnce()
    {
        QSettings().setValue(QStringLiteral("keymap/profile"), QStringLiteral("vim"));
        {
            Fixture f; // Fixture re-sets the option, but the registry migrated first
            QVERIFY(f.line()->messageText().contains(QStringLiteral("Preferences")));
        }
        QVERIFY(!QSettings().contains(QStringLiteral("vim/migrationNotice")));
        Fixture again;
        QVERIFY(again.line()->messageText().isEmpty());
    }

    // ---- The mode line ----

    void modeLineIsHiddenWhenOff()
    {
        Fixture f(false);
        QVERIFY(!f.line()->isVisible());
        QVERIFY(!f.vim()->active());
        f.type(QStringLiteral("dd")); // plain keys are the preset's
        QVERIFY(f.vim()->machine().pending().isEmpty());
    }

    void modeLineIsInThePageBarOnEveryPage()
    {
        Fixture f;
        auto* bar = f.window->findChild<PageBar*>();
        QVERIFY(bar);
        QVERIFY(f.line()->isVisible());
        QVERIFY(f.line()->parentWidget() == bar);
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        for (const char* page : {"media", "cut", "edit", "color", "audio", "deliver"}) {
            if (!f.window->setCurrentPage(QLatin1String(page)))
                continue; // a page this build has not got
            QVERIFY2(f.line()->isVisible(), page);
            QVERIFY2(bar->isVisible(), page);
            QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        }
    }

    void modeLineShowsPendingKeysAndThePanel()
    {
        Fixture f;
        f.focusTimeline();
        f.type(QStringLiteral("3d"));
        QCOMPARE(f.line()->pendingText(), QStringLiteral("3d"));
        QVERIFY(f.line()->panelText().startsWith(
            QStringLiteral("timeline"))); // the panel is named after the timeline
        f.key(Qt::Key_Escape);
        QVERIFY(f.line()->pendingText().isEmpty());
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
    }

    void modeLineShowsRecording()
    {
        Fixture f;
        f.focusTimeline();
        QVERIFY(f.line()->recordingText().isEmpty());
        f.type(QStringLiteral("qq"));
        QCOMPARE(f.line()->recordingText(), QStringLiteral("recording @q"));
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        f.type(QStringLiteral("l"));
        f.type(QStringLiteral("q"));
        QVERIFY(f.line()->recordingText().isEmpty());
    }

    void commandLineReplacesTheModeLine()
    {
        Fixture f;
        f.type(QStringLiteral(":"));
        auto* cmd = f.window->vimController()->commandLine();
        QVERIFY(cmd->isVisible());
        QCOMPARE(f.line()->modeText(), QStringLiteral("COMMAND"));
        QVERIFY(cmd->parentWidget() == f.window->findChild<PageBar*>());
        QVERIFY(cmd->geometry().left() <= f.line()->geometry().left()
            && cmd->geometry().right() >= f.line()->geometry().right()); // laid over the bar
        QTest::keyClicks(cmd, QStringLiteral("nosuchcommand"));
        QTest::keyClick(cmd, Qt::Key_Return);
        QVERIFY(!cmd->isVisible());
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        QCOMPARE(f.line()->messageText(), QStringLiteral("Not a command: nosuchcommand"));
        QVERIFY(QApplication::focusWidget() == f.timeline()); // the focus comes back
    }

    void theOldToolBarLabelIsGone()
    {
        Fixture f;
        QVERIFY(!f.window->findChild<QWidget*>(QStringLiteral("VimModeIndicator")));
    }

    // ---- INSERT mode ----

    void iEntersInsertAndTheShortcutsComeBack()
    {
        Fixture f;
        int markIn = 0;
        connect(f.actions.action(QStringLiteral("mark.in")), &QAction::triggered, this,
            [&] { ++markIn; });
        f.type(QStringLiteral("i"));
        QCOMPARE(markIn, 0); // NORMAL: the key is the layer's (it entered INSERT)
        QCOMPARE(f.line()->modeText(), QStringLiteral("INSERT"));
        QVERIFY(f.vim()->machine().mode() == VimMachine::Mode::Insert);
        // INSERT: the preset's own key (Resolve: I = Mark In) works.
        f.type(QStringLiteral("i"));
        QCOMPARE(markIn, 1);
        // The layer's keys do nothing in INSERT.
        f.project.timeline()->setPlayhead(50);
        const int undo = f.project.undoStack()->index();
        f.type(QStringLiteral("dd"));
        QCOMPARE(f.project.undoStack()->index(), undo);
        // Esc goes back; Space plays/pauses only in INSERT is checked by the action firing.
        f.key(Qt::Key_Escape);
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        f.type(QStringLiteral("i"));
        QCOMPARE(f.line()->modeText(), QStringLiteral("INSERT"));
        QCOMPARE(markIn, 1);
        f.key(Qt::Key_BracketLeft, Qt::ControlModifier); // Ctrl+[ is Esc
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        f.type(QStringLiteral("a"));
        QCOMPARE(f.line()->modeText(), QStringLiteral("INSERT"));
    }

    void insertDoesNotSurviveTurningTheOptionOff()
    {
        Fixture f;
        f.type(QStringLiteral("i"));
        optionAction(f.actions)->setChecked(false);
        optionAction(f.actions)->setChecked(true);
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
    }

    void textFieldsAreInsertAutomatically()
    {
        Fixture f;
        auto* edit = new QLineEdit(f.window.get());
        edit->setGeometry(10, 10, 200, 24);
        edit->show();
        edit->setFocus();
        QVERIFY(QApplication::focusWidget() == edit);
        QCOMPARE(f.line()->modeText(), QStringLiteral("INSERT"));
        QVERIFY(
            f.vim()->machine().mode() == VimMachine::Mode::Normal); // the machine did not change
        f.focusTimeline();
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        edit->deleteLater();
    }

    // ---- The Esc rule chain ----

    void escDropsPendingKeysFirst()
    {
        Fixture f;
        f.type(QStringLiteral("v"));
        QCOMPARE(f.line()->modeText(), QStringLiteral("VISUAL"));
        f.type(QStringLiteral("3g"));
        QCOMPARE(f.line()->pendingText(), QStringLiteral("3g"));
        f.key(Qt::Key_Escape); // rule 1: pending only
        QVERIFY(f.line()->pendingText().isEmpty());
        QCOMPARE(f.line()->modeText(), QStringLiteral("VISUAL"));
        f.key(Qt::Key_Escape); // rule 3: VISUAL leaves, selection cleared
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        QVERIFY(f.selection.clips().isEmpty());
    }

    void escClosesTheCommandLine()
    {
        Fixture f;
        f.type(QStringLiteral(":"));
        auto* cmd = f.window->vimController()->commandLine();
        QTest::keyClicks(cmd, QStringLiteral("abc"));
        QTest::keyClick(cmd, Qt::Key_Escape);
        QVERIFY(!cmd->isVisible());
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        QVERIFY(QApplication::focusWidget() == f.timeline());
        f.type(QStringLiteral(":"));
        QVERIFY(cmd->text().isEmpty()); // discarded
        QTest::keyClick(cmd, Qt::Key_BracketLeft, Qt::ControlModifier);
        QVERIFY(!cmd->isVisible());
    }

    void escInATextFieldKeepsTheTextAndLeavesTheField()
    {
        Fixture f;
        auto* panel = new PanelFrame(QStringLiteral("Side"), f.window.get());
        auto* edit = new QLineEdit;
        panel->setContentWidget(edit);
        panel->setGeometry(10, 10, 240, 100);
        panel->show();
        edit->setFocus();
        QVERIFY(QApplication::focusWidget() == edit);
        QTest::keyClicks(edit, QStringLiteral("keep me"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_VERIFY(QApplication::focusWidget() != edit); // focus left the field ...
        QCOMPARE(edit->text(), QStringLiteral("keep me")); // ... the text stays
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        QVERIFY(panel->isAncestorOf(QApplication::focusWidget())
            || QApplication::focusWidget() == panel);
        delete panel;
    }

    void escInAControlGoesBackToThePanel()
    {
        Fixture f;
        auto* panel = new PanelFrame(QStringLiteral("Side"), f.window.get());
        auto* combo = new QComboBox;
        combo->addItems({QStringLiteral("a"), QStringLiteral("b")});
        panel->setContentWidget(combo);
        panel->setGeometry(10, 10, 240, 100);
        panel->show();
        combo->setFocus();
        QVERIFY(QApplication::focusWidget() == combo);
        QTest::keyClick(combo, Qt::Key_Escape);
        QTRY_VERIFY(QApplication::focusWidget() == panel);
        // In the panel itself Esc does nothing more.
        QTest::keyClick(panel, Qt::Key_Escape);
        QTest::qWait(10);
        QVERIFY(QApplication::focusWidget() == panel);
        delete panel;
    }

    void escWithNothingPendingIsNotTaken()
    {
        Fixture f;
        // The timeline widget gets the key as always (a drag is cancelled by it).
        struct Spy : QObject
        {
            int escapes = 0;
            bool eventFilter(QObject*, QEvent* e) override
            {
                if (e->type() == QEvent::KeyPress
                    && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape)
                    ++escapes;
                return false;
            }
        } spy;
        f.timeline()->installEventFilter(&spy);
        f.key(Qt::Key_Escape);
        QCOMPARE(spy.escapes, 1);
        QVERIFY(QApplication::focusWidget() == f.timeline());
        f.type(QStringLiteral("3"));
        f.key(Qt::Key_Escape); // taken: it drops the count
        QCOMPARE(spy.escapes, 1);
        f.timeline()->removeEventFilter(&spy);
    }

    void ctrlCIsEscOnlyWhenSomethingIsPending()
    {
        Fixture f;
        int copies = 0;
        connect(f.actions.action(QStringLiteral("edit.copy")), &QAction::triggered, this,
            [&] { ++copies; });
        f.key(Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(copies, 1); // nothing pending: still Copy
        f.type(QStringLiteral("2d"));
        f.key(Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(copies, 1);
        QVERIFY(f.line()->pendingText().isEmpty());
        f.type(QStringLiteral("v"));
        f.key(Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(f.line()->modeText(), QStringLiteral("NORMAL"));
        QCOMPARE(copies, 1);
    }

    // ---- The focused-panel frame ----

    void thePanelWithTheFocusGetsAFrame()
    {
        Fixture f;
        PanelFrame* panel = nullptr;
        for (QWidget* w = f.timeline(); w && !panel; w = w->parentWidget())
            panel = qobject_cast<PanelFrame*>(w);
        QVERIFY(panel);
        QVERIFY(panel->vimFocusFrame());
        optionAction(f.actions)->setChecked(false);
        QVERIFY(!panel->vimFocusFrame());
        optionAction(f.actions)->setChecked(true);
        QVERIFY(panel->vimFocusFrame());
        // Moving the focus moves the frame.
        auto* other = new PanelFrame(QStringLiteral("Other"), f.window.get());
        auto* edit = new QLineEdit;
        other->setContentWidget(edit);
        other->setGeometry(10, 10, 240, 100);
        other->show();
        edit->setFocus();
        QVERIFY(other->vimFocusFrame());
        QVERIFY(!panel->vimFocusFrame());
        delete other;
    }

    // ---- Preferences > Keyboard ----

    void preferencesHasTheCheckBox()
    {
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        SettingsPageRegistry registry;
        registerStandardSettingsPages(registry, &actions);
        SettingsPage* page = nullptr;
        QWidget host;
        for (const SettingsPageInfo& info : registry.pages()) {
            if (info.id == QLatin1String("keyboard"))
                page = info.create(&host);
        }
        QVERIFY(page);
        auto* box = page->findChild<QCheckBox*>(QStringLiteral("VimModeCheck"));
        QVERIFY(box);
        QVERIFY(!box->isChecked());
        box->setChecked(true);
        QVERIFY(!optionAction(actions)->isChecked()); // stored on OK, like every page
        page->apply();
        QVERIFY(optionAction(actions)->isChecked());
        QVERIFY(vimopts::enabled());
        // The menu item changes the open page's box.
        optionAction(actions)->setChecked(false);
        QVERIFY(!box->isChecked());
    }
};

QTEST_MAIN(TestVimOption)
#include "test_vimoption.moc"
