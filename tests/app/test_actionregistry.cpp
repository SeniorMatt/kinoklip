// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for ActionRegistry, the standard action catalogue, default keymap and icons.
#include "ui/common/ActionRegistry.h"
#include "ui/common/Icons.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

class TestActionRegistry : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_actionregistry"));
        initResources();
    }

    void init() { QSettings().clear(); }

    void defaultKeymapCoversEveryStandardAction()
    {
        ActionRegistry reg;
        QVERIFY(reg.loadDefaultKeymap());
        registerStandardActions(reg);

        QFile file(QStringLiteral(":/keymaps/resolve-windows.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject keymap = QJsonDocument::fromJson(file.readAll())
                                       .object()
                                       .value(QLatin1String("shortcuts"))
                                       .toObject();
        for (const QString& id : reg.ids())
            QVERIFY2(keymap.contains(id), qPrintable(id + QStringLiteral(" missing in keymap")));
        for (const QString& id : keymap.keys())
            QVERIFY2(reg.contains(id), qPrintable(id + QStringLiteral(" in keymap but unknown")));
    }

    void plannedShortcuts()
    {
        ActionRegistry reg;
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        const QList<QPair<const char*, const char*>> expected {
            {"transport.play_pause", "Space"},
            {"edit.split", "Ctrl+\\"},
            {"edit.ripple_cut", "Ctrl+Shift+X"},
            {"view.page_deliver", "Shift+8"},
            {"edit.undo", "Ctrl+Z"},
            {"edit.redo", "Ctrl+Shift+Z"},
            {"tool.blade", "B"},
            {"mark.in", "I"},
            {"edit.insert", "F9"},
            {"timeline.zoom_fit", "Shift+Z"},
            {"view.page_edit", "Shift+4"},
            {"clip.add_keyframe", "Alt+K"},
        };
        for (const auto& [id, key] : expected) {
            QCOMPARE(reg.action(QLatin1String(id))->shortcut(),
                QKeySequence::fromString(QLatin1String(key), QKeySequence::PortableText));
        }
        QCOMPARE(reg.shortcuts(QStringLiteral("edit.ripple_delete")).size(), 2);
    }

    void noDuplicateDefaultShortcuts()
    {
        ActionRegistry reg;
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        QHash<QKeySequence, QString> seen;
        for (const QString& id : reg.ids()) {
            for (const QKeySequence& key : reg.shortcuts(id)) {
                QVERIFY2(!seen.contains(key),
                    qPrintable(key.toString() + QStringLiteral(": ") + seen.value(key)
                        + QStringLiteral(" and ") + id));
                seen.insert(key, id);
            }
        }
    }

    void everyActionIconExists()
    {
        ActionRegistry reg;
        registerStandardActions(reg);
        for (QAction* action : reg.actions()) {
            // Actions registered with an icon name must have resolved it.
            if (!action->icon().isNull())
                QVERIFY(!action->icon().pixmap(16, 16).isNull());
        }
        for (const char* name : {"play", "pause", "blade", "select", "trim", "snap", "link",
                 "keyframe_on", "keyframe_off", "zoom_in", "zoom_out", "marker", "loop",
                 "panel_inspector"}) {
            QVERIFY2(hasIcon(QLatin1String(name)), name);
        }
        QVERIFY(!hasIcon(QStringLiteral("does_not_exist")));
        // The application icon: one PNG per size (resources/icons/app).
        for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
            const QImage icon(QStringLiteral(":/icons/app/kinoklip-%1.png").arg(size));
            QCOMPARE(icon.size(), QSize(size, size));
        }
    }

    void menusAreComplete()
    {
        ActionRegistry reg;
        registerStandardActions(reg);
        QStringList menuIds;
        for (const MenuCategory& m : standardMenus())
            menuIds.append(m.id);
        for (const QString& category : reg.categories())
            QVERIFY2(menuIds.contains(category) || reg.isSubmenu(category), qPrintable(category));
        const QStringList file = reg.menuEntries(QStringLiteral("file"));
        QCOMPARE(file.first(), QStringLiteral("file.project_manager")); // spec 38.2
        QVERIFY(file.contains(QStringLiteral("file.new")));
        QVERIFY(file.contains(QLatin1String(ActionRegistry::kSeparator)));
        QVERIFY(file.last() != QLatin1String(ActionRegistry::kSeparator));
    }

    void rebindPersistsAndResets()
    {
        {
            ActionRegistry reg;
            reg.loadDefaultKeymap();
            registerStandardActions(reg);
            QSignalSpy spy(&reg, &ActionRegistry::shortcutsChanged);
            reg.setShortcuts(QStringLiteral("edit.split"), {QKeySequence(Qt::Key_S)});
            QCOMPARE(spy.count(), 1);
            QCOMPARE(reg.actionForShortcut(QKeySequence(Qt::Key_S)), QStringLiteral("edit.split"));
            QVERIFY(reg.action(QStringLiteral("edit.split"))->toolTip().contains(QLatin1Char('S')));
            reg.setShortcuts(QStringLiteral("edit.copy"), {}); // explicitly unbound
        }
        {
            ActionRegistry reg; // a new run reads the overrides back
            reg.loadDefaultKeymap();
            registerStandardActions(reg);
            QCOMPARE(reg.action(QStringLiteral("edit.split"))->shortcut(), QKeySequence(Qt::Key_S));
            QVERIFY(reg.shortcuts(QStringLiteral("edit.copy")).isEmpty());
            reg.resetAllShortcuts();
            QCOMPARE(reg.action(QStringLiteral("edit.split"))->shortcut(),
                QKeySequence(QStringLiteral("Ctrl+\\")));
            QVERIFY(QSettings().childGroups().isEmpty() || QSettings().allKeys().isEmpty());
        }
    }

    void exportImportRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("keys.json"));
        ActionRegistry reg;
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        reg.setShortcuts(QStringLiteral("mark.in"), {QKeySequence(Qt::Key_F2)});
        QVERIFY(reg.exportKeymap(path));
        reg.resetAllShortcuts();
        QCOMPARE(reg.action(QStringLiteral("mark.in"))->shortcut(), QKeySequence(Qt::Key_I));
        QVERIFY(reg.importKeymap(path));
        QCOMPARE(reg.action(QStringLiteral("mark.in"))->shortcut(), QKeySequence(Qt::Key_F2));
        QString error;
        QVERIFY(!reg.importKeymap(dir.filePath(QStringLiteral("missing.json")), &error));
        QVERIFY(!error.isEmpty());
    }

    void customToolTipSurvivesTextChange()
    {
        ActionRegistry reg;
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        QAction* undo = reg.action(QStringLiteral("edit.undo"));
        undo->setText(QStringLiteral("Undo Move Clip"));
        QVERIFY(undo->toolTip().startsWith(QLatin1String("Undo Move Clip")));
        QAction* color = reg.action(QStringLiteral("view.page_color"));
        color->setToolTip(QStringLiteral("custom"));
        color->setText(QStringLiteral("Color!"));
        QCOMPARE(color->toolTip(), QStringLiteral("custom"));
    }

    void builtInProfileIsDefault()
    {
        ActionRegistry reg;
        QTemporaryDir dir;
        reg.setUserProfileDirectory(dir.path());
        QVERIFY(reg.loadDefaultKeymap());
        registerStandardActions(reg);
        QCOMPARE(reg.activeProfile(), QLatin1String(ActionRegistry::kDefaultProfile));
        const auto list = reg.profiles();
        QVERIFY(!list.isEmpty());
        QCOMPARE(list.first().id, QLatin1String(ActionRegistry::kDefaultProfile));
        QVERIFY(list.first().builtIn);
        QVERIFY(!reg.isProfileModified(list.first().id));
        QString error;
        QVERIFY(!reg.renameProfile(list.first().id, QStringLiteral("x"), &error));
        QVERIFY(!reg.deleteProfile(list.first().id, &error));
        QVERIFY(!reg.setActiveProfile(QStringLiteral("nope"), &error));
        QCOMPARE(reg.activeProfile(), QLatin1String(ActionRegistry::kDefaultProfile));
    }

    void userProfilesDuplicateSwitchRenameDelete()
    {
        QTemporaryDir dir;
        const QString split = QStringLiteral("edit.split");
        const QList<QKeySequence> builtInSplit = [&] {
            ActionRegistry reg;
            reg.setUserProfileDirectory(dir.path());
            reg.loadDefaultKeymap();
            registerStandardActions(reg);
            return reg.shortcuts(split);
        }();
        QString userId;
        {
            ActionRegistry reg;
            reg.setUserProfileDirectory(dir.path());
            reg.loadDefaultKeymap();
            registerStandardActions(reg);
            QSignalSpy listSpy(&reg, &ActionRegistry::profilesChanged);
            userId = reg.duplicateProfile(reg.activeProfile(), QStringLiteral("My Keys"));
            QVERIFY(!userId.isEmpty());
            QCOMPARE(listSpy.count(), 1);
            QCOMPARE(reg.profiles().size(), 2);
            QCOMPARE(reg.profile(userId)->name, QStringLiteral("My Keys"));
            QCOMPARE(reg.profile(userId)->basedOn, QLatin1String(ActionRegistry::kDefaultProfile));

            // Per-profile overrides: a change in the user profile leaves the built-in alone.
            QSignalSpy activeSpy(&reg, &ActionRegistry::activeProfileChanged);
            QVERIFY(reg.setActiveProfile(userId));
            QCOMPARE(activeSpy.count(), 1);
            reg.setShortcuts(split, {QKeySequence(Qt::Key_F7)});
            QVERIFY(reg.isProfileModified(userId));
            QVERIFY(reg.setActiveProfile(QLatin1String(ActionRegistry::kDefaultProfile)));
            QCOMPARE(reg.shortcuts(split), builtInSplit);
            QVERIFY(reg.setActiveProfile(userId));
            QCOMPARE(reg.shortcuts(split), QList<QKeySequence> {QKeySequence(Qt::Key_F7)});
            // Reset restores the base of the profile.
            reg.resetShortcuts(split);
            QCOMPARE(reg.shortcuts(split), builtInSplit);
            reg.setShortcuts(split, {QKeySequence(Qt::Key_F7)});
            QVERIFY(reg.renameProfile(userId, QStringLiteral("Renamed")));
            QCOMPARE(reg.profile(userId)->name, QStringLiteral("Renamed"));
        }
        {
            // A new run restores the active user profile and its overrides.
            ActionRegistry reg;
            reg.setUserProfileDirectory(dir.path());
            reg.loadDefaultKeymap();
            registerStandardActions(reg);
            QCOMPARE(reg.activeProfile(), userId);
            QCOMPARE(reg.shortcuts(split), QList<QKeySequence> {QKeySequence(Qt::Key_F7)});
            // Duplicating a user profile keeps its base and its overrides.
            const QString copy = reg.duplicateProfile(userId, QStringLiteral("Renamed"));
            QVERIFY(copy != userId);
            QVERIFY(reg.setActiveProfile(copy));
            QCOMPARE(reg.shortcuts(split), QList<QKeySequence> {QKeySequence(Qt::Key_F7)});
            // Deleting the active profile falls back to its base.
            QVERIFY(reg.deleteProfile(copy));
            QCOMPARE(reg.activeProfile(), QLatin1String(ActionRegistry::kDefaultProfile));
            QCOMPARE(reg.shortcuts(split), builtInSplit);
            QVERIFY(reg.deleteProfile(userId));
            QCOMPARE(reg.profiles().size(), 1);
        }
    }

    void profileImportExport()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("exported.json"));
        ActionRegistry reg;
        reg.setUserProfileDirectory(dir.filePath(QStringLiteral("profiles")));
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        reg.setShortcuts(QStringLiteral("mark.in"), {QKeySequence(Qt::Key_F2)});
        QVERIFY(reg.exportProfile(reg.activeProfile(), file));
        const QString id = reg.importProfile(file, QStringLiteral("Imported"));
        QVERIFY(!id.isEmpty());
        reg.resetAllShortcuts();
        QVERIFY(!reg.isProfileModified(reg.activeProfile()));
        QVERIFY(reg.isProfileModified(id));
        QVERIFY(reg.setActiveProfile(id));
        QCOMPARE(reg.action(QStringLiteral("mark.in"))->shortcut(), QKeySequence(Qt::Key_F2));
        QString error;
        QVERIFY(
            reg.importProfile(dir.filePath(QStringLiteral("missing.json")), {}, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void legacyOverridesMigrate()
    {
        // Overrides saved before keymap profiles existed belong to the default profile.
        QSettings().setValue(QStringLiteral("shortcuts/mark.in"), QStringLiteral("F3"));
        QTemporaryDir dir;
        ActionRegistry reg;
        reg.setUserProfileDirectory(dir.path());
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
        QCOMPARE(reg.action(QStringLiteral("mark.in"))->shortcut(), QKeySequence(Qt::Key_F3));
        QVERIFY(!QSettings().contains(QStringLiteral("shortcuts/mark.in")));
        QVERIFY(QSettings().contains(QStringLiteral("shortcuts/resolve-windows/mark.in")));
    }

    void duplicateRegistrationReturnsExisting()
    {
        ActionRegistry reg;
        QAction* a = reg.add(QStringLiteral("x.test"), QStringLiteral("Test"), QStringLiteral("x"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("registered twice")));
        QCOMPARE(
            reg.add(QStringLiteral("x.test"), QStringLiteral("Again"), QStringLiteral("x")), a);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("unknown action")));
        QVERIFY(!reg.action(QStringLiteral("nope")));
    }
};

QTEST_MAIN(TestActionRegistry)
#include "test_actionregistry.moc"
