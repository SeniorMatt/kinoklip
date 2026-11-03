// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLock — one writer per project file: the window that edits a single-file project
// holds its lock, so a second window or process opens it read-only instead of writing into
// the same container (docs/single-file-project.md, "One writer per file").
#pragma once

#include <QString>

#include <memory>

class QLockFile;

namespace kk {

class ProjectLock
{
public:
    /// Tries to take the lock of the project file @p projectPath. Null if another window or
    /// process holds it (a lock left by a crashed process is taken over).
    static std::unique_ptr<ProjectLock> acquire(const QString& projectPath);
    /// True if some window or process holds the lock of @p projectPath right now.
    [[nodiscard]] static bool isLocked(const QString& projectPath);
    ~ProjectLock();

    [[nodiscard]] QString projectPath() const { return m_projectPath; }

    /// Folder of the lock files (`<AppDataLocation>/locks`): never next to the project, so
    /// nothing appears in the user's folders.
    [[nodiscard]] static QString lockDirectory();

private:
    ProjectLock() = default;
    QString m_projectPath;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace kk
