// SPDX-License-Identifier: GPL-3.0-or-later
// ActionRegistry keymap profiles: built-in profiles (:/keymaps/*.json), user profiles in
// <AppData>/keymaps, switching, duplicate / rename / delete, import / export.
#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace kk {

namespace {

Q_LOGGING_CATEGORY(lcKeymaps, "kk.ui.keymaps", QtInfoMsg)

/// Resource folder of the built-in profiles (resources/keymaps/<id>.json).
const QString kBuiltInDir = QStringLiteral(":/keymaps");
/// QSettings key of the active profile id.
const QString kActiveKey = QStringLiteral("keymap/profile");
/// Prefix of user profile ids, so they can never collide with a built-in id.
const QString kUserPrefix = QStringLiteral("user-");

QStringList keyStrings(const QList<QKeySequence>& keys)
{
    QStringList list;
    for (const QKeySequence& k : keys)
        list.append(k.toString(QKeySequence::PortableText));
    return list;
}

} // namespace

QString ActionRegistry::userProfileDirectory() const
{
    if (!m_userDir.isEmpty())
        return m_userDir;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/keymaps");
}

void ActionRegistry::setUserProfileDirectory(const QString& dir)
{
    m_userDir = dir;
    m_profilesScanned = false;
    scanProfiles();
}

void ActionRegistry::scanProfiles()
{
    m_profilesScanned = true;
    m_builtIns.clear();
    m_userProfiles.clear();
    const QFileInfoList builtIns
        = QDir(kBuiltInDir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : builtIns) {
        KeyMap ignored;
        QString name;
        if (!readKeymap(fi.filePath(), ignored, nullptr, &name)) {
            qCWarning(lcKeymaps) << "broken built-in keymap" << fi.filePath();
            continue;
        }
        KeymapProfile p;
        p.id = fi.completeBaseName();
        p.name = name.isEmpty() ? p.id : name;
        p.builtIn = true;
        m_builtIns.append(p);
    }
    const QFileInfoList users
        = QDir(userProfileDirectory()).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo& fi : users) {
        KeyMap ignored;
        QString name;
        QString basedOn;
        if (!readKeymap(fi.filePath(), ignored, nullptr, &name, &basedOn)) {
            qCWarning(lcKeymaps) << "skipping unreadable keymap profile" << fi.filePath();
            continue;
        }
        UserProfile u;
        u.info.id = kUserPrefix + fi.completeBaseName();
        u.info.name = name.isEmpty() ? fi.completeBaseName() : name;
        u.info.basedOn = basedOn;
        u.path = fi.filePath();
        m_userProfiles.append(u);
    }
    auto byName = [](const auto& a, const auto& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    };
    std::sort(m_builtIns.begin(), m_builtIns.end(), byName);
    std::sort(m_userProfiles.begin(), m_userProfiles.end(),
        [&](const UserProfile& a, const UserProfile& b) { return byName(a.info, b.info); });
}

QVector<ActionRegistry::KeymapProfile> ActionRegistry::profiles() const
{
    if (!m_profilesScanned)
        const_cast<ActionRegistry*>(this)->scanProfiles();
    QVector<KeymapProfile> list = m_builtIns;
    for (const UserProfile& u : m_userProfiles)
        list.append(u.info);
    return list;
}

std::optional<ActionRegistry::KeymapProfile> ActionRegistry::profile(const QString& id) const
{
    for (const KeymapProfile& p : profiles()) {
        if (p.id == id)
            return p;
    }
    return std::nullopt;
}

const ActionRegistry::UserProfile* ActionRegistry::userProfile(const QString& id) const
{
    for (const UserProfile& u : m_userProfiles) {
        if (u.info.id == id)
            return &u;
    }
    return nullptr;
}

QString ActionRegistry::builtInPath(const QString& id) const
{
    return kBuiltInDir + QLatin1Char('/') + id + QStringLiteral(".json");
}

