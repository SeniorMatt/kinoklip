// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs: keyboard customisation conflicts / cancel / search, project settings with undo,
// preferences storage.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/Preferences.h"
#include "ui/common/Resources.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

class TestDialogs : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_dialogs"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void cleanup() { QSettings().clear(); }

    void keyboardConflictDetection()
    {
        PanelFixture f;
        KeyboardCustomizationDialog dialog(f.actions);
        const QKeySequence undoKey(QStringLiteral("Ctrl+Z"));
        const QString split = QStringLiteral("edit.split");
        const QString undo = QStringLiteral("edit.undo");
        QCOMPARE(dialog.conflictFor(split, undoKey), undo);
        QCOMPARE(dialog.conflictFor(undo, undoKey), QString()); // own key is no conflict

        // Cancel: nothing changes.
        QVERIFY(!dialog.assignShortcut(
            split, undoKey, KeyboardCustomizationDialog::ConflictPolicy::Cancel));
        QCOMPARE(f.actions.shortcuts(undo), QList<QKeySequence> {undoKey});

        // Reassign: the other command loses the key, split's primary key is replaced.
        QVERIFY(dialog.assignShortcut(
            split, undoKey, KeyboardCustomizationDialog::ConflictPolicy::Reassign));
        QCOMPARE(f.actions.shortcuts(split).front(), undoKey);
        QVERIFY(f.actions.shortcuts(undo).isEmpty());
        QCOMPARE(f.actions.action(split)->shortcut(), undoKey);

        // A free key needs no policy at all.
        const QKeySequence free(QStringLiteral("Ctrl+Alt+Shift+F11"));
        QVERIFY(dialog.conflictFor(undo, free).isEmpty());
        QVERIFY(dialog.assignShortcut(undo, free));
        QCOMPARE(f.actions.shortcuts(undo), QList<QKeySequence> {free});

        // Cancel restores everything the dialog changed.
        dialog.reject();
        QCOMPARE(f.actions.shortcuts(undo), QList<QKeySequence> {undoKey});
        QCOMPARE(f.actions.shortcuts(split), f.actions.defaultShortcuts(split));
    }

    void keyboardSecondaryKeysAndClear()
    {
        PanelFixture f;
        KeyboardCustomizationDialog dialog(f.actions);
        const QString ripple = QStringLiteral("edit.ripple_delete");
        const QList<QKeySequence> defaults = f.actions.shortcuts(ripple);
        QCOMPARE(defaults.size(), 2);
        const QKeySequence key(QStringLiteral("Ctrl+Alt+R"));
        QVERIFY(dialog.assignShortcut(ripple, key));
        QCOMPARE(f.actions.shortcuts(ripple), (QList<QKeySequence> {key, defaults[1]}));
        dialog.clearShortcuts(ripple);
        QVERIFY(f.actions.shortcuts(ripple).isEmpty());
        dialog.accept(); // keeps the change
        QVERIFY(f.actions.shortcuts(ripple).isEmpty());
        f.actions.resetAllShortcuts();
    }

    void keyboardSearchAndScreenshot()
    {
        PanelFixture f;
        KeyboardCustomizationDialog dialog(f.actions);
        dialog.setFilterText(QStringLiteral("undo"));
        QVERIFY(dialog.visibleIds().contains(QStringLiteral("edit.undo")));
        QVERIFY(!dialog.visibleIds().contains(QStringLiteral("edit.split")));
        dialog.setFilterText(QStringLiteral("Ctrl+B")); // search by key
        QVERIFY(dialog.visibleIds().contains(QStringLiteral("edit.split")));
        dialog.setFilterText({});
        QCOMPARE(dialog.visibleIds().size(), f.actions.ids().size());
        dialog.assignShortcut(
            QStringLiteral("clip.add_keyframe"), QKeySequence(QStringLiteral("Alt+K")));
        dialog.setCurrentId(QStringLiteral("clip.add_keyframe"));
        saveScreenshot(dialog, QStringLiteral("dialog_keyboard"));
        dialog.reject();
    }

    void recorderCapturesChord()
    {
        ShortcutRecorder recorder;
        QSignalSpy spy(&recorder, &ShortcutRecorder::recorded);
        QTest::keyClick(&recorder, Qt::Key_Control, Qt::ControlModifier); // modifier only: ignored
        QCOMPARE(spy.count(), 0);
        QTest::keyClick(&recorder, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.front().front().value<QKeySequence>(),
            QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    }

    void projectSettingsApplyIsUndoable()
    {
        PanelFixture f;
        ProjectSettingsDialog dialog(&f.project);
        QCOMPARE(dialog.settings(), f.project.settings());
        ProjectSettings wanted = f.project.settings();
        wanted.name = QStringLiteral("Short Film");
        wanted.width = 1280;
        wanted.height = 720;
        wanted.fps = {30000, 1001};
        dialog.setSettings(wanted);
        QCOMPARE(dialog.settings(), wanted);
        // The 720p preset is recognised, so the size fields are locked.
        QVERIFY(!dialog.findChildren<QSpinBox*>().front()->isEnabled());
        saveScreenshot(dialog, QStringLiteral("dialog_project_settings"));

        const int commands = f.project.undoStack()->count();
        dialog.apply();
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QCOMPARE(f.project.timeline()->data().width, 1280);
        QCOMPARE(f.project.timeline()->data().fps, (Rational {30000, 1001}));
        QCOMPARE(f.project.name(), QStringLiteral("Short Film"));
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().width, 1920);

        // Custom sizes unlock the spin boxes.
        ProjectSettings custom = wanted;
        custom.width = 1000;
        custom.height = 500;
        dialog.setSettings(custom);
        QVERIFY(dialog.findChildren<QSpinBox*>().front()->isEnabled());
        QCOMPARE(dialog.settings().width, 1000);
    }

    void preferencesAreStored()
    {
        PanelFixture f;
        PreferencesDialog dialog(nullptr); // standard pages only
        const auto combos = dialog.findChildren<QComboBox*>();
        QComboBox* hw = combos.front();
        hw->setCurrentIndex(hw->findData(QStringLiteral("vaapi")));
        dialog.findChild<QSpinBox*>()->setValue(7);
        QSignalSpy changed(&dialog, &PreferencesDialog::preferencesChanged);
        // Let the hardware probe (worker thread) fill in its label for the screenshot.
        QElapsedTimer probe;
        probe.start();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.findChild<QLabel*>(QStringLiteral("HwInfo"))
                                      ->text()
                                      .startsWith(QStringLiteral("Checking")),
            15000);
        qInfo("hardware probe took %lld ms", probe.elapsed());
        saveScreenshot(dialog, QStringLiteral("dialog_preferences"));
        dialog.apply();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(prefs::hwAccel(), prefs::HwAccel::Vaapi);
        QCOMPARE(prefs::autosaveMinutes(), 7);
    }
};

QTEST_MAIN(TestDialogs)
#include "test_dialogs.moc"
