// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for ActionRegistry, the standard action catalogue, default keymap and icons.
#include "ui/common/ActionRegistry.h"
#include "ui/common/Icons.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QFile>
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

        QFile file(QStringLiteral(":/keymap_default.json"));
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
            {"edit.split", "Ctrl+B"},
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
                 "panel_inspector", "app"}) {
            QVERIFY2(hasIcon(QLatin1String(name)), name);
        }
        QVERIFY(!hasIcon(QStringLiteral("does_not_exist")));
    }

    void menusAreComplete()
    {
        ActionRegistry reg;
        registerStandardActions(reg);
        QStringList menuIds;
        for (const MenuCategory& m : standardMenus())
            menuIds.append(m.id);
        for (const QString& category : reg.categories())
            QVERIFY2(menuIds.contains(category), qPrintable(category));
        const QStringList file = reg.menuEntries(QStringLiteral("file"));
        QCOMPARE(file.first(), QStringLiteral("file.new"));
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
                QKeySequence(QStringLiteral("Ctrl+B")));
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
