// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the "Vim keys" tab of Keyboard Customization (spec section 83): the tree by context,
// adding, changing, removing and resetting keys with the conflict and prefix checks, export and
// import, the vimrc rows, and the tab inside the dialog and its button in Preferences.
#include "ui/common/ActionRegistry.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimOptions.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/VimKeysTab.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace kk;

namespace {

struct Fixture
{
    ActionRegistry actions;
    VimKeymap* keymap = nullptr;
    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        keymap = VimKeymap::of(actions);
    }
};

VimKeysTab::Edit edit(const char* context, const char* modes, const char* keys, const char* command)
{
    VimKeysTab::Edit e;
    e.context = QLatin1String(context);
    e.modes = QLatin1String(modes);
    e.keys = QLatin1String(keys);
    e.command = QLatin1String(command);
    return e;
}

bool hasRow(const VimKeysTab& tab, const QString& text)
{
    for (const QString& row : tab.rowTexts()) {
        if (row.contains(text))
            return true;
    }
    return false;
}

} // namespace

class TestVimKeysTab : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimkeystab"));
        QSettings().clear();
        initResources();
    }
    void init() { QSettings().clear(); }

    void treeIsGroupedByContext()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        QTreeWidget* tree = tab.tree();
        QStringList groups;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            groups.append(tree->topLevelItem(i)->text(0));
        QCOMPARE(groups,
            (QStringList {QStringLiteral("Leader"), QStringLiteral("Global"),
                QStringLiteral("Timeline"), QStringLiteral("Viewer"),
                QStringLiteral("Lists and trees"), QStringLiteral("Inspector and forms"),
                QStringLiteral("Keyframes panel"), QStringLiteral("Node graphs"),
                QStringLiteral("Mixer")}));
        QVERIFY(hasRow(tab, QStringLiteral("Lists and trees | j | n v o | vim:row_down")));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | h | n v o | vim:frame_prev")));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | d | n v | vim:op_delete")));
        QVERIFY(hasRow(tab, QStringLiteral("Leader | <leader>si | n | action:edit.insert")));
        QVERIFY(hasRow(tab, QStringLiteral("Global | <C-r> | n v | action:edit.redo")));
        QVERIFY(hasRow(tab, QStringLiteral("Viewer | zf | n v | action:view.viewer_zoom_fit")));
        // Groups (prefix names) are not rows.
        QVERIFY(!hasRow(tab, QStringLiteral("| <leader>s |")));
    }

    void searchFiltersRows()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        const int all = tab.rowTexts().size();
        tab.setFilterText(QStringLiteral("marker"));
        const QStringList rows = tab.rowTexts();
        QVERIFY(rows.size() < all && rows.size() >= 4);
        QVERIFY(hasRow(tab, QStringLiteral("<leader>mm")));
        tab.setFilterText(QStringLiteral("zzzz-nothing"));
        QVERIFY(tab.rowTexts().isEmpty());
        tab.setFilterText({});
        QCOMPARE(tab.rowTexts().size(), all);
    }

    void addAChangeAndSeeItInTheMachineTable()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        QString message;
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "Q", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::Saved);
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | Q | n | action:edit.undo")));
        QCOMPARE(f.keymap->trie(QStringLiteral("timeline"), 'n').match({"Q"}).kind,
            VimMatch::Kind::Exact);
        // Changing a row moves its keys: x now means Q's old command; the old keys are free.
        const VimKeysTab::Edit before = edit("timeline", "n", "Q", "edit.undo");
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "Z", "edit.redo"), &before, false, &message),
            VimKeysTab::Result::Saved);
        QVERIFY(!hasRow(tab, QStringLiteral("Timeline | Q |")));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | Z | n | action:edit.redo")));
        // A leader key with a description.
        VimKeysTab::Edit leader = edit("global", "n", "<leader>zz", "vim:visual_char");
        leader.description = QStringLiteral("my visual");
        QCOMPARE(tab.saveBinding(leader, nullptr, false, &message), VimKeysTab::Result::Saved);
        QVERIFY(hasRow(tab, QStringLiteral("Leader | <leader>zz |")));
        // Stored: a restart sees it.
        VimKeymap again;
        again.reload();
        QCOMPARE(
            again.trie(QStringLiteral("timeline"), 'n').match({"Z"}).kind, VimMatch::Kind::Exact);
        QCOMPARE(
            again.trie(QStringLiteral("timeline"), 'n').match({"<Space>", "z", "z"}).binding->desc,
            QStringLiteral("my visual"));
    }

    void invalidAndConflictingKeysAreRefused()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        QString message;
        QCOMPARE(
            tab.saveBinding(edit("timeline", "n", "a b", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::Invalid);
        QVERIFY(!message.isEmpty());
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::Invalid);
        QCOMPARE(
            tab.saveBinding(edit("timeline", "n", "Q", "no.such.action"), nullptr, false, &message),
            VimKeysTab::Result::Invalid);
        QVERIFY(message.contains(QStringLiteral("no.such.action")));
        QCOMPARE(
            tab.saveBinding(edit("timeline", "n", "Q", "vim:nonsense"), nullptr, false, &message),
            VimKeysTab::Result::Invalid);
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "Q", ""), nullptr, false, &message),
            VimKeysTab::Result::Invalid);
        QVERIFY(f.keymap->userOps().isEmpty());
        // h is bound in the timeline: a conflict, nothing changes until the user says replace.
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "h", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::Conflict);
        QVERIFY(message.contains(QStringLiteral("already bound")));
        QVERIFY(f.keymap->userOps().isEmpty());
        QCOMPARE(f.keymap->trie(QStringLiteral("timeline"), 'n').match({"h"}).binding->run.value(0),
            QStringLiteral("vim:frame_prev"));
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "h", "edit.undo"), nullptr, true, &message),
            VimKeysTab::Result::Saved);
        QCOMPARE(f.keymap->trie(QStringLiteral("timeline"), 'n').match({"h"}).binding->run.value(0),
            QStringLiteral("action:edit.undo"));
        // The same keys in VISUAL only are free: h in VISUAL is still the motion.
        QCOMPARE(f.keymap->trie(QStringLiteral("timeline"), 'v').match({"h"}).binding->run.value(0),
            QStringLiteral("vim:frame_prev"));
    }

    void prefixesAreWarnedAbout()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        QString message;
        // g is the start of gg and gj...: it works, but waits for timeoutlen.
        QCOMPARE(tab.saveBinding(edit("timeline", "n", "g", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::SavedWithWarning);
        QVERIFY(message.contains(QStringLiteral("ms")));
        QVERIFY(tab.issueLines().join(QLatin1Char('\n')).contains(QStringLiteral("Warning")));
        // A long key under a short one: ggx starts with gg.
        QCOMPARE(
            tab.saveBinding(edit("timeline", "n", "ggx", "edit.undo"), nullptr, false, &message),
            VimKeysTab::Result::SavedWithWarning);
        QVERIFY(message.contains(QStringLiteral("starts with")));
    }

    void removeResetAndResetAll()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        // Remove a default.
        QVERIFY(tab.removeBinding(
            QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p")));
        QVERIFY(!hasRow(tab, QStringLiteral("Timeline | p | n |")));
        QCOMPARE(f.keymap->trie(QStringLiteral("timeline"), 'n').match({"p"}).kind,
            VimMatch::Kind::None);
        // Reset brings it back.
        tab.resetBinding(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p"));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | p | n | vim:cmd_paste")));
        // Remove one of your own: gone for good.
        QString message;
        tab.saveBinding(edit("timeline", "n", "Q", "edit.undo"), nullptr, false, &message);
        QVERIFY(tab.removeBinding(
            QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("Q")));
        QVERIFY(!hasRow(tab, QStringLiteral("Timeline | Q |")));
        QVERIFY(f.keymap->userOps().isEmpty());
        // Reset all.
        tab.saveBinding(edit("timeline", "n", "h", "edit.undo"), nullptr, true, &message);
        tab.removeBinding(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("x"));
        tab.resetAll();
        QVERIFY(f.keymap->userOps().isEmpty());
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | x | n | vim:cmd_delete")));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | h | n v o | vim:frame_prev")));
    }

    void exportAndImportThroughFiles()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        QString message;
        tab.saveBinding(edit("timeline", "n", "Q", "edit.undo"), nullptr, false, &message);
        tab.removeBinding(QStringLiteral("timeline"), QStringLiteral("n"), QStringLiteral("p"));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("vim-keys.json"));
        QString error;
        QVERIFY2(tab.exportTo(path, &error), qPrintable(error));
        tab.resetAll();
        QVERIFY(!hasRow(tab, QStringLiteral("Timeline | Q |")));
        QVERIFY2(tab.importFrom(path, &error), qPrintable(error));
        QVERIFY(hasRow(tab, QStringLiteral("Timeline | Q | n | action:edit.undo")));
        QVERIFY(!hasRow(tab, QStringLiteral("Timeline | p | n |")));
        QVERIFY(!tab.importFrom(dir.filePath(QStringLiteral("missing.json")), &error));
        QVERIFY(!error.isEmpty());
    }

    void vimrcRowsAreReadOnlyAndMarked()
    {
        Fixture f;
        QTemporaryDir dir;
        QFile rc(dir.filePath(QStringLiteral("vimrc")));
        QVERIFY(rc.open(QIODevice::WriteOnly));
        rc.write("nmap Q edit.undo\nnmap Y vim:nonsense\n");
        rc.close();
        QVERIFY(f.keymap->sourceVimrc(rc.fileName()));
        VimKeysTab tab(f.actions, *f.keymap);
        QVERIFY(hasRow(tab, QStringLiteral("Global | Q | n | action:edit.undo")));
        // The row says where it comes from; the error of line 2 is in the checks.
        QTreeWidgetItem* row = nullptr;
        for (int g = 0; g < tab.tree()->topLevelItemCount(); ++g) {
            for (int i = 0; i < tab.tree()->topLevelItem(g)->childCount(); ++i) {
                QTreeWidgetItem* item = tab.tree()->topLevelItem(g)->child(i);
                if (item->text(0) == QStringLiteral("Q"))
                    row = item;
            }
        }
        QVERIFY(row);
        QVERIFY(row->text(3).contains(QStringLiteral("vimrc")));
        QVERIFY(tab.issueLines().join(QLatin1Char('\n')).contains(QStringLiteral("vimrc line 2")));
    }

    void checksListInformationAboutTheProfilesKeys()
    {
        Fixture f;
        VimKeysTab tab(f.actions, *f.keymap);
        const QString text = tab.issueLines().join(QLatin1Char('\n'));
        QVERIFY(text.contains(QStringLiteral("Info:")));
        QVERIFY(text.contains(QStringLiteral("INSERT")));
        QVERIFY(!text.contains(QStringLiteral("Error:")));
    }

    void dialogHasTwoTabs()
    {
        Fixture f;
        QSettings().setValue(QStringLiteral("vim/enabled"), true);
        KeyboardCustomizationDialog dialog(f.actions);
        dialog.show();
        QTabWidget* tabs = dialog.tabs();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Shortcuts"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Vim keys"));
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(dialog.vimTab());
        QVERIFY(tabs->widget(1) == dialog.vimTab());
        // The banner on the Shortcuts tab (Vim mode is on) links to the Vim tab.
        auto* banner = dialog.findChild<QLabel*>(QStringLiteral("VimBanner"));
        QVERIFY(banner && banner->isVisible());
        QMetaObject::invokeMethod(banner, "linkActivated", Q_ARG(QString, QStringLiteral("vim")));
        QCOMPARE(tabs->currentIndex(), 1);
        dialog.showVimKeys();
        QCOMPARE(tabs->currentIndex(), 1);
        // The profile's own tree still works on the first tab.
        tabs->setCurrentIndex(0);
        QVERIFY(dialog.findChild<QTreeWidget*>(QStringLiteral("KeyboardCommands")));
        QVERIFY(!dialog.visibleIds().isEmpty());
    }

    void dialogBannerFollowsTheOption()
    {
        Fixture f;
        KeyboardCustomizationDialog off(f.actions);
        off.show();
        QVERIFY(!off.findChild<QLabel*>(QStringLiteral("VimBanner"))->isVisible());
    }

    void preferencesHasAVimKeysButton()
    {
        Fixture f;
        SettingsPageRegistry registry;
        registerStandardSettingsPages(registry, &f.actions);
        QWidget host;
        SettingsPage* page = nullptr;
        for (const SettingsPageInfo& info : registry.pages()) {
            if (info.id == QLatin1String("keyboard"))
                page = info.create(&host);
        }
        QVERIFY(page);
        auto* button = page->findChild<QPushButton*>(QStringLiteral("OpenVimKeys"));
        QVERIFY(button);
        QVERIFY(button->isEnabled());
    }
};

QTEST_MAIN(TestVimKeysTab)
#include "test_vimkeystab.moc"
