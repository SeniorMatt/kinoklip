// SPDX-License-Identifier: GPL-3.0-or-later
// PowerBins implementation: JSON file next to the application settings, saved on every change.
#include "core/PowerBins.h"

#include "core/Logging.h"
#include "core/Serialization.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace kk {

namespace {
constexpr int kPowerBinsVersion = 1; ///< "formatVersion" of power-bins.json
}

PowerBins::PowerBins(const QString& filePath, QObject* parent)
    : QObject(parent)
    , m_pool(new MediaPool(this))
    , m_filePath(filePath.isEmpty() ? defaultFilePath() : filePath)
{
    load();
    connect(m_pool, &MediaPool::changed, this, [this] {
        if (!m_loading)
            save();
    });
}

QString PowerBins::defaultFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("power-bins.json"));
}

bool PowerBins::load(QString* error)
{
    m_loading = true;
    MediaPoolState state;
    bool ok = true;
    QFile file(m_filePath);
    if (file.exists()) {
        QJsonParseError parseError;
        const QJsonDocument doc = file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll(), &parseError)
            : QJsonDocument();
        if (!doc.isObject()) {
            ok = false;
            if (error)
                *error = tr("%1 is not a valid power bins file").arg(m_filePath);
            // Keep the damaged file: the next change overwrites the original.
            const QString backup = m_filePath + QStringLiteral(".bak");
            QFile::remove(backup);
            QFile::copy(m_filePath, backup);
            qCWarning(lcCore) << "power bins unreadable:" << m_filePath << "copied to" << backup;
        } else {
            // Paths are absolute; the folder of the file only resolves relative fallbacks.
            state = serialization::mediaPoolFromJson(
                doc.object().value(QLatin1String("mediaPool")).toObject(),
                QFileInfo(m_filePath).absoluteDir());
        }
    }
    m_pool->setState(std::move(state));
    m_loading = false;
    return ok;
}

bool PowerBins::save(QString* error) const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QJsonObject root;
    root[QLatin1String("formatVersion")] = kPowerBinsVersion;
    root[QLatin1String("mediaPool")]
        = serialization::mediaPoolToJson(m_pool->state(), QFileInfo(m_filePath).absoluteDir());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0
        || !file.commit()) {
        if (error)
            *error = file.errorString();
        qCWarning(lcCore) << "power bins not saved:" << m_filePath << file.errorString();
        return false;
    }
    return true;
}

} // namespace kk
