// SPDX-License-Identifier: GPL-3.0-or-later
// AutosaveController — periodically writes a copy of the modified project (no prompts) and
// finds the copies a crashed session left behind.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QVector>

namespace kk {

class Project;

/// An autosave file left by a session that did not end normally (crash, kill, power loss).
struct RecoveryCandidate
{
    QString autosavePath; ///< the autosave file
    QString documentPath; ///< project file it belongs to; empty for a never-saved project
    QString name; ///< project name at the time of the autosave
    QDateTime savedAt; ///< modification time of the autosave file
};

/// Every prefs::autosaveMinutes() minutes (default 2) writes the project, if it has unsaved
/// changes since the last autosave, to `<AppDataLocation>/autosave/<name>-<hash>.kkproj` with
/// Project::saveCopy() (the document's path and modified flag are untouched). The copy carries
/// an "autosave" object: {documentPath, name, pid}.
///
/// Life cycle of the file: it is deleted when it becomes obsolete, i.e. the project is saved,
/// a different project is loaded/created (after the unsaved-changes prompt), or the window
/// closes normally (discard()). So any autosave that is still there at start-up comes from a
/// session that ended abnormally: findRecoverable() lists those that are newer than their
/// project file, and ProjectController::offerRecovery() offers to restore them.
///
/// Thread-safety: GUI thread only.
class AutosaveController : public QObject
{
    Q_OBJECT
public:
    /// Starts the timer for @p project.
    explicit AutosaveController(Project* project, QObject* parent = nullptr);

    /// Directory holding autosave files (created on demand).
    [[nodiscard]] static QString autosaveDirectory();
    /// Autosave file path for the project's current name/file.
    [[nodiscard]] QString autosavePath() const;
    /// Changes the interval; 0 disables autosave.
    void setIntervalMinutes(int minutes);

    /// Autosaves of ended sessions that hold changes newer than their project file, newest
    /// first. Obsolete ones (project file saved after the autosave) are deleted on the way.
    /// Files written by another running Kinoklip process are skipped.
    [[nodiscard]] static QVector<RecoveryCandidate> findRecoverable();
    /// Deletes the autosave file of @p candidate (the user declined the recovery).
    static void discardCandidate(const RecoveryCandidate& candidate);
    /// After a recovery: treat @p autosavePath as this session's file, so it is removed like
    /// one written by us once it is obsolete.
    void adopt(const QString& autosavePath);

public slots:
    /// Writes the autosave file now if there is something new. Returns false on write errors.
    bool autosaveNow();
    /// Deletes this session's autosave file (normal shutdown, project saved or replaced).
    void discard();

private:
    Project* m_project;
    QTimer m_timer;
    QString m_lastWrittenPath; ///< file written (or adopted) by this session
    quint64 m_savedTimelineRevision = 0;
    quint64 m_savedPoolRevision = 0;
};

} // namespace kk
