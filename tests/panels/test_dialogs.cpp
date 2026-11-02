// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs: keyboard customisation conflicts / cancel / search, project settings with undo,
// preferences storage.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/EditBehaviour.h"
#include "core/Preferences.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>
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
        QVERIFY(!dialog.findChild<QSpinBox*>(QStringLiteral("TimelineWidth"))->isEnabled());
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
        QVERIFY(dialog.findChild<QSpinBox*>(QStringLiteral("TimelineWidth"))->isEnabled());
        QCOMPARE(dialog.settings().width, 1000);
    }

    void projectSettingsSections()
    {
        PanelFixture f;
        prefs::setAutosaveMinutes(2);
        ProjectSettingsDialog dialog(&f.project);
        for (const char* id : {"master", "scaling", "audio", "general", "cache"})
            QVERIFY2(dialog.showSection(QLatin1String(id)), id);
        QVERIFY(!dialog.showSection(QStringLiteral("nope")));
        // Not supported by the engine yet: shown, disabled, with the reason.
        auto* scaling = dialog.findChild<QComboBox*>(QStringLiteral("MismatchedResolution"));
        QVERIFY(!scaling->isEnabled());
        QVERIFY(!scaling->toolTip().isEmpty());
        // General Options: the autosave interval is the shared user preference.
        QSignalSpy changed(&dialog, &ProjectSettingsDialog::preferencesChanged);
        dialog.findChild<QSpinBox*>(QStringLiteral("AutosaveMinutes"))->setValue(9);
        dialog.apply();
        QCOMPARE(prefs::autosaveMinutes(), 9);
        QCOMPARE(changed.count(), 1);
    }

    void preferencesAreStored()
    {
        PanelFixture f;
        PreferencesDialog dialog(nullptr); // standard pages only
        // Resolve's structure: System and User sections.
        for (const char* id : {"memory", "storage", "decode", "audio_io", "ui", "project",
                 "editing", "playback", "keyboard"}) {
            QVERIFY2(dialog.showSection(QLatin1String(id)), id);
        }
        auto combo = [&dialog](const char* name) {
            return dialog.findChild<QComboBox*>(QLatin1String(name));
        };
        auto spin = [&dialog](const char* name) {
            return dialog.findChild<QSpinBox*>(QLatin1String(name));
        };
        auto seconds = [&dialog](const char* name) {
            return dialog.findChild<QDoubleSpinBox*>(QLatin1String(name));
        };
        QComboBox* hw = combo("HwAccel");
        hw->setCurrentIndex(hw->findData(QStringLiteral("vaapi")));
        spin("AutosaveMinutes")->setValue(7);
        spin("StillCacheMiB")->setValue(512);
        combo("GpuProcessing")->setCurrentIndex(1);
        seconds("StillSeconds")->setValue(3.0);
        seconds("TransitionSeconds")->setValue(0.5);
        spin("MultiFrameNudge")->setValue(10);
        combo("ZoomAnchor")->setCurrentIndex(1);
        combo("PreviewQuality")->setCurrentIndex(2);
        seconds("PrerollSeconds")->setValue(3.0);
        QSignalSpy changed(&dialog, &PreferencesDialog::preferencesChanged);
        // Let the hardware probe (worker thread) fill in its table for the screenshot.
        auto* table = dialog.findChild<QTreeWidget*>(QStringLiteral("HwInfo"));
        QElapsedTimer probe;
        probe.start();
        QTRY_VERIFY_WITH_TIMEOUT(table->topLevelItemCount() > 0
                && !table->topLevelItem(0)->text(1).startsWith(QStringLiteral("Checking")),
            15000);
        qInfo("hardware probe took %lld ms", probe.elapsed());
        dialog.showSection(QStringLiteral("decode"));
        saveScreenshot(dialog, QStringLiteral("dialog_preferences"));
        dialog.apply();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(prefs::hwAccel(), prefs::HwAccel::Vaapi);
        QCOMPARE(prefs::autosaveMinutes(), 7);
        QCOMPARE(prefs::stillCacheMiB(), 512);
        QVERIFY(prefs::forceCpuRendering());
        QVERIFY(prefs::zoomAroundMouse());
        QCOMPARE(prefs::previewQuality(), QStringLiteral("half"));
        // The values reach the modules that read them.
        const EditBehaviour edit = EditBehaviour::fromSettings();
        QCOMPARE(edit.stillSeconds, 3.0);
        QCOMPARE(edit.transitionSeconds, 0.5);
        QCOMPARE(edit.multiFrameNudge, 10);
        QCOMPARE(PlaybackBehaviour::fromSettings().prerollMs, 3000);
        applyRuntimePreferences(nullptr); // applies without a playback engine (headless)

        // A reopened dialog shows the stored values.
        PreferencesDialog again(nullptr);
        QCOMPARE(again.findChild<QSpinBox*>(QStringLiteral("StillCacheMiB"))->value(), 512);
        QCOMPARE(again.findChild<QComboBox*>(QStringLiteral("ZoomAnchor"))->currentIndex(), 1);
        QSettings().clear();
    }

    void preferencesKeyboardEntry()
    {
        PanelFixture f;
        SettingsPageRegistry registry;
        registerStandardSettingsPages(registry, &f.actions);
        PreferencesDialog dialog(&registry);
        QSignalSpy opened(
            f.actions.action(QStringLiteral("edit.keyboard_customization")), &QAction::triggered);
        auto* button = dialog.findChild<QPushButton*>(QStringLiteral("OpenKeyboardCustomization"));
        QVERIFY(button && button->isEnabled());
        button->click();
        QCOMPARE(opened.count(), 1);
    }
};

QTEST_MAIN(TestDialogs)
#include "test_dialogs.moc"