bool ActionRegistry::loadProfileMaps(
    const QString& id, KeyMap& base, KeyMap& overrides, QString* error) const
{
    const auto info = profile(id);
    if (!info) {
        if (error)
            *error = tr("Unknown keyboard profile %1").arg(id);
        return false;
    }
    if (info->builtIn) {
        if (!readKeymap(builtInPath(id), base, error))
            return false;
        // Overrides of a read-only profile live in QSettings. Stored as one string
        // ("Ctrl+B; F5") because INI files cannot hold empty lists; an empty string means
        // "explicitly unbound".
        QSettings settings;
        settings.beginGroup(m_settingsGroup + QLatin1Char('/') + id);
        for (const QString& key : settings.childKeys()) {
            QList<QKeySequence> keys = QKeySequence::listFromString(
                settings.value(key).toString(), QKeySequence::PortableText);
            keys.removeIf([](const QKeySequence& k) { return k.isEmpty(); });
            overrides.insert(key, keys);
        }
        return true;
    }
    const UserProfile* user = userProfile(id);
    QString basedOn = user->info.basedOn;
    if (!profile(basedOn) || !profile(basedOn)->builtIn)
        basedOn = QLatin1String(kDefaultProfile); // the base was removed from a later version
    if (!readKeymap(builtInPath(basedOn), base, error))
        return false;
    return readKeymap(user->path, overrides, error);
}

void ActionRegistry::persistOverrides()
{
    if (m_batch > 0)
        return;
    const auto info = profile(m_activeProfile);
    if (!info)
        return;
    if (info->builtIn) {
        QSettings settings;
        settings.beginGroup(m_settingsGroup + QLatin1Char('/') + m_activeProfile);
        settings.remove(QString()); // the whole group: rewritten below
        for (auto it = m_overrides.cbegin(); it != m_overrides.cend(); ++it)
            settings.setValue(
                it.key(), QKeySequence::listToString(it.value(), QKeySequence::PortableText));
        return;
    }
    const UserProfile* user = userProfile(m_activeProfile);
    QString error;
    if (!writeKeymap(user->path, m_overrides, user->info.name, user->info.basedOn, &error))
        qCWarning(lcKeymaps) << error;
}

void ActionRegistry::migrateLegacyOverrides()
{
    // Before profiles existed overrides were stored directly in the settings group; they were
    // made against the default keymap, so they become overrides of the default profile.
    QSettings settings;
    settings.beginGroup(m_settingsGroup);
    const QStringList legacy = settings.childKeys();
    if (legacy.isEmpty())
        return;
    QHash<QString, QVariant> values;
    for (const QString& key : legacy)
        values.insert(key, settings.value(key));
    for (const QString& key : legacy)
        settings.remove(key);
    settings.beginGroup(QLatin1String(kDefaultProfile));
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!settings.contains(it.key()))
            settings.setValue(it.key(), it.value());
    }
}

bool ActionRegistry::setActiveProfile(const QString& id, QString* error)
{
    KeyMap base;
    KeyMap overrides;
    if (!loadProfileMaps(id, base, overrides, error))
        return false;
    const bool changed = id != m_activeProfile;
    m_activeProfile = id;
    m_defaults = base;
    m_overrides = overrides;
    QSettings().setValue(kActiveKey, id);
    applyAll();
    if (changed)
        emit activeProfileChanged(id);
    return true;
}

QString ActionRegistry::uniqueUserId(const QString& name) const
{
    // File-name safe slug of the name; numbered when taken.
    QString slug = name.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (slug.isEmpty())
        slug = QStringLiteral("profile");
    QString id = kUserPrefix + slug;
    for (int n = 2; profile(id)
         || QFileInfo::exists(userProfileDirectory() + QLatin1Char('/') + id.mid(kUserPrefix.size())
             + QStringLiteral(".json"));
         ++n)
        id = kUserPrefix + slug + QLatin1Char('-') + QString::number(n);
    return id;
}

