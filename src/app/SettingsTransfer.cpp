// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SettingsTransfer.h"

#include "app/AppInfo.h"
#include "core/Preferences.h"
#include "ui/common/VimOptions.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kk::settingstransfer {

namespace {

constexpr char kHeaderKey[] = "kinoklipSettings";
constexpr char kLocalControlKey[] = "control/localTools";
constexpr char kRecentKey[] = "paths/recentProjects";
constexpr int kMaxKeyLength = 200;
constexpr int kMaxTextLength = 65536;
constexpr qsizetype kMaxBytesValue = 512 * 1024;
constexpr int kMaxListItems = 1000;
constexpr int kMaxListItemLength = 4096;

enum class Kind {
    Bool,
    Int,
    Double,
    Choice,
    Text,
    TextList,
    Auto
};

/// What one key (or a whole group, pattern ending in "/") must look like. First match wins.
struct Rule
{
    const char* pattern; ///< exact key, or a prefix ending in '/'
    Kind kind;
    double min = 0;
    double max = 0;
    QStringList choices;
    bool exclude = false; ///< machine-specific: neither exported nor imported
};

const QVector<Rule>& rules()
{
    static const QVector<Rule> table = [] {
        QVector<Rule> r;
        auto add = [&r](const char* p, Kind k, double lo = 0, double hi = 0, QStringList c = {}) {
            r.append({p, k, lo, hi, std::move(c), false});
        };
        auto skip = [&r](const char* p) { r.append({p, Kind::Auto, 0, 0, {}, true}); };
        // Machine-specific: folders and devices that would not exist on another computer.
        skip("project/libraryDirectory");
        skip("system/cacheDirectory");
        skip("system/proxyDirectory");
        skip("system/audioOutputDevice");
        skip("system/audioInputDevice");
        skip("color/lutFolders");
        skip("compositing/paramFile");
        skip("paths/"); // last-used folders and the recent-projects list (a separate option)
        // Local Control is special-cased in apply(); it is the only Bool that may differ.
        add("control/localTools", Kind::Bool);
        add("ui/widgetStyle", Kind::Text, 0, 64);
        add("ui/colorScheme", Kind::Text, 0, 128);
        add("ui/useSystemColors", Kind::Bool);
        add("ui/menuLayout", Kind::Choice, 0, 0, {"resolve", "streamlined"});
        add("ui/animations", Kind::Bool);
        add("ui/animationSpeed", Kind::Double, 0.1, 10);
        add("ui/scaleFactor", Kind::Text, 0, 16);
        add("ui/focusSearchOnPanelKey", Kind::Bool);
        add("project/autosaveMinutes", Kind::Int, 0, 120);
        add("project/startupAction", Kind::Choice, 0, 0, {"manager", "last"});
        add("system/stillCacheMiB", Kind::Int, 16, 65536);
        add("system/renderBackend", Kind::Choice, 0, 0, {"auto", "cpu"});
        add("system/renderCacheLimitGiB", Kind::Int, 1, 100000);
        add("playback/hwaccel", Kind::Choice, 0, 0, prefs::hwAccelIds());
        add("playback/previewQuality", Kind::Choice, 0, 0, {"full", "half", "quarter", "auto"});
        add("playback/previewQualityRevision", Kind::Int, 0, 100);
        add("playback/prerollMs", Kind::Int, 0, 600000);
        add("playback/postrollMs", Kind::Int, 0, 600000);
        for (const char* k :
            {"playback/audioScrubbing", "playback/restartAtEnd", "playback/returnAfterPlayAround",
                "playback/stopOnPlayheadMove", "playback/stopOnEdit"})
            add(k, Kind::Bool);
        add("voiceOver/countInSeconds", Kind::Int, 0, 10);
        add("voiceOver/latencyOffsetMs", Kind::Int, 0, 1000);
        add("voiceOver/bitDepth", Kind::Int, 16, 24);
        add("edit/zoomAnchor", Kind::Choice, 0, 0, {"playhead", "mouse"});
        for (const char* k :
            {"edit/stillSeconds", "edit/generatorSeconds", "edit/transitionSeconds"})
            add(k, Kind::Double, 0.01, 3600);
        add("edit/newKeyInterp", Kind::Choice, 0, 0,
            {"linear", "easeIn", "easeOut", "easeInOut", "smooth", "hold"});
        add("edit/multiFrameNudge", Kind::Int, 1, 1000);
        add("edit/wheelScheme", Kind::Auto);
        for (const char* k : {"edit/threePointEditing", "edit/rippleDeleteGapAtPlayhead",
                 "edit/rippleDeleteClipAtPlayhead", "edit/selectToolRollsAtCut",
                 "edit/trimToolSlideInNameBar", "edit/twoUpProgramLook", "edit/timelineTabs",
                 "edit/rulerPressJumpsPlayhead", "edit/trimToPlayheadRipples"})
            add(k, Kind::Bool);
        add("timeline/thumbnails", Kind::Choice, 0, 0, {"filmstrip", "headtail", "none"});
        add("timeline/waveforms", Kind::Bool);
        add("timeline/sourceTimecode", Kind::Bool);
        add("timeline/markerColor", Kind::Text, 0, 64);
        add("timeline/flagColor", Kind::Text, 0, 64);
        add("effects/livePreview", Kind::Bool);
        add("mediapool/livePreview", Kind::Bool);
        add("deliver/notifyWhenFinished", Kind::Bool);
        add("editIndex/hiddenColumns/", Kind::TextList);
        add("vim/enabled", Kind::Bool);
        add("vim/physicalKeys", Kind::Bool);
        add("vim/timeoutlen", Kind::Int, 50, 5000);
        add("vim/leader", Kind::Text, 0, 32);
        add("vim/maps", Kind::Text, 0, 65536);
        // Layouts, view options and remembered choices of the panels and dialogs: any plain
        // value (text, number, flag, list, saved window state) of a bounded size.
        for (const char* g : {"mainWindow/", "pages/", "projectManager/", "mediaPool/", "timeline/",
                 "editIndex/", "effects/", "deliver/", "import/", "viewer/", "colorPage/",
                 "dialogs/", "keyframes/", "scopesWindow/", "keymap/", "shortcuts/", "edit/", "ui/",
                 "playback/", "project/", "system/", "multicam/", "scopes/"})
            add(g, Kind::Auto);
        return r;
    }();
    return table;
}

const Rule* ruleFor(const QString& key)
{
    for (const Rule& rule : rules()) {
        const QLatin1String p(rule.pattern);
        if (p.endsWith(QLatin1Char('/')) ? key.startsWith(p) : key == p)
            return &rule;
    }
    return nullptr;
}

bool keyIsSane(const QString& key)
{
    static const QRegularExpression ok(
        QStringLiteral("^[A-Za-z0-9_.@ +%-]+(/[A-Za-z0-9_.@ +%-]+)*$"));
    if (key.isEmpty() || key.size() > kMaxKeyLength || !ok.match(key).hasMatch())
        return false;
    const QStringList parts = key.split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(),
        [](const QString& s) { return s == QLatin1String(".") || s == QLatin1String(".."); });
}

