// SPDX-License-Identifier: GPL-3.0-or-later
// AutosaveController implementation.
#include "app/AutosaveController.h"

#include "app/AppInfo.h"
#include "core/Preferences.h"
#include "core/Project.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

namespace cw {

namespace {
Q_LOGGING_CATEGORY(lcAutosave, "cw.app.autosave")
constexpr int kMsPerMinute = 60 * 1000;
constexpr int kHashChars = 8; // enough to keep projects with the same name apart
} // namespace

AutosaveController::AutosaveController(Project* project, QObject* parent)
    : QObject(parent)
    , m_project(project)
{
    connect(&m_timer, &QTimer::timeout, this, &AutosaveController::autosaveNow);
    setIntervalMinutes(prefs::autosaveMinutes());
}

QString AutosaveController::autosaveDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("autosave"));
}

QString AutosaveController::autosavePath() const
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9 _.-]"));
    QString base = m_project->name();
    base.replace(unsafe, QStringLiteral("_"));
    const QString identity
        = m_project->filePath().isEmpty() ? QStringLiteral("unsaved") : m_project->filePath();
    const QString hash
        = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1)
                                  .toHex()
                                  .left(kHashChars));
    return QDir(autosaveDirectory())
        .filePath(
            QStringLiteral("%1-%2.%3").arg(base, hash, QLatin1String(appinfo::projectExtension)));
}

void AutosaveController::setIntervalMinutes(int minutes)
{
    if (minutes <= 0) {
        m_timer.stop();
        return;
    }
    m_timer.start(minutes * kMsPerMinute);
}

bool AutosaveController::autosaveNow()
{
    const quint64 timelineRevision = m_project->timeline()->revision();
    const quint64 poolRevision = m_project->mediaPool()->revision();
    if (!m_project->isDirty() || m_project->isEditing()
        || (timelineRevision == m_savedTimelineRevision && poolRevision == m_savedPoolRevision))
        return true;

    QDir().mkpath(autosaveDirectory());
    QString error;
    if (!m_project->saveCopy(autosavePath(), &error)) {
        qCWarning(lcAutosave) << "autosave failed:" << error;
        return false;
    }
    m_savedTimelineRevision = timelineRevision;
    m_savedPoolRevision = poolRevision;
    qCDebug(lcAutosave) << "autosaved to" << autosavePath();
    return true;
}

} // namespace cw
