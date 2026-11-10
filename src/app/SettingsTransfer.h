// SPDX-License-Identifier: GPL-3.0-or-later
// SettingsTransfer — export and import of all the application's settings as one file
// (spec section 61). No widgets: the dialog (SettingsTransferDialog) and the `settings.export` /
// `settings.import` commands both use this.
//
// The file is JSON with the suffix .kinoklip-settings: a header (format number, application
// version), the QSettings keys the application owns ("settings"), the list of recent projects
// (only when asked for) and the small user files in the data folder ("files": custom keyboard
// profiles, guide presets, power bins, render presets). It is untrusted input on import: every
// key must match a known rule and pass its type and range check, unknown keys are ignored and
// file names are checked against a fixed list of patterns.
#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace kk::settingstransfer {

inline constexpr int kFormatNumber = 1; ///< a file with a higher number is refused
inline constexpr char kFileSuffix[] = "kinoklip-settings";
inline constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024; ///< whole file
inline constexpr qint64 kMaxUserFileBytes = 1024 * 1024; ///< one embedded user file
inline constexpr int kMaxKeys = 5000;
inline constexpr int kMaxFiles = 200;
inline constexpr int kMaxRecentProjects = 100;

/// Where the files of "files" live and where backups go. Tests point both at a temporary folder.
struct Locations
{
    QString dataDir; ///< the application's data folder (QStandardPaths::AppDataLocation)
    QString backupDir; ///< next to the settings file: "<config folder>/settings-backups"
};
[[nodiscard]] Locations standardLocations();

struct ExportOptions
{
    bool includeRecentProjects = false; ///< off by default: the paths differ on another computer
};

/// A checked settings file, ready to apply.
struct Parsed
{
    int formatNumber = 0;
    QString appVersion;
    QString created;
    QVariantMap settings; ///< checked values by key (bool, int, double, QString, QStringList, QByteArray)
    QMap<QString, QByteArray> files; ///< relative path in the data folder -> content
    QStringList recentProjects;
    bool hasRecentProjects = false;
    int ignoredKeys = 0; ///< keys no rule knows (ignored without a message)
    QStringList rejected; ///< known keys / files that failed the checks, one sentence each
    bool localControlStaysOff = false; ///< the file asked for Local Control on: not applied
    /// What the file contains, one line per area ("Editing preferences: 14 settings").
    QStringList summary;
};

struct ApplyOptions
{
    bool includeRecentProjects = false;
    Locations locations = standardLocations();
};

struct ApplyResult
{
    bool ok = false;
    QString error;
    QString backupPath; ///< the backup of the settings that were replaced
    int settingsApplied = 0;
    int filesApplied = 0;
    bool restartNeeded = false;
    QStringList notes; ///< things the user should know (Local Control, rejected entries)
};

/// The settings file as bytes (compact enough to read; keys sorted).
[[nodiscard]] QByteArray exportDocument(
    const ExportOptions& options = {}, const Locations& locations = standardLocations());
/// Writes exportDocument() to @p path (atomically). False with @p error on failure.
[[nodiscard]] bool exportToFile(const QString& path, const ExportOptions& options = {},
    QString* error = nullptr, const Locations& locations = standardLocations());

/// Checks @p data. False with a clear message in @p error for a file that cannot be used at all
/// (not JSON, not a Kinoklip settings file, a newer format, too large); otherwise fills @p out.
[[nodiscard]] bool parse(const QByteArray& data, Parsed* out, QString* error);
[[nodiscard]] bool parseFile(const QString& path, Parsed* out, QString* error);

/// Backs up the current settings, then applies @p parsed (values in the file replace the
/// current ones; settings not in the file stay). Local Control is never switched on.
[[nodiscard]] ApplyResult apply(const Parsed& parsed, const ApplyOptions& options = {});
/// parseFile() + apply() in one step (the command).
[[nodiscard]] ApplyResult importFile(const QString& path, const ApplyOptions& options = {});

/// True once an import changed the stored settings in this run: the open window must not write
/// its own (old) layout over them when it closes.
[[nodiscard]] bool importPending();
/// Tells the open window that settings were replaced.
class Notifier final : public QObject
{
    Q_OBJECT
signals:
    /// After every successful apply(); the main window re-reads Local Control.
    void applied();
};
[[nodiscard]] Notifier* notifier();

/// The area a key belongs to for the summary ("Keyboard", "Editing", ...); empty = unknown key.
[[nodiscard]] QString areaOfKey(const QString& key);

} // namespace kk::settingstransfer
