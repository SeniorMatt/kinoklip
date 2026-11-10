// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard presets of other editors (spec 23.14 - 23.18, 38.32): every built-in preset loads
// and has no key twice in one scope, S splits with Vegas Pro, the preset chooser and Cancel,
// Premiere Pro .kys and Final Cut Pro .commandset imports, and the first start's choice.
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/ForeignKeymaps.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/projects/KeymapChoiceBanner.h"
#include "ui/projects/ProjectManagerWindow.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

const QStringList kPresets {QStringLiteral("resolve-windows"),
    QStringLiteral("avid-media-composer"), QStringLiteral("final-cut-pro"),
    QStringLiteral("kdenlive"), QStringLiteral("premiere-pro"), QStringLiteral("shotcut"),
    QStringLiteral("vegas-pro"), QStringLiteral("vim")};

QKeySequence key(const char* text)
{
    return QKeySequence::fromString(QLatin1String(text), QKeySequence::PortableText);
}

bool hasKey(const ActionRegistry& reg, const char* id, const char* text)
{
    return reg.shortcuts(QLatin1String(id)).contains(key(text));
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

/// A registry with the standard commands and a private user profile folder.
struct Registry
{
    QTemporaryDir dir;
    ActionRegistry reg;
    Registry()
    {
        reg.setUserProfileDirectory(dir.path());
        reg.loadDefaultKeymap();
        registerStandardActions(reg);
    }
};

/// The main window with three clips on V1: [0,40) [40,80) [80,120).
struct Window
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QTemporaryDir dir;

    Window()
    {
        actions.setUserProfileDirectory(dir.path());
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
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
        timeline()->scrollToFrame(0);
        QCoreApplication::processEvents();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }

    QString layout() const
    {
        QStringList parts;
        for (const ClipData& c : project.timeline()->data().track(kV1)->clips)
            parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
        return parts.join(QLatin1Char(' '));
    }

    void clickClip(double frame)
    {
        const TimelineLayout& L = timeline()->layout();
        const QPoint p(int(std::lround(L.frameToX(frame))), L.rowRect(L.rowOf(kV1)).center().y());
        QTest::mouseClick(timeline()->viewport(), Qt::LeftButton, {}, p);
    }

    static void press(Qt::Key k, Qt::KeyboardModifiers mods = {})
    {
        QWidget* target = QApplication::focusWidget();
        QVERIFY2(target, "no focus widget");
        QTest::keyClick(target, k, mods);
    }
};

// Sample files written for these tests in the general shape of the formats (not vendor files).
const QByteArray kKys
    = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<PremiereData Version=\"3\">\n"
      "  <KeyboardShortcuts>\n"
      "    <Context Name=\"Global\">\n"
      "      "
      "<shortcut><commandname>cmd.edit.undo</commandname><virtualkey>90</virtualkey><modifiers>1</"
      "modifiers></shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.timeline.razorAtCTI</commandname><virtualkey>0x53</"
      "virtualkey><modifiers>0</modifiers></shortcut>\n"
      "      <shortcut commandname=\"cmd.marker.addMarker\" virtualkey=\"77\" modifiers=\"2\"/>\n"
      "      "
      "<shortcut><commandname>cmd.transport.playStop</commandname><key>Space</key></shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.sequence.zoomIn</commandname><virtualkey>187</virtualkey></"
      "shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.edit.lift</commandname><virtualkey>0x60</virtualkey></shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.edit.extract</commandname><shortcut>Ctrl+Shift+Del</shortcut></"
      "shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.app.brewCoffee</commandname><virtualkey>81</virtualkey></"
      "shortcut>\n"
      "      "
      "<shortcut><commandname>cmd.edit.redo</commandname><virtualkey>999</virtualkey></shortcut>\n"
      "      <shortcut><virtualkey>70</virtualkey></shortcut>\n"
      "      <garbage><nothing/></garbage>\n"
      "    </Context>\n"
      "  </KeyboardShortcuts>\n"
      "</PremiereData>\n";

