// SPDX-License-Identifier: GPL-3.0-or-later
// AutosaveController — periodically writes a copy of the modified project (no prompts).
#pragma once

#include <QObject>
#include <QTimer>

namespace cw {

class Project;

/// Every prefs::autosaveMinutes() minutes (default 2) writes the project, if it has unsaved
/// changes since the last autosave, to `<AppDataLocation>/autosave/<name>-<hash>.cwproj` with
/// Project::saveCopy() (the document's path and modified flag are untouched).
/// TODO(app): offer to restore autosaves after a crash.
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

public slots:
    /// Writes the autosave file now if there is something new. Returns false on write errors.
    bool autosaveNow();

private:
    Project* m_project;
    QTimer m_timer;
    quint64 m_savedTimelineRevision = 0;
    quint64 m_savedPoolRevision = 0;
};

} // namespace cw