QString ActionRegistry::duplicateProfile(
    const QString& sourceId, const QString& name, QString* error)
{
    KeyMap base;
    KeyMap overrides;
    if (!loadProfileMaps(sourceId, base, overrides, error))
        return {};
    if (sourceId == m_activeProfile)
        overrides = m_overrides; // includes changes not written yet
    const auto source = profile(sourceId);
    const QString basedOn = source->builtIn ? sourceId : source->basedOn;
    const QString cleanName
        = name.trimmed().isEmpty() ? tr("%1 (copy)").arg(source->name) : name.trimmed();
    if (!QDir().mkpath(userProfileDirectory())) {
        if (error)
            *error = tr("Cannot create %1").arg(userProfileDirectory());
        return {};
    }
    const QString id = uniqueUserId(cleanName);
    const QString path
        = userProfileDirectory() + QLatin1Char('/') + id.mid(kUserPrefix.size()) + ".json";
    if (!writeKeymap(path, overrides, cleanName, basedOn, error))
        return {};
    scanProfiles();
    emit profilesChanged();
    return id;
}

bool ActionRegistry::renameProfile(const QString& id, const QString& name, QString* error)
{
    const UserProfile* user = userProfile(id);
    if (!user || name.trimmed().isEmpty()) {
        if (error)
            *error = user ? tr("The name is empty.") : tr("Built-in profiles cannot be renamed.");
        return false;
    }
    KeyMap overrides;
    if (id == m_activeProfile)
        overrides = m_overrides;
    else if (!readKeymap(user->path, overrides, error))
        return false;
    // The id (file name) stays, so the active-profile setting keeps pointing at it.
    if (!writeKeymap(user->path, overrides, name.trimmed(), user->info.basedOn, error))
        return false;
    scanProfiles();
    emit profilesChanged();
    return true;
}

bool ActionRegistry::deleteProfile(const QString& id, QString* error)
{
    const UserProfile* user = userProfile(id);
    if (!user) {
        if (error)
            *error = tr("Built-in profiles cannot be deleted.");
        return false;
    }
    const QString basedOn = user->info.basedOn;
    if (!QFile::remove(user->path)) {
        if (error)
            *error = tr("Cannot delete %1").arg(user->path);
        return false;
    }
    scanProfiles();
    if (id == m_activeProfile)
        setActiveProfile(profile(basedOn) ? basedOn : QLatin1String(kDefaultProfile));
    emit profilesChanged();
    return true;
}

QString ActionRegistry::importProfile(const QString& path, const QString& name, QString* error)
{
    KeyMap keys;
    QString fileName;
    QString basedOn;
    if (!readKeymap(path, keys, error, &fileName, &basedOn))
        return {};
    if (!profile(basedOn) || !profile(basedOn)->builtIn)
        basedOn = QLatin1String(kDefaultProfile);
    KeyMap base;
    if (!readKeymap(builtInPath(basedOn), base, error))
        return {};
    // Store only what differs from the base, so later fixes of the base reach the profile.
    KeyMap overrides;
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        if (keyStrings(it.value()) != keyStrings(base.value(it.key())))
            overrides.insert(it.key(), it.value());
    }
    QString cleanName = name.trimmed();
    if (cleanName.isEmpty())
        cleanName = fileName.isEmpty() ? QFileInfo(path).completeBaseName() : fileName;
    if (!QDir().mkpath(userProfileDirectory())) {
        if (error)
            *error = tr("Cannot create %1").arg(userProfileDirectory());
        return {};
    }
    const QString id = uniqueUserId(cleanName);
    const QString target
        = userProfileDirectory() + QLatin1Char('/') + id.mid(kUserPrefix.size()) + ".json";
    if (!writeKeymap(target, overrides, cleanName, basedOn, error))
        return {};
    scanProfiles();
    emit profilesChanged();
    return id;
}

bool ActionRegistry::exportProfile(const QString& id, const QString& path, QString* error) const
{
    KeyMap base;
    KeyMap overrides;
    if (!loadProfileMaps(id, base, overrides, error))
        return false;
    if (id == m_activeProfile)
        overrides = m_overrides;
    KeyMap all = base;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        all.insert(it.key(), it.value());
    const auto info = profile(id);
    return writeKeymap(path, all, info->name, info->builtIn ? id : info->basedOn, error);
}

bool ActionRegistry::isProfileModified(const QString& id) const
{
    if (id == m_activeProfile)
        return !m_overrides.isEmpty();
    KeyMap base;
    KeyMap overrides;
    if (!loadProfileMaps(id, base, overrides, nullptr))
        return false;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        if (keyStrings(it.value()) != keyStrings(base.value(it.key())))
            return true;
    }
    return false;
}

} // namespace kk