bool plainText(const QString& s, int maxLength)
{
    if (s.size() > maxLength)
        return false;
    return std::none_of(s.cbegin(), s.cend(), [](QChar c) {
        return c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\t');
    });
}

/// A value read from QSettings (INI files give back strings for numbers and flags), as the JSON
/// value of the file; Undefined when it cannot be stored.
QJsonValue toJson(const QVariant& v, const Rule& rule)
{
    switch (rule.kind) {
    case Kind::Bool: {
        const QString s = v.toString().toLower();
        if (v.typeId() == QMetaType::Bool)
            return v.toBool();
        if (s == QLatin1String("true") || s == QLatin1String("1"))
            return true;
        if (s == QLatin1String("false") || s == QLatin1String("0"))
            return false;
        return QJsonValue::Undefined;
    }
    case Kind::Int: {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? QJsonValue(double(n)) : QJsonValue(QJsonValue::Undefined);
    }
    case Kind::Double: {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        return ok && std::isfinite(d) ? QJsonValue(d) : QJsonValue(QJsonValue::Undefined);
    }
    case Kind::Choice:
    case Kind::Text:
        return v.typeId() == QMetaType::QString ? QJsonValue(v.toString())
                                                : QJsonValue(QJsonValue::Undefined);
    case Kind::TextList:
    case Kind::Auto:
        break;
    }
    if (v.typeId() == QMetaType::QByteArray) {
        const QByteArray b = v.toByteArray();
        if (b.size() > kMaxBytesValue)
            return QJsonValue::Undefined;
        return QJsonObject {{QStringLiteral("bytes"), QString::fromLatin1(b.toBase64())}};
    }
    if (v.typeId() == QMetaType::QStringList || v.typeId() == QMetaType::QVariantList) {
        QJsonArray a;
        for (const QString& s : v.toStringList())
            a.append(s);
        return a;
    }
    if (rule.kind == Kind::TextList)
        return QJsonValue::Undefined;
    switch (v.typeId()) {
    case QMetaType::QString:
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
        return v.toString();
    default:
        return QJsonValue::Undefined; // sizes, rectangles, ...: not stored by this application
    }
}

