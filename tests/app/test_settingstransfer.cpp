// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the settings export / import (spec section 61): the round trip of many kinds of
// settings and user files, what is left out, partial files, hostile files, Local Control never
// switched on, the backup, the settings.export / settings.import commands and the dialog.
#include "app/SettingsTransfer.h"
#include "app/SettingsTransferDialog.h"
#include "app/commands/Command.h"
#include "core/Preferences.h"

#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QTimer>
#include <memory>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
namespace st = kk::settingstransfer;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// A settings file made by hand: header plus the given settings / files.
QByteArray makeFile(const QJsonObject& settings, const QJsonObject& files = {},
    int format = st::kFormatNumber, const QJsonValue& recent = QJsonValue::Undefined)
{
    QJsonObject root {{QStringLiteral("kinoklipSettings"),
                          QJsonObject {{QStringLiteral("format"), format},
                              {QStringLiteral("appVersion"), QStringLiteral("9.9.9")}}},
        {QStringLiteral("settings"), settings}, {QStringLiteral("files"), files}};
    if (!recent.isUndefined())
        root.insert(QStringLiteral("recentProjects"), recent);
    return QJsonDocument(root).toJson();
}

} // namespace

class TestSettingsTransfer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    st::Locations m_loc;

    st::ApplyResult importBytes(const QByteArray& data, bool recent = false)
    {
        st::Parsed parsed;
        QString error;
        if (!st::parse(data, &parsed, &error)) {
            st::ApplyResult r;
            r.error = error;
            return r;
        }
        st::ApplyOptions options;
        options.includeRecentProjects = recent;
        options.locations = m_loc;
        return st::apply(parsed, options);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_settingstransfer"));
        QVERIFY(m_dir.isValid());
    }

    void init()
    {
        QSettings().clear();
        QDir(m_dir.path()).removeRecursively();
        m_loc.dataDir = m_dir.path() + QStringLiteral("/data");
        m_loc.backupDir = m_dir.path() + QStringLiteral("/backups");
        QDir().mkpath(m_loc.dataDir);
    }

    void roundTripManyKinds()
    {
        QSettings s;
        s.setValue(QStringLiteral("ui/colorScheme"), QStringLiteral("BreezeLight"));
        s.setValue(QStringLiteral("ui/animations"), false);
        s.setValue(QStringLiteral("ui/animationSpeed"), 1.5);
        s.setValue(QStringLiteral("project/autosaveMinutes"), 7);
        s.setValue(QStringLiteral("project/startupAction"), QStringLiteral("last"));
        s.setValue(QStringLiteral("playback/hwaccel"), QStringLiteral("off"));
        s.setValue(QStringLiteral("playback/prerollMs"), 3500);
        s.setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        s.setValue(QStringLiteral("edit/stillSeconds"), 4.5);
        s.setValue(QStringLiteral("edit/threePointEditing"), true);
        s.setValue(QStringLiteral("timeline/thumbnails"), QStringLiteral("headtail"));
        s.setValue(QStringLiteral("editIndex/hiddenColumns/edits"),
            QStringList {QStringLiteral("a"), QStringLiteral("b")});
        s.setValue(QStringLiteral("mainWindow/state"), QByteArray("\x01\x02state\xff", 8));
        s.setValue(QStringLiteral("pages/edit/main"), QByteArray("splitter-bytes"));
        s.setValue(QStringLiteral("keymap/profile"), QStringLiteral("vim"));
        s.setValue(QStringLiteral("shortcuts/resolve-windows/edit.undo"), QStringLiteral("Ctrl+Y"));
        s.setValue(QStringLiteral("paths/recentProjects"), QStringList {QStringLiteral("/a.kkproj")});
        s.setValue(QStringLiteral("system/cacheDirectory"), QStringLiteral("/home/other/cache"));
        s.setValue(QStringLiteral("system/audioOutputDevice"), QStringLiteral("alsa:xyz"));
        writeFile(m_loc.dataDir + QStringLiteral("/keymaps/mine.json"), "{\"shortcuts\": {}}");
        writeFile(m_loc.dataDir + QStringLiteral("/guide-presets.json"), "[]");
        writeFile(m_loc.dataDir + QStringLiteral("/export_presets.json"), "{\"presets\": []}");
        writeFile(m_loc.dataDir + QStringLiteral("/power-bins.json"), "[]");
        writeFile(m_loc.dataDir + QStringLiteral("/autosave/x.kkproj"), "{}");

        const QString file = m_dir.path() + QStringLiteral("/out.kinoklip-settings");
        QString error;
        QVERIFY2(st::exportToFile(file, {}, &error, m_loc), qPrintable(error));
        const QByteArray text = readFile(file);
        QVERIFY(!text.contains("cacheDirectory"));
        QVERIFY(!text.contains("audioOutputDevice"));
        QVERIFY(!text.contains("recentProjects"));
        QVERIFY(!text.contains("autosave/"));
        QVERIFY(text.contains("\"format\": 1"));

        QSettings().clear();
        QDir(m_loc.dataDir).removeRecursively();
        s.setValue(QStringLiteral("system/cacheDirectory"), QStringLiteral("/mine"));

        st::Parsed parsed;
        QVERIFY2(st::parseFile(file, &parsed, &error), qPrintable(error));
        QVERIFY(parsed.rejected.isEmpty());
        st::ApplyOptions options;
        options.locations = m_loc;
        const st::ApplyResult r = st::apply(parsed, options);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.restartNeeded);
        QVERIFY(r.settingsApplied >= 15);
        QCOMPARE(r.filesApplied, 4);

        QSettings t;
        QCOMPARE(t.value(QStringLiteral("ui/colorScheme")).toString(), QStringLiteral("BreezeLight"));
        QCOMPARE(t.value(QStringLiteral("ui/animations")).toBool(), false);
        QCOMPARE(t.value(QStringLiteral("ui/animationSpeed")).toDouble(), 1.5);
        QCOMPARE(t.value(QStringLiteral("project/autosaveMinutes")).toInt(), 7);
        QCOMPARE(t.value(QStringLiteral("project/startupAction")).toString(), QStringLiteral("last"));
        QCOMPARE(t.value(QStringLiteral("playback/hwaccel")).toString(), QStringLiteral("off"));
        QCOMPARE(t.value(QStringLiteral("playback/prerollMs")).toInt(), 3500);
        QCOMPARE(t.value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
        QCOMPARE(t.value(QStringLiteral("edit/stillSeconds")).toDouble(), 4.5);
        QCOMPARE(t.value(QStringLiteral("edit/threePointEditing")).toBool(), true);
        QCOMPARE(t.value(QStringLiteral("timeline/thumbnails")).toString(), QStringLiteral("headtail"));
        QCOMPARE(t.value(QStringLiteral("editIndex/hiddenColumns/edits")).toStringList(),
            (QStringList {QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(t.value(QStringLiteral("mainWindow/state")).toByteArray(), QByteArray("\x01\x02state\xff", 8));
        QCOMPARE(t.value(QStringLiteral("pages/edit/main")).toByteArray(), QByteArray("splitter-bytes"));
        QCOMPARE(t.value(QStringLiteral("keymap/profile")).toString(), QStringLiteral("vim"));
        QCOMPARE(t.value(QStringLiteral("shortcuts/resolve-windows/edit.undo")).toString(), QStringLiteral("Ctrl+Y"));
        // Machine-specific values are not touched by an import, nor is the recent list.
        QCOMPARE(t.value(QStringLiteral("system/cacheDirectory")).toString(), QStringLiteral("/mine"));
        QVERIFY(!t.contains(QStringLiteral("paths/recentProjects")));
        QCOMPARE(readFile(m_loc.dataDir + QStringLiteral("/keymaps/mine.json")), QByteArray("{\"shortcuts\": {}}"));
        QVERIFY(QFile::exists(m_loc.dataDir + QStringLiteral("/power-bins.json")));
        QVERIFY(!QFile::exists(m_loc.dataDir + QStringLiteral("/autosave/x.kkproj")));
    }

    void recentProjectsOnlyOnRequest()
    {
        QSettings().setValue(QStringLiteral("paths/recentProjects"), QStringList {QStringLiteral("/a.kkproj"), QStringLiteral("/b.kkproj")});
        QVERIFY(!st::exportDocument({}, m_loc).contains("/a.kkproj"));
        const QByteArray with = st::exportDocument({true}, m_loc);
        QVERIFY(with.contains("/a.kkproj"));
        QSettings().clear();
        QVERIFY(importBytes(with, false).ok);
        QVERIFY(!QSettings().contains(QStringLiteral("paths/recentProjects")));
        QVERIFY(importBytes(with, true).ok);
        QCOMPARE(QSettings().value(QStringLiteral("paths/recentProjects")).toStringList().size(), 2);
    }

    void backupHoldsTheOldSettings()
    {
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        QSettings().setValue(QStringLiteral("paths/recentProjects"), QStringList {QStringLiteral("/old.kkproj")});
        const st::ApplyResult r = importBytes(makeFile({{QStringLiteral("edit/zoomAnchor"), QStringLiteral("playhead")}}));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.backupPath.startsWith(m_loc.backupDir));
        QVERIFY(r.backupPath.endsWith(QStringLiteral(".kinoklip-settings")));
        st::Parsed backup;
        QString error;
        QVERIFY2(st::parseFile(r.backupPath, &backup, &error), qPrintable(error));
        QCOMPARE(backup.settings.value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
        QVERIFY(backup.recentProjects.contains(QStringLiteral("/old.kkproj")));
        QCOMPARE(QSettings().value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("playhead"));
    }

    void backupFailureChangesNothing()
    {
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        writeFile(m_dir.path() + QStringLiteral("/blocker"), "x");
        m_loc.backupDir = m_dir.path() + QStringLiteral("/blocker/sub"); // a file is in the way
        const st::ApplyResult r = importBytes(makeFile({{QStringLiteral("edit/zoomAnchor"), QStringLiteral("playhead")}}));
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("nothing was changed")));
        QCOMPARE(QSettings().value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
    }

    void partialFileKeepsTheRest()
    {
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        QSettings().setValue(QStringLiteral("project/autosaveMinutes"), 9);
        const st::ApplyResult r = importBytes(makeFile({{QStringLiteral("project/autosaveMinutes"), 3}}));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.settingsApplied, 1);
        QCOMPARE(QSettings().value(QStringLiteral("project/autosaveMinutes")).toInt(), 3);
        QCOMPARE(QSettings().value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
        // A file with only a header is valid and changes nothing.
        QVERIFY(importBytes(makeFile({})).ok);
        QCOMPARE(QSettings().value(QStringLiteral("project/autosaveMinutes")).toInt(), 3);
    }

    void keymapGroupIsReplacedWhole()
    {
        QSettings().setValue(QStringLiteral("shortcuts/vim/edit.undo"), QStringLiteral("U"));
        QSettings().setValue(QStringLiteral("shortcuts/vim/edit.redo"), QStringLiteral("R"));
        QVERIFY(importBytes(makeFile({{QStringLiteral("shortcuts/vim/edit.redo"), QStringLiteral("Ctrl+R")}})).ok);
        QVERIFY(!QSettings().contains(QStringLiteral("shortcuts/vim/edit.undo")));
        QCOMPARE(QSettings().value(QStringLiteral("shortcuts/vim/edit.redo")).toString(), QStringLiteral("Ctrl+R"));
    }

    void hostileHeaders()
    {
        QCOMPARE(importBytes("not json at all").ok, false);
        QVERIFY(importBytes("not json").error.contains(QStringLiteral("not a Kinoklip")));
        QVERIFY(!importBytes("[1,2,3]").ok);
        QVERIFY(!importBytes("{\"settings\": {}}").ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": 5}").ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": {\"format\": \"1\"}}").ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": {\"format\": 0}}").ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": {\"format\": 1.5}}").ok);
        const st::ApplyResult newer = importBytes(makeFile({}, {}, st::kFormatNumber + 1));
        QVERIFY(!newer.ok);
        QVERIFY(newer.error.contains(QStringLiteral("newer Kinoklip")));
        QVERIFY(!importBytes(QByteArray(st::kMaxFileBytes + 1, ' ')).ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": {\"format\": 1}, \"settings\": [1]}").ok);
        QVERIFY(!importBytes("{\"kinoklipSettings\": {\"format\": 1}, \"files\": \"x\"}").ok);
    }

    void hostileValues()
    {
        QSettings s;
        QJsonObject bad {
            {QStringLiteral("project/autosaveMinutes"), 100000},
            {QStringLiteral("ui/animations"), QStringLiteral("yes")},
            {QStringLiteral("ui/animationSpeed"), 0.0},
            {QStringLiteral("edit/zoomAnchor"), QStringLiteral("elsewhere")},
            {QStringLiteral("playback/hwaccel"), QJsonArray {1}},
            {QStringLiteral("system/stillCacheMiB"), 1.5},
            {QStringLiteral("ui/widgetStyle"), QString(100, QLatin1Char('x'))},
            {QStringLiteral("ui/colorScheme"), QStringLiteral("a\nb\x01")},
            {QStringLiteral("editIndex/hiddenColumns/x"), QStringLiteral("not a list")},
            {QStringLiteral("mainWindow/state"), QJsonObject {{QStringLiteral("bytes"), QStringLiteral("!!!notbase64")}}},
            {QStringLiteral("mainWindow/geometry"), QJsonObject {{QStringLiteral("bytes"), QString(800000, QLatin1Char('A'))}}},
        };
        st::Parsed parsed;
        QString error;
        QVERIFY(st::parse(makeFile(bad), &parsed, &error));
        QVERIFY(parsed.settings.isEmpty());
        QVERIFY(parsed.rejected.size() >= 9);
        // Unknown keys and keys with a path-like shape are ignored without a message.
        QJsonObject unknown {{QStringLiteral("nothing/known"), 1}, {QStringLiteral("../etc/passwd"), 1},
            {QStringLiteral("edit/../../x"), 1}, {QStringLiteral("/abs"), 1}, {QStringLiteral(""), 1},
            {QStringLiteral("paths/lastMediaDir"), QStringLiteral("/tmp")},
            {QStringLiteral("system/cacheDirectory"), QStringLiteral("/tmp/x")},
            {QStringLiteral("project/libraryDirectory"), QStringLiteral("/tmp/x")}};
        QVERIFY(st::parse(makeFile(unknown), &parsed, &error));
        QVERIFY(parsed.settings.isEmpty());
        QVERIFY(parsed.rejected.isEmpty());
        QCOMPARE(parsed.ignoredKeys, unknown.size());
        QVERIFY(importBytes(makeFile(unknown)).ok == true);
        QVERIFY(!QSettings().contains(QStringLiteral("paths/lastMediaDir")));
        QVERIFY(!QSettings().contains(QStringLiteral("system/cacheDirectory")));
        QVERIFY(QSettings().allKeys().isEmpty());
    }

    void hostileFilesNeverEscape()
    {
        const QByteArray ok = "{}";
        QJsonObject files {
            {QStringLiteral("../outside.json"), QString::fromLatin1(ok)},
            {QStringLiteral("keymaps/../../outside.json"), QString::fromLatin1(ok)},
            {QStringLiteral("keymaps/..\\x.json"), QString::fromLatin1(ok)},
            {QStringLiteral("/tmp/kk-absolute.json"), QString::fromLatin1(ok)},
            {QStringLiteral("keymaps/sub/x.json"), QString::fromLatin1(ok)},
            {QStringLiteral("keymaps/.hidden.json"), QString::fromLatin1(ok)},
            {QStringLiteral("keymaps/x.sh"), QString::fromLatin1(ok)},
            {QStringLiteral("autosave/x.json"), QString::fromLatin1(ok)},
            {QStringLiteral("power-bins.json"), QStringLiteral("this is not json")},
            {QStringLiteral("guide-presets.json"), QString(st::kMaxUserFileBytes + 1, QLatin1Char(' '))},
            {QStringLiteral("export_presets.json"), 12},
        };
        st::Parsed parsed;
        QString error;
        QVERIFY(st::parse(makeFile({}, files), &parsed, &error));
        QVERIFY(parsed.files.isEmpty());
        QCOMPARE(parsed.rejected.size(), files.size());
        QVERIFY(importBytes(makeFile({}, files)).ok);
        QVERIFY(!QFile::exists(m_dir.path() + QStringLiteral("/outside.json")));
        QVERIFY(!QFile::exists(QStringLiteral("/tmp/kk-absolute.json")));
        QVERIFY(!QFile::exists(m_loc.dataDir + QStringLiteral("/../outside.json")));
        QVERIFY(QDir(m_loc.dataDir).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        // A good file name next to the bad ones is still written.
        QVERIFY(importBytes(makeFile({}, {{QStringLiteral("keymaps/good name.json"), QStringLiteral("{}")}})).ok);
        QVERIFY(QFile::exists(m_loc.dataDir + QStringLiteral("/keymaps/good name.json")));
    }

    void tooManyKeysAndFiles()
    {
        QJsonObject many;
        for (int i = 0; i <= st::kMaxKeys; ++i)
            many.insert(QStringLiteral("mediaPool/k%1").arg(i), 1);
        QVERIFY(!importBytes(makeFile(many)).ok);
        QJsonObject files;
        for (int i = 0; i < st::kMaxFiles + 20; ++i)
            files.insert(QStringLiteral("keymaps/k%1.json").arg(i), QStringLiteral("{}"));
        st::Parsed parsed;
        QString error;
        QVERIFY(st::parse(makeFile({}, files), &parsed, &error));
        QCOMPARE(parsed.files.size(), st::kMaxFiles);
    }

    void localControlNeverSwitchedOn()
    {
        QCOMPARE(prefs::localControlEnabled(), false);
        st::ApplyResult r = importBytes(makeFile({{QStringLiteral("control/localTools"), true},
            {QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse")}}));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(prefs::localControlEnabled(), false);
        QVERIFY(!QSettings().contains(QStringLiteral("control/localTools")));
        QVERIFY(!r.notes.isEmpty());
        // On here, off in the file: switched off.
        prefs::setLocalControlEnabled(true);
        r = importBytes(makeFile({{QStringLiteral("control/localTools"), false}}));
        QVERIFY(r.ok);
        QCOMPARE(prefs::localControlEnabled(), false);
        // On here, "on" in the file, nothing else: stays as it was (on), never forced either way.
        prefs::setLocalControlEnabled(true);
        QVERIFY(importBytes(makeFile({{QStringLiteral("control/localTools"), true}})).ok);
        QCOMPARE(prefs::localControlEnabled(), true);
        // The wrong type does not switch anything.
        prefs::setLocalControlEnabled(false);
        QVERIFY(importBytes(makeFile({{QStringLiteral("control/localTools"), QStringLiteral("true")}})).ok);
        QCOMPARE(prefs::localControlEnabled(), false);
        // An exported file carries the real value, and importing it again cannot raise it.
        prefs::setLocalControlEnabled(true);
        const QByteArray exported = st::exportDocument({}, m_loc);
        prefs::setLocalControlEnabled(false);
        QVERIFY(importBytes(exported).ok);
        QCOMPARE(prefs::localControlEnabled(), false);
    }

    void commandsRoundTrip()
    {
        const auto& registry = commands::CommandRegistry::builtin();
        QVERIFY(registry.find(QStringLiteral("settings.export")));
        QVERIFY(registry.find(QStringLiteral("settings.import")));
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        const QString file = m_dir.path() + QStringLiteral("/cmd.kinoklip-settings");
        commands::CommandResult r = registry.execute(QStringLiteral("settings.export"), {{QStringLiteral("file"), file}});
        QVERIFY2(r.ok, qPrintable(r.text));
        QVERIFY(QFile::exists(file));
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("playhead"));
        r = registry.execute(QStringLiteral("settings.import"), {{QStringLiteral("file"), file}});
        QVERIFY2(r.ok, qPrintable(r.text));
        QCOMPARE(QSettings().value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
        QVERIFY(r.data.value(QStringLiteral("restartNeeded")).toBool());
        QVERIFY(QFile::exists(r.data.value(QStringLiteral("backup")).toString()));
        // Errors: missing file, hostile file, newer format.
        r = registry.execute(QStringLiteral("settings.import"), {{QStringLiteral("file"), m_dir.path() + QStringLiteral("/none")}});
        QVERIFY(!r.ok);
        writeFile(m_dir.path() + QStringLiteral("/new.kinoklip-settings"), makeFile({}, {}, 99));
        r = registry.execute(QStringLiteral("settings.import"), {{QStringLiteral("file"), m_dir.path() + QStringLiteral("/new.kinoklip-settings")}});
        QVERIFY(!r.ok);
        // Local Control by command: a file cannot switch it on.
        writeFile(m_dir.path() + QStringLiteral("/lc.kinoklip-settings"), makeFile({{QStringLiteral("control/localTools"), true}}));
        r = registry.execute(QStringLiteral("settings.import"), {{QStringLiteral("file"), m_dir.path() + QStringLiteral("/lc.kinoklip-settings")}});
        QVERIFY(r.ok);
        QCOMPARE(prefs::localControlEnabled(), false);
        QVERIFY(r.data.value(QStringLiteral("notes")).toArray().size() >= 1);
    }

    void importDialogFlow()
    {
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse"));
        writeFile(m_loc.dataDir + QStringLiteral("/guide-presets.json"), "[]");
        const QByteArray data = st::exportDocument({true}, m_loc);
        QSettings().setValue(QStringLiteral("edit/zoomAnchor"), QStringLiteral("playhead"));
        st::Parsed parsed;
        QString error;
        QVERIFY(st::parse(data, &parsed, &error));
        std::unique_ptr<SettingsTransferDialog> d(SettingsTransferDialog::forImport(QStringLiteral("/x/a.kinoklip-settings"), parsed, m_loc));
        QVERIFY(d->contentsText().contains(QStringLiteral("Editing preferences")));
        QVERIFY(d->contentsText().contains(QStringLiteral("Guide presets")));
        auto* replace = d->findChild<QLabel*>(QStringLiteral("ReplaceWarning"));
        QVERIFY(replace && replace->text().contains(QStringLiteral("replaces your current settings")));
        QVERIFY(!d->includeRecentProjects());
        auto* button = d->findChild<QPushButton*>(QStringLiteral("ImportButton"));
        QVERIFY(button && button->isEnabled());
        auto* recent = d->findChild<QCheckBox*>(QStringLiteral("IncludeRecentProjects"));
        QVERIFY(recent);
        QVERIFY(!recent->isChecked()); // off by default
        d->show();
        QTimer::singleShot(0, d.get(), [&] { button->click(); });
        QCOMPARE(d->exec(), int(QDialog::Accepted));
        st::ApplyOptions options;
        options.locations = m_loc;
        const st::ApplyResult r = st::apply(parsed, options);
        QVERIFY(r.ok);
        QCOMPARE(QSettings().value(QStringLiteral("edit/zoomAnchor")).toString(), QStringLiteral("mouse"));
        // Cancel does not apply anything.
        std::unique_ptr<SettingsTransferDialog> c(SettingsTransferDialog::forImport(QStringLiteral("/x/a.kinoklip-settings"), parsed, m_loc));
        c->show();
        QTimer::singleShot(0, c.get(), [&] { c->reject(); });
        QCOMPARE(c->exec(), int(QDialog::Rejected));
        // An empty file cannot be imported.
        st::Parsed empty;
        QVERIFY(st::parse(makeFile({}), &empty, &error));
        std::unique_ptr<SettingsTransferDialog> e(SettingsTransferDialog::forImport(QStringLiteral("/x/e"), empty, m_loc));
        QVERIFY(!e->findChild<QPushButton*>(QStringLiteral("ImportButton"))->isEnabled());
        // Export dialog.
        std::unique_ptr<SettingsTransferDialog> x(SettingsTransferDialog::forExport(QStringLiteral("/x/out.kinoklip-settings"), nullptr));
        QVERIFY(x->findChild<QPushButton*>(QStringLiteral("ExportButton")));
        QVERIFY(!x->includeRecentProjects());
    }

    void importPendingFlag()
    {
        QVERIFY(importBytes(makeFile({{QStringLiteral("edit/zoomAnchor"), QStringLiteral("mouse")}})).ok);
        QVERIFY(st::importPending());
    }
};

QTEST_MAIN(TestSettingsTransfer)
#include "test_settingstransfer.moc"