const QByteArray kCommandSet
    = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "  <key>Version</key><integer>2</integer>\n"
      "  <key>Blade</key>\n"
      "  "
      "<array><dict><key>characterString</key><string>b</string><key>modifiers</"
      "key><integer>1179648</integer></dict></array>\n"
      "  <key>SetInPoint</key>\n"
      "  <array><dict><key>characterString</key><string>e</string></dict></array>\n"
      "  <key>NextMarker</key>\n"
      "  "
      "<dict><key>characterString</key><string>&#xF703;</string><key>modifiers</"
      "key><integer>262144</integer></dict>\n"
      "  <key>ToggleSnapping</key>\n"
      "  "
      "<array><dict><key>keyEquivalent</key><string>n</string><key>modifierMask</"
      "key><string>option</string></dict></array>\n"
      "  <key>Delete</key>\n"
      "  <array><dict><key>keyCode</key><integer>51</integer></dict></array>\n"
      "  <key>MakeTea</key>\n"
      "  <array><dict><key>characterString</key><string>t</string></dict></array>\n"
      "  <key>Metadata</key>\n"
      "  <dict><key>Author</key><string>me</string><key>Fancy</key><true/></dict>\n"
      "</dict>\n"
      "</plist>\n";

} // namespace

class TestKeyPresets : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_keypresets"));
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    /// Spec 23.14: the seven built-in presets, the default first; each covers every command
    /// and binds no key twice in one scope (the Keyboard Customization conflict rules).
    void everyPresetLoadsWithoutConflicts()
    {
        Registry r;
        QStringList builtIns;
        for (const auto& p : r.reg.profiles()) {
            if (p.builtIn)
                builtIns.append(p.id);
        }
        QCOMPARE(builtIns.first(), QLatin1String(ActionRegistry::kDefaultProfile));
        QCOMPARE(QSet<QString>(builtIns.begin(), builtIns.end()),
            QSet<QString>(kPresets.begin(), kPresets.end()));
        for (const QString& id : kPresets) {
            QFile file(QStringLiteral(":/keymaps/%1.json").arg(id));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject keymap = QJsonDocument::fromJson(file.readAll())
                                           .object()
                                           .value(QLatin1String("shortcuts"))
                                           .toObject();
            for (const QString& action : r.reg.ids())
                QVERIFY2(keymap.contains(action), qPrintable(id + QLatin1String(": ") + action));
            for (const QString& action : keymap.keys())
                QVERIFY2(r.reg.contains(action), qPrintable(id + QLatin1String(": ") + action));
            for (auto it = keymap.constBegin(); it != keymap.constEnd(); ++it) {
                for (const QJsonValue& v : it.value().toArray())
                    QVERIFY2(!key(qPrintable(v.toString())).isEmpty(),
                        qPrintable(id + QLatin1String(": bad key ") + v.toString()));
            }

            QString error;
            QVERIFY2(r.reg.setActiveProfile(id, &error), qPrintable(error));
            QHash<QKeySequence, QStringList> seen;
            for (const QString& action : r.reg.ids()) {
                for (const QKeySequence& k : r.reg.shortcuts(action)) {
                    for (const QString& other : seen.value(k)) {
                        QVERIFY2(!ActionRegistry::scopesConflict(
                                     r.reg.shortcutScope(action), r.reg.shortcutScope(other)),
                            qPrintable(id + QLatin1String(": ") + k.toString()
                                + QLatin1String(" on ") + other + QLatin1String(" and ") + action));
                    }
                    seen[k].append(action);
                }
                // The dialog's conflict check agrees.
                for (const QKeySequence& k : r.reg.shortcuts(action))
                    QCOMPARE(r.reg.actionForShortcut(k, action), QString());
            }
            QVERIFY(!r.reg.isProfileModified(id));
        }
    }

    /// Spec 23.15: the keys the presets are known for.
    void presetsHaveTheirEditorsKeys()
    {
        Registry r;
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("vegas-pro")));
        QVERIFY(hasKey(r.reg, "edit.split", "S"));
        QVERIFY(!hasKey(r.reg, "edit.split", "Ctrl+\\"));
        QVERIFY(hasKey(r.reg, "timeline.snapping", "F8"));
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("premiere-pro")));
        QVERIFY(hasKey(r.reg, "edit.split", "Ctrl+K"));
        QVERIFY(hasKey(r.reg, "tool.blade", "C"));
        QVERIFY(hasKey(r.reg, "edit.insert", ","));
        QVERIFY(hasKey(r.reg, "timeline.snapping", "S"));
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("final-cut-pro")));
        QVERIFY(hasKey(r.reg, "edit.split", "Ctrl+B")); // Command+B
        QVERIFY(hasKey(r.reg, "edit.insert", "W"));
        QVERIFY(hasKey(r.reg, "edit.ripple_delete", "Backspace"));
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("avid-media-composer")));
        QVERIFY(hasKey(r.reg, "edit.ripple_delete", "X")); // Extract
        QVERIFY(hasKey(r.reg, "mark.in", "E"));
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("kdenlive")));
        QVERIFY(hasKey(r.reg, "edit.split", "Shift+R"));
        QVERIFY(r.reg.setActiveProfile(QStringLiteral("shotcut")));
        QVERIFY(hasKey(r.reg, "edit.split", "S"));
        QVERIFY(hasKey(r.reg, "edit.ripple_delete", "X"));
        // The default stays Resolve's.
        QVERIFY(r.reg.setActiveProfile(QLatin1String(ActionRegistry::kDefaultProfile)));
        QVERIFY(hasKey(r.reg, "edit.split", "Ctrl+\\"));
        QVERIFY(r.reg.shortcuts(QStringLiteral("edit.split")).indexOf(key("S")) < 0);
    }

    /// Spec 23.15: with the Vegas Pro preset, a real S key press splits at the playhead.
    void sSplitsWithVegasPro()
    {
        Window w;
        w.clickClip(30);
        w.selection.clear();
        w.project.timeline()->setPlayhead(50);
        w.press(Qt::Key_S); // the Resolve keymap: S does nothing on the timeline
        QCOMPARE(w.layout(), QStringLiteral("0:40 40:40 80:40"));
        QVERIFY(w.actions.setActiveProfile(QStringLiteral("vegas-pro")));
        w.press(Qt::Key_S);
        QCOMPARE(w.layout(), QStringLiteral("0:40 40:10 50:30 80:40"));
    }

    /// Spec 23.16: choosing a preset in Keyboard Customization activates it, the user's own
    /// changes on top stay possible, and Cancel restores the profile and keys of before.
    void choosingAPresetAndCancelRestores()
    {
        Registry r;
        r.reg.setShortcuts(QStringLiteral("mark.in"), {QKeySequence(Qt::Key_F7)});
        const auto before = r.reg.shortcuts(QStringLiteral("edit.split"));
        {
            KeyboardCustomizationDialog dialog(r.reg);
            QComboBox* combo = dialog.profileCombo();
            QCOMPARE(combo->itemData(0).toString(), QLatin1String(ActionRegistry::kDefaultProfile));
            QVERIFY(combo->itemText(0).contains(QLatin1String("Resolve")));
            QVERIFY(combo->count() >= kPresets.size());
            const int vegas = combo->findData(QStringLiteral("vegas-pro"));
            QVERIFY(vegas > 0);
            combo->setCurrentIndex(vegas);
            emit combo->activated(vegas);
            QCOMPARE(r.reg.activeProfile(), QStringLiteral("vegas-pro"));
            QVERIFY(hasKey(r.reg, "edit.split", "S"));
            // A change of one's own on top of the preset.
            QVERIFY(dialog.assignShortcut(QStringLiteral("edit.split"), QKeySequence(Qt::Key_F6),
                KeyboardCustomizationDialog::ConflictPolicy::Reassign));
            QVERIFY(r.reg.isProfileModified(QStringLiteral("vegas-pro")));
            dialog.reject();
        }
        QCOMPARE(r.reg.activeProfile(), QLatin1String(ActionRegistry::kDefaultProfile));
        QCOMPARE(r.reg.shortcuts(QStringLiteral("edit.split")), before);
        QCOMPARE(r.reg.shortcuts(QStringLiteral("mark.in")),
            QList<QKeySequence> {QKeySequence(Qt::Key_F7)});
        QCOMPARE(QSettings().value(QStringLiteral("keymap/profile")).toString(),
            QLatin1String(ActionRegistry::kDefaultProfile));
        // OK keeps the choice.
        {
            KeyboardCustomizationDialog dialog(r.reg);
            const int kd = dialog.profileCombo()->findData(QStringLiteral("kdenlive"));
            emit dialog.profileCombo()->activated(kd);
            dialog.accept();
        }
        QCOMPARE(r.reg.activeProfile(), QStringLiteral("kdenlive"));
    }

    /// Spec 23.17: a Premiere Pro .kys file: recognised commands get their keys on top of the
    /// Premiere preset, a key they take leaves the preset's other command, unknown entries
    /// are skipped and counted.
    void premiereKysImport()
    {
        Registry r;
        const QString path = r.dir.filePath(QStringLiteral("My Premiere Keys.kys"));
        writeFile(path, kKys);
        foreignkeys::Result parsed;
        QVERIFY(foreignkeys::readFile(path, parsed));
        QCOMPARE(parsed.format, QStringLiteral("Premiere Pro"));
        QCOMPARE(parsed.bindings.size(), 7);
        QCOMPARE(parsed.skipped, 3); // brewCoffee, key 999, no command name

        KeyboardCustomizationDialog dialog(r.reg);
        QString error;
        const QString id = dialog.importFile(path, &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QCOMPARE(r.reg.activeProfile(), id);
        QCOMPARE(r.reg.profile(id)->basedOn, QStringLiteral("premiere-pro"));
        QCOMPARE(r.reg.profile(id)->name, QStringLiteral("My Premiere Keys"));
        QVERIFY(dialog.statusText().contains(QLatin1String("Imported 7")));
        QVERIFY(dialog.statusText().contains(QLatin1String("skipped 3")));
        QCOMPARE(r.reg.shortcuts(QStringLiteral("edit.split")), QList<QKeySequence> {key("S")});
        QVERIFY(r.reg.shortcuts(QStringLiteral("timeline.snapping")).isEmpty()); // S was Snap
        QCOMPARE(r.reg.shortcuts(QStringLiteral("mark.add_marker")),
            QList<QKeySequence> {key("Shift+M")});
        QVERIFY(r.reg.shortcuts(QStringLiteral("transport.next_marker")).isEmpty());
        QVERIFY(hasKey(r.reg, "edit.undo", "Ctrl+Z"));
        QVERIFY(hasKey(r.reg, "transport.play_pause", "Space"));
        QVERIFY(hasKey(r.reg, "timeline.zoom_in", "="));
        QVERIFY(hasKey(r.reg, "edit.delete", "Num+0"));
        QVERIFY(hasKey(r.reg, "edit.ripple_delete", "Ctrl+Shift+Del"));
        // Commands the file does not name keep the Premiere preset's keys.
        QVERIFY(hasKey(r.reg, "tool.blade", "C"));
        // Cancel deletes the imported profile again.
        dialog.reject();
        QVERIFY(!r.reg.profile(id));

        // Broken files: an error, never a crash.
        const QString broken = r.dir.filePath(QStringLiteral("broken.kys"));
        writeFile(broken, "this is not xml at all");
        QVERIFY(r.reg.importProfile(broken, {}, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        const QString empty = r.dir.filePath(QStringLiteral("empty.kys"));
        writeFile(empty, "<?xml version=\"1.0\"?><PremiereData/>");
        ActionRegistry::ImportSummary summary;
        QVERIFY(!r.reg.importProfile(empty, {}, &error, &summary).isEmpty());
        QCOMPARE(summary.imported, 0);
    }

    /// Spec 23.18: a Final Cut Pro .commandset (XML property list): Command becomes Ctrl,
    /// Option Alt, Control Meta; unknown commands are skipped; a binary one is refused with a
    /// hint.
    void finalCutCommandSetImport()
    {
        Registry r;
        const QString path = r.dir.filePath(QStringLiteral("fcp.commandset"));
        writeFile(path, kCommandSet);
        ActionRegistry::ImportSummary summary;
        QString error;
        const QString id = r.reg.importProfile(path, {}, &error, &summary);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QCOMPARE(summary.format, QStringLiteral("Final Cut Pro"));
        QCOMPARE(summary.imported, 5);
        QCOMPARE(summary.skipped, 1); // MakeTea
        QCOMPARE(r.reg.profile(id)->basedOn, QStringLiteral("final-cut-pro"));
        QVERIFY(r.reg.setActiveProfile(id));
        QCOMPARE(r.reg.shortcuts(QStringLiteral("edit.split")),
            QList<QKeySequence> {key("Ctrl+Shift+B")}); // Command+Shift+B
        QCOMPARE(r.reg.shortcuts(QStringLiteral("mark.in")), QList<QKeySequence> {key("E")});
        QVERIFY(r.reg.shortcuts(QStringLiteral("edit.append_at_end")).isEmpty()); // E taken
        QCOMPARE(r.reg.shortcuts(QStringLiteral("transport.next_marker")),
            QList<QKeySequence> {key("Meta+Right")}); // Control+Right
        QCOMPARE(r.reg.shortcuts(QStringLiteral("timeline.snapping")),
            QList<QKeySequence> {key("Alt+N")});
        QCOMPARE(r.reg.shortcuts(QStringLiteral("edit.ripple_delete")),
            QList<QKeySequence> {key("Backspace")}); // Mac "delete" closes the gap
        QVERIFY(hasKey(r.reg, "tool.blade", "B")); // from the preset

        const QString binary = r.dir.filePath(QStringLiteral("binary.commandset"));
        writeFile(binary, QByteArray("bplist00\x01\x02\x03", 11));
        QVERIFY(r.reg.importProfile(binary, {}, &error).isEmpty());
        QVERIFY(error.contains(QLatin1String("plutil")));
    }

    void commandNames()
    {
        QCOMPARE(foreignkeys::actionForCommand(QStringLiteral("cmd.edit.undo")),
            QStringLiteral("edit.undo"));
        QCOMPARE(foreignkeys::actionForCommand(QStringLiteral("Ripple Delete")),
            QStringLiteral("edit.ripple_delete"));
        QCOMPARE(foreignkeys::actionForCommand(QStringLiteral("DeselectAll")),
            QStringLiteral("edit.deselect_all"));
        QCOMPARE(
            foreignkeys::actionForCommand(QStringLiteral("cmd.unknown.frobnicate")), QString());
    }

    /// Spec 38.32: the first start offers "Coming from another editor?" in the Project Manager,
    /// once; choosing a preset activates it; it never shows again.
    void firstRunChoiceIsShownOnce()
    {
        QTemporaryDir library;
        prefs::setProjectLibraryDirectory(library.path());
        QVERIFY(KeymapChoiceBanner::shouldOffer());
        {
            Window w;
            w.window->projectController()->showProjectManager();
            ProjectManagerWindow* pm = w.window->projectController()->projectManager();
            QVERIFY(pm && pm->isVisible());
            auto* banner = pm->findChild<KeymapChoiceBanner*>();
            QVERIFY(banner);
            QVERIFY(banner->isVisible());
            QVERIFY(!KeymapChoiceBanner::shouldOffer()); // shown: not again
            QComboBox* presets = banner->presetCombo();
            QCOMPARE(
                presets->itemData(0).toString(), QLatin1String(ActionRegistry::kDefaultProfile));
            QCOMPARE(presets->count(), kPresets.size());
            presets->setCurrentIndex(presets->findData(QStringLiteral("vegas-pro")));
            banner->useSelected();
            QVERIFY(!banner->isVisible());
            QCOMPARE(w.actions.activeProfile(), QStringLiteral("vegas-pro"));
            QVERIFY(hasKey(w.actions, "edit.split", "S"));
            pm->hide();
        }
        {
            // The next start: the chosen preset is active, no banner.
            Window w;
            QCOMPARE(w.actions.activeProfile(), QStringLiteral("vegas-pro"));
            w.window->projectController()->showProjectManager();
            ProjectManagerWindow* pm = w.window->projectController()->projectManager();
            QVERIFY(!pm->findChild<KeymapChoiceBanner*>());
            pm->hide();
        }
        // Skipping also ends it, and keeps Resolve's keys.
        QSettings().clear();
        {
            Registry r;
            KeymapChoiceBanner banner(r.reg);
            banner.show();
            banner.skip();
            QCOMPARE(r.reg.activeProfile(), QLatin1String(ActionRegistry::kDefaultProfile));
            QVERIFY(!KeymapChoiceBanner::shouldOffer());
        }
        // A user who already chose a keymap (an earlier version) is not asked.
        QSettings().clear();
        QSettings().setValue(QStringLiteral("keymap/profile"), QStringLiteral("resolve-windows"));
        QVERIFY(!KeymapChoiceBanner::shouldOffer());
        prefs::setProjectLibraryDirectory({});
    }
};

QTEST_MAIN(TestKeyPresets)
#include "test_keypresets.moc"