/// Checks a JSON value from a file against @p rule; an invalid QVariant when it fails.
QVariant fromJson(const QJsonValue& j, const Rule& rule)
{
    switch (rule.kind) {
    case Kind::Bool:
        return j.isBool() ? QVariant(j.toBool()) : QVariant();
    case Kind::Int: {
        if (!j.isDouble())
            return {};
        const double d = j.toDouble();
        if (d != std::floor(d) || d < rule.min || d > rule.max)
            return {};
        return qint64(d) > INT32_MAX ? QVariant(qint64(d)) : QVariant(int(d));
    }
    case Kind::Double: {
        if (!j.isDouble())
            return {};
        const double d = j.toDouble();
        return std::isfinite(d) && d >= rule.min && d <= rule.max ? QVariant(d) : QVariant();
    }
    case Kind::Choice:
        return j.isString() && rule.choices.contains(j.toString()) ? QVariant(j.toString())
                                                                   : QVariant();
    case Kind::Text:
        return j.isString() && plainText(j.toString(), int(rule.max)) ? QVariant(j.toString())
                                                                      : QVariant();
    case Kind::TextList:
    case Kind::Auto:
        break;
    }
    if (j.isArray()) {
        const QJsonArray a = j.toArray();
        if (a.size() > kMaxListItems)
            return {};
        QStringList list;
        for (const QJsonValue& item : a) {
            if (!item.isString() || !plainText(item.toString(), kMaxListItemLength))
                return {};
            list.append(item.toString());
        }
        return list;
    }
    if (rule.kind == Kind::TextList)
        return {};
    if (j.isObject()) {
        const QJsonObject o = j.toObject();
        if (o.size() != 1 || !o.value(QStringLiteral("bytes")).isString())
            return {};
        const QByteArray text = o.value(QStringLiteral("bytes")).toString().toLatin1();
        if (text.size() > kMaxBytesValue * 4 / 3 + 8)
            return {};
        const auto decoded
            = QByteArray::fromBase64Encoding(text, QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok
            || decoded.decoded.size() > kMaxBytesValue)
            return {};
        return decoded.decoded;
    }
    if (j.isString())
        return plainText(j.toString(), kMaxTextLength) ? QVariant(j.toString()) : QVariant();
    if (j.isBool())
        return j.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (j.isDouble() && std::isfinite(j.toDouble()))
        return QString::number(j.toDouble(), 'g', 15);
    return {};
}

/// The user files of the data folder that travel with the settings.
bool fileNameAllowed(const QString& rel)
{
    static const QRegularExpression keymap(
        QStringLiteral("^keymaps/[A-Za-z0-9][A-Za-z0-9_. -]{0,99}\\.json$"));
    return rel == QLatin1String("guide-presets.json") || rel == QLatin1String("power-bins.json")
        || rel == QLatin1String("export_presets.json") || keymap.match(rel).hasMatch();
}

QString areaOfFile(const QString& rel)
{
    if (rel.startsWith(QLatin1String("keymaps/")))
        return QObject::tr("Custom keyboard profiles");
    if (rel == QLatin1String("guide-presets.json"))
        return QObject::tr("Guide presets");
    if (rel == QLatin1String("power-bins.json"))
        return QObject::tr("Power bins");
    return QObject::tr("Render presets");
}

bool jsonIsValid(const QByteArray& data)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    return err.error == QJsonParseError::NoError && !doc.isNull();
}

bool g_importPending = false;

} // namespace

