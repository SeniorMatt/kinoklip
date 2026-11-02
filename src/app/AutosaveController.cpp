// SPDX-License-Identifier: GPL-3.0-or-later
// AutosaveController implementation.
#include "app/AutosaveController.h"

#include "app/AppInfo.h"
#include "core/Preferences.h"
#include "core/Project.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

namespace kk {

namespace {
Q_LOGGING_CATEGORY(lcAutosave, "kk.app.autosave", QtInfoMsg)
constexpr int kMsPerMinute = 60 * 1000;
constexpr int kHashChars = 8; // enough to keep projects with the same name apart
constexpr char kInfoKey[] = "autosave";
constexpr char kDocumentKey[] = "documentPath";
constexpr char kNameKey[] = "name";
constexpr char kPidKey[] = "pid";

/// True if process @p pid (not us) is alive: its autosave belongs to a running session.
bool otherProcessAlive(qint64 pid)
{
    if (pid <= 0 || pid == QCoreApplication::applicationPid())
        return false;
#ifdef Q_OS_UNIX
    // Signal 0 only checks for existence; EPERM means it exists but belongs to someone else.
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#else
    return false;
#endif
}
} // namespace

AutosaveController::AutosaveController(Project* project, QObject* parent)
    : QObject(parent)
    , m_project(project)
{
    connect(&m_timer, &QTimer::timeout, this, &AutosaveController::autosaveNow);
    // Saved (dirty -> clean) or replaced by New/Open (the unsaved-changes prompt ran first):
    // the autosave holds nothing the project file lacks any more.
    connect(project, &Project::dirtyChanged, this, [this](bool dirty) {
        if (!dirty)
            discard();
    });
    connect(project, &Project::projectReset, this, [this] {
        discard();
        m_savedTimelineRevision = m_savedPoolRevision = 0;
    });
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
    const QString path = autosavePath();
    const QJsonObject info {{QLatin1String(kDocumentKey), m_project->filePath()},
        {QLatin1String(kNameKey), m_project->name()},
        {QLatin1String(kPidKey), QCoreApplication::applicationPid()}};
    if (!m_project->saveCopy(path, &error, info)) {
        qCWarning(lcAutosave) << "autosave failed:" << error;
        return false;
    }
    // A rename or Save As changes the file name: never leave the previous file behind.
    if (!m_lastWrittenPath.isEmpty() && m_lastWrittenPath != path)
        QFile::remove(m_lastWrittenPath);
    m_lastWrittenPath = path;
    m_savedTimelineRevision = timelineRevision;
    m_savedPoolRevision = poolRevision;
    qCDebug(lcAutosave) << "autosaved to" << autosavePath();
    return true;
}

void AutosaveController::discard()
{
    if (!m_lastWrittenPath.isEmpty()) {
        QFile::remove(m_lastWrittenPath);
        m_lastWrittenPath.clear();
    }
    m_savedTimelineRevision = m_savedPoolRevision = 0;
}

void AutosaveController::adopt(const QString& autosavePath)
{
    m_lastWrittenPath = autosavePath;
}

QVector<RecoveryCandidate> AutosaveController::findRecoverable()
{
    QVector<RecoveryCandidate> result;
    const QFileInfoList files
        = QDir(autosaveDirectory())
              .entryInfoList(
                  {QStringLiteral("*.") + QLatin1String(appinfo::projectExtension)}, QDir::Files);
    for (const QFileInfo& file : files) {
        QFile f(file.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        // Files without the info object were written by 0.1.0-alpha.1, which never deleted its
        // autosaves: they cannot be told apart from stale copies, so they are not offered.
        const QJsonObject info = root.value(QLatin1String(kInfoKey)).toObject();
        if (info.isEmpty())
            continue;
        if (otherProcessAlive(info.value(QLatin1String(kPidKey)).toInteger()))
            continue;
        RecoveryCandidate c;
        c.autosavePath = file.absoluteFilePath();
        c.documentPath = info.value(QLatin1String(kDocumentKey)).toString();
        c.name = info.value(QLatin1String(kNameKey)).toString(file.completeBaseName());
        c.savedAt = file.lastModified();
        // Project file written after the autosave (e.g. saved, then the app was killed before
        // the autosave was removed): nothing to recover.
        if (!c.documentPath.isEmpty()) {
            const QFileInfo document(c.documentPath);
            if (document.exists() && document.lastModified() >= c.savedAt) {
                QFile::remove(c.autosavePath);
                continue;
            }
        }
        result.append(c);
    }
    std::sort(
        result.begin(), result.end(), [](const RecoveryCandidate& a, const RecoveryCandidate& b) {
            return a.savedAt > b.savedAt;
        });
    return result;
}

void AutosaveController::discardCandidate(const RecoveryCandidate& candidate)
{
    QFile::remove(candidate.autosavePath);
}

} // namespace kk
