// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimOptions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>

namespace kk::vimopts {

bool enabled()
{
    return QSettings().value(QLatin1String(kEnabledKey), false).toBool();
}

void setEnabled(bool on)
{
    QSettings().setValue(QLatin1String(kEnabledKey), on);
}

QString leader()
{
    const QString token = QSettings().value(QLatin1String(kLeaderKey)).toString();
    return token.isEmpty() ? QStringLiteral("<Space>") : token;
}

void setLeader(const QString& token)
{
    QSettings().setValue(QLatin1String(kLeaderKey), token);
}

int timeoutMs()
{
    const int ms = QSettings().value(QLatin1String(kTimeoutKey), kDefaultTimeoutMs).toInt();
    return std::clamp(ms, kMinTimeoutMs, kMaxTimeoutMs);
}

void setTimeoutMs(int ms)
{
    QSettings().setValue(QLatin1String(kTimeoutKey), std::clamp(ms, kMinTimeoutMs, kMaxTimeoutMs));
}

bool physicalKeys()
{
    return QSettings().value(QLatin1String(kPhysicalKeysKey), true).toBool();
}

void setPhysicalKeys(bool on)
{
    QSettings().setValue(QLatin1String(kPhysicalKeysKey), on);
}

bool migrateLegacyVimProfile(const QString& userKeymapDir, const QString& shortcutsGroup)
{
    static const QString legacy = QStringLiteral("vim");
    static const QString replacement = QStringLiteral("resolve-windows");
    bool migrated = false;
    QSettings settings;

    // User profile files based on the removed preset.
    QString activeUserBase;
    const QString active = settings.value(QStringLiteral("keymap/profile")).toString();
    if (!userKeymapDir.isEmpty()) {
        const QFileInfoList files
            = QDir(userKeymapDir).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
        for (const QFileInfo& fi : files) {
            QFile in(fi.filePath());
            if (!in.open(QIODevice::ReadOnly))
                continue;
            QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
            in.close();
            if (root.value(QLatin1String("basedOn")).toString() != legacy)
                continue;
            if (QStringLiteral("user-") + fi.completeBaseName() == active)
                activeUserBase = legacy;
            root.insert(QLatin1String("basedOn"), replacement);
            QSaveFile out(fi.filePath());
            if (out.open(QIODevice::WriteOnly)) {
                out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                out.commit();
                migrated = true;
            }
        }
    }
    if (activeUserBase == legacy)
        settings.setValue(QLatin1String(kEnabledKey), true);

    if (active == legacy) {
        settings.setValue(QLatin1String(kEnabledKey), true);
        settings.setValue(QStringLiteral("keymap/profile"), replacement);
        settings.setValue(QLatin1String(kMigrationNoticeKey), true);
        migrated = true;
    }

    // The per-key changes made under the old preset.
    settings.beginGroup(shortcutsGroup + QLatin1Char('/') + legacy);
    const QStringList keys = settings.childKeys();
    QHash<QString, QVariant> values;
    for (const QString& key : keys)
        values.insert(key, settings.value(key));
    settings.remove(QString());
    settings.endGroup();
    if (!values.isEmpty()) {
        settings.beginGroup(shortcutsGroup + QLatin1Char('/') + replacement);
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!settings.contains(it.key()))
                settings.setValue(it.key(), it.value());
        }
        settings.endGroup();
        migrated = true;
    }
    return migrated;
}

} // namespace kk::vimopts