Locations standardLocations()
{
    Locations l;
    l.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    l.backupDir
        = QFileInfo(QSettings().fileName()).absolutePath() + QStringLiteral("/settings-backups");
    return l;
}

QString areaOfKey(const QString& key)
{
    const QString group = key.section(QLatin1Char('/'), 0, 0);
    if (key == QLatin1String(kLocalControlKey))
        return QObject::tr("Local Control");
    if (group == QLatin1String("keymap") || group == QLatin1String("shortcuts"))
        return QObject::tr("Keyboard profile and shortcuts");
    if (group == QLatin1String("mainWindow") || group == QLatin1String("pages")
        || group == QLatin1String("projectManager") || group == QLatin1String("keyframes")
        || group == QLatin1String("scopesWindow"))
        return QObject::tr("Window and panel layout");
    if (group == QLatin1String("ui") || group == QLatin1String("project"))
        return QObject::tr("Interface and project preferences");
    if (group == QLatin1String("system"))
        return QObject::tr("System preferences");
    if (group == QLatin1String("playback"))
        return QObject::tr("Playback preferences");
    if (group == QLatin1String("edit"))
        return QObject::tr("Editing preferences");
    return QObject::tr("View options and remembered choices");
}

QByteArray exportDocument(const ExportOptions& options, const Locations& locations)
{
    QSettings settings;
    QJsonObject values;
    for (const QString& key : settings.allKeys()) {
        if (!keyIsSane(key))
            continue;
        const Rule* rule = ruleFor(key);
        if (!rule || rule->exclude)
            continue;
        const QJsonValue j = toJson(settings.value(key), *rule);
        if (j.isUndefined() || j.isNull())
            continue;
        if (fromJson(j, *rule).isValid()) // only what this build would also accept back
            values.insert(key, j);
        if (values.size() >= kMaxKeys)
            break;
    }
    QJsonObject files;
    const QDir data(locations.dataDir);
    QStringList candidates {QStringLiteral("guide-presets.json"), QStringLiteral("power-bins.json"),
        QStringLiteral("export_presets.json")};
    const QStringList keymaps = QDir(data.filePath(QStringLiteral("keymaps")))
                                    .entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : keymaps)
        candidates.append(QStringLiteral("keymaps/") + name);
    for (const QString& rel : candidates) {
        if (!fileNameAllowed(rel) || files.size() >= kMaxFiles)
            continue;
        QFile f(data.filePath(rel));
        if (!f.exists() || f.size() > kMaxUserFileBytes || !f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray content = f.readAll();
        if (jsonIsValid(content))
            files.insert(rel, QString::fromUtf8(content));
    }
    QJsonObject header {{QStringLiteral("format"), kFormatNumber},
        {QStringLiteral("app"), QString::fromLatin1(appinfo::name)},
        {QStringLiteral("appVersion"), appinfo::displayVersion()},
        {QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    QJsonObject doc {{QLatin1String(kHeaderKey), header}, {QStringLiteral("settings"), values},
        {QStringLiteral("files"), files}};
    if (options.includeRecentProjects) {
        QJsonArray recent;
        for (const QString& p : settings.value(QLatin1String(kRecentKey)).toStringList()) {
            if (recent.size() < kMaxRecentProjects && plainText(p, kMaxListItemLength))
                recent.append(p);
        }
        doc.insert(QStringLiteral("recentProjects"), recent);
    }
    return QJsonDocument(doc).toJson(QJsonDocument::Indented);
}

bool exportToFile(
    const QString& path, const ExportOptions& options, QString* error, const Locations& locations)
{
    const QByteArray data = exportDocument(options, locations);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2")
                         .arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    return true;
}

bool parse(const QByteArray& data, Parsed* out, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (data.size() > kMaxFileBytes)
        return fail(QObject::tr("The file is too large to be a Kinoklip settings file."));
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QObject::tr("This is not a Kinoklip settings file (it is not valid JSON)."));
    const QJsonObject root = doc.object();
    const QJsonValue headerValue = root.value(QLatin1String(kHeaderKey));
    if (!headerValue.isObject())
        return fail(QObject::tr("This is not a Kinoklip settings file."));
    const QJsonObject header = headerValue.toObject();
    const QJsonValue format = header.value(QStringLiteral("format"));
    if (!format.isDouble() || format.toDouble() < 1
        || format.toDouble() != std::floor(format.toDouble()))
        return fail(QObject::tr("This settings file has no valid format number."));
    if (format.toDouble() > kFormatNumber)
        return fail(QObject::tr("This settings file was made by a newer Kinoklip (format %1; this "
                                "version reads up to format %2). Update Kinoklip to import it.")
                        .arg(int(format.toDouble()))
                        .arg(kFormatNumber));

    Parsed result;
    result.formatNumber = int(format.toDouble());
    result.appVersion = header.value(QStringLiteral("appVersion")).toString().left(64);
    result.created = header.value(QStringLiteral("created")).toString().left(64);

    const QJsonValue settingsValue = root.value(QStringLiteral("settings"));
    if (!settingsValue.isUndefined() && !settingsValue.isObject())
        return fail(QObject::tr("The settings part of this file is damaged."));
    const QJsonObject values = settingsValue.toObject();
    if (values.size() > kMaxKeys)
        return fail(QObject::tr("This settings file has too many entries."));
    QMap<QString, int> perArea;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString key = it.key();
        const Rule* rule = keyIsSane(key) ? ruleFor(key) : nullptr;
        if (!rule || rule->exclude) {
            ++result.ignoredKeys;
            continue;
        }
        const QVariant v = fromJson(it.value(), *rule);
        if (!v.isValid()) {
            result.rejected.append(
                QObject::tr("Setting %1 has a value that is not allowed; skipped.").arg(key));
            continue;
        }
        if (key == QLatin1String(kLocalControlKey) && v.toBool()) {
            result.localControlStaysOff = true; // never switched on by a file
            continue;
        }
        result.settings.insert(key, v);
        ++perArea[areaOfKey(key)];
    }

    const QJsonValue filesValue = root.value(QStringLiteral("files"));
    if (!filesValue.isUndefined() && !filesValue.isObject())
        return fail(QObject::tr("The files part of this settings file is damaged."));
    const QJsonObject files = filesValue.toObject();
    QMap<QString, int> perFileArea;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (result.files.size() >= kMaxFiles) {
            result.rejected.append(
                QObject::tr("Too many files in the settings file; the rest were skipped."));
            break;
        }
        const QString rel = it.key();
        if (!fileNameAllowed(rel)) {
            result.rejected.append(
                QObject::tr("File %1 is not a settings file of Kinoklip; skipped.")
                    .arg(rel.left(80)));
            continue;
        }
        const QByteArray content
            = it.value().isString() ? it.value().toString().toUtf8() : QByteArray();
        if (content.isEmpty() || content.size() > kMaxUserFileBytes || !jsonIsValid(content)) {
            result.rejected.append(
                QObject::tr("File %1 is damaged or too large; skipped.").arg(rel));
            continue;
        }
        result.files.insert(rel, content);
        ++perFileArea[areaOfFile(rel)];
    }

    if (root.contains(QStringLiteral("recentProjects"))) {
        const QJsonValue recent = root.value(QStringLiteral("recentProjects"));
        if (recent.isArray() && recent.toArray().size() <= kMaxRecentProjects) {
            bool good = true;
            QStringList list;
            for (const QJsonValue& item : recent.toArray()) {
                good = good && item.isString() && plainText(item.toString(), kMaxListItemLength);
                list.append(item.toString());
            }
            if (good) {
                result.recentProjects = list;
                result.hasRecentProjects = true;
            }
        }
        if (!result.hasRecentProjects)
            result.rejected.append(QObject::tr("The list of recent projects is damaged; skipped."));
    }

    for (auto it = perArea.cbegin(); it != perArea.cend(); ++it)
        result.summary.append(QObject::tr("%1: %n setting(s)", nullptr, it.value()).arg(it.key()));
    for (auto it = perFileArea.cbegin(); it != perFileArea.cend(); ++it)
        result.summary.append(QObject::tr("%1: %n file(s)", nullptr, it.value()).arg(it.key()));
    if (result.hasRecentProjects)
        result.summary.append(
            QObject::tr("Recent projects: %n project(s) (not applied unless you choose to)",
                nullptr, int(result.recentProjects.size())));
    *out = result;
    return true;
}

