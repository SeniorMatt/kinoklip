// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLock implementation: a QLockFile per project path in the application data folder.
#include "projects/ProjectLock.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>

namespace kk {

namespace {
QString lockFileFor(const QString& projectPath)
{
    const QString absolute = QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath());
    const QByteArray hash
        = QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha1).toHex();
    return ProjectLock::lockDirectory() + QLatin1Char('/') + QString::fromLatin1(hash)
        + QStringLiteral(".lock");
}

/// Locks held by this process: QLockFile cannot tell two holders in one process apart (same
/// pid, so the lock never looks stale), but a second window must still see it as taken.
QMutex& heldMutex()
{
    static QMutex m;
    return m;
}
QSet<QString>& held()
{
    static QSet<QString> set;
    return set;
}
} // namespace

QString ProjectLock::lockDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/locks");
}

std::unique_ptr<ProjectLock> ProjectLock::acquire(const QString& projectPath)
{
    const QString file = lockFileFor(projectPath);
    {
        QMutexLocker locker(&heldMutex());
        if (held().contains(file))
            return nullptr;
    }
    QDir().mkpath(lockDirectory());
    auto lock = std::make_unique<QLockFile>(file);
    lock->setStaleLockTime(0); // only a dead owner makes it stale, never its age
    // A lock left by a crashed process (its pid no longer runs) is removed by tryLock().
    if (!lock->tryLock(0))
        return nullptr;
    std::unique_ptr<ProjectLock> result(new ProjectLock);
    result->m_projectPath = QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath());
    result->m_lock = std::move(lock);
    QMutexLocker locker(&heldMutex());
    held().insert(file);
    return result;
}

bool ProjectLock::isLocked(const QString& projectPath)
{
    const QString file = lockFileFor(projectPath);
    {
        QMutexLocker locker(&heldMutex());
        if (held().contains(file))
            return true;
    }
    QLockFile probe(file);
    probe.setStaleLockTime(0);
    if (!probe.tryLock(0))
        return probe.error() == QLockFile::LockFailedError;
    probe.unlock();
    return false;
}

ProjectLock::~ProjectLock()
{
    if (!m_lock)
        return;
    const QString file = m_lock->fileName();
    m_lock->unlock();
    QMutexLocker locker(&heldMutex());
    held().remove(file);
}

} // namespace kk