bool parseFile(const QString& path, Parsed* out, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2")
                         .arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    if (f.size() > kMaxFileBytes) {
        if (error)
            *error = QObject::tr("The file is too large to be a Kinoklip settings file.");
        return false;
    }
    return parse(f.read(kMaxFileBytes + 1), out, error);
}

ApplyResult apply(const Parsed& parsed, const ApplyOptions& options)
{
    ApplyResult result;
    // 1. The backup of what is about to be replaced (everything, the recent projects too).
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QDir().mkpath(options.locations.backupDir);
    QString backup = QStringLiteral("%1/settings-backup-%2.%3")
                         .arg(options.locations.backupDir, stamp, QLatin1String(kFileSuffix));
    for (int n = 2; QFile::exists(backup); ++n)
        backup = QStringLiteral("%1/settings-backup-%2-%3.%4")
                     .arg(options.locations.backupDir, stamp)
                     .arg(n)
                     .arg(QLatin1String(kFileSuffix));
    QString error;
    if (!exportToFile(backup, ExportOptions {true}, &error, options.locations)) {
        result.error = QObject::tr(
            "The backup of your current settings could not be written, so nothing was changed. %1")
                           .arg(error);
        return result;
    }
    result.backupPath = backup;

    // 2. Files first (they are checked and written atomically one by one).
    const QString dataRoot = QDir(options.locations.dataDir).absolutePath();
    for (auto it = parsed.files.cbegin(); it != parsed.files.cend(); ++it) {
        if (!fileNameAllowed(it.key()))
            continue; // parse() already refuses these; checked again at the point of writing
        const QString target = QDir::cleanPath(dataRoot + QLatin1Char('/') + it.key());
        if (!target.startsWith(dataRoot + QLatin1Char('/')))
            continue;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile f(target);
        if (f.open(QIODevice::WriteOnly) && f.write(it.value()) == it.value().size() && f.commit())
            ++result.filesApplied;
        else
            result.notes.append(QObject::tr("Could not write %1.").arg(it.key()));
    }

    // 3. The settings.
    QSettings settings;
    QSet<QString>
        keymapGroups; // "shortcuts/<profile>/" groups present in the file are replaced whole
    for (auto it = parsed.settings.cbegin(); it != parsed.settings.cend(); ++it) {
        const QStringList parts = it.key().split(QLatin1Char('/'));
        if (parts.size() >= 3
            && (parts.first() == QLatin1String("keymap")
                || parts.first() == QLatin1String("shortcuts")))
            keymapGroups.insert(parts[0] + QLatin1Char('/') + parts[1] + QLatin1Char('/'));
    }
    if (!keymapGroups.isEmpty()) {
        for (const QString& key : settings.allKeys()) {
            for (const QString& group : keymapGroups) {
                if (key.startsWith(group))
                    settings.remove(key);
            }
        }
    }
    for (auto it = parsed.settings.cbegin(); it != parsed.settings.cend(); ++it) {
        settings.setValue(it.key(), it.value());
        ++result.settingsApplied;
    }
    if (parsed.localControlStaysOff)
        result.notes.append(
            QObject::tr("The file asked for Local Control to be on. It stays as it was: switch it "
                        "on yourself in Preferences > Local Control."));
    if (options.includeRecentProjects && parsed.hasRecentProjects) {
        settings.setValue(QLatin1String(kRecentKey), parsed.recentProjects);
        ++result.settingsApplied;
    }
    // A file from 1.2 - 1.4 may still name the removed Vim keyboard preset.
    vimopts::migrateLegacyVimProfile(dataRoot + QStringLiteral("/keymaps"));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        result.error = QObject::tr("The settings could not be written. Your backup is %1.")
                           .arg(QDir::toNativeSeparators(backup));
        return result;
    }
    for (const QString& r : parsed.rejected)
        result.notes.append(r);
    result.ok = true;
    result.restartNeeded = result.settingsApplied > 0 || result.filesApplied > 0;
    if (result.restartNeeded) {
        g_importPending = true;
        emit notifier() -> applied();
    }
    return result;
}

ApplyResult importFile(const QString& path, const ApplyOptions& options)
{
    Parsed parsed;
    ApplyResult result;
    if (!parseFile(path, &parsed, &result.error))
        return result;
    return apply(parsed, options);
}

bool importPending()
{
    return g_importPending;
}

Notifier* notifier()
{
    static Notifier instance;
    return &instance;
}

} // namespace kk::settingstransfer
