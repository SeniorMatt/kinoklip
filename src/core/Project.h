// SPDX-License-Identifier: GPL-3.0-or-later
// Project — owns the media pool, the timeline, the undo stack and project-level settings.
#pragma once

#include "core/MediaPool.h"
#include "core/Timeline.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class QUndoStack;

namespace kk {

/// Project-wide settings. Resolution and frame rate are applied to the timeline.
struct ProjectSettings
{
    QString name = QStringLiteral("Untitled Project");
    int width = 1920;
    int height = 1080;
    Rational fps {25, 1};

    friend bool operator==(const ProjectSettings&, const ProjectSettings&) = default;
};

/// The document: media pool + timeline + undo history + settings + file path.
///
/// Undo is snapshot based: beginEdit() remembers the timeline and media pool state, endEdit()
/// pushes one QUndoCommand holding the before/after copies (nothing is pushed if nothing
/// changed). Edits nest: only the outermost begin/end pair creates a command.
///
/// Thread-safety: GUI thread only.
class Project : public QObject
{
    Q_OBJECT
public:
    /// Creates a new, empty project (same as newProject()).
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    /// The media pool (owned by the project, never null).
    [[nodiscard]] MediaPool* mediaPool() const { return m_mediaPool; }
    /// The timeline (owned by the project, never null).
    [[nodiscard]] Timeline* timeline() const { return m_timeline; }
    /// The undo stack (owned by the project, never null). Prefer undo()/redo() below, which
    /// refuse to run while an edit is open.
    [[nodiscard]] QUndoStack* undoStack() const { return m_undoStack; }

    // ---- Settings & document state ----

    /// Current settings (name, resolution, frame rate).
    [[nodiscard]] ProjectSettings settings() const;
    /// Applies new settings; resolution/fps changes are undoable timeline edits.
    void setSettings(const ProjectSettings& settings);
    /// Project display name.
    [[nodiscard]] QString name() const { return m_name; }
    /// Renames the project (marks it modified).
    void setName(const QString& name);
    /// Path of the project file, empty if never saved.
    [[nodiscard]] QString filePath() const { return m_filePath; }
    /// True if there are unsaved changes.
    [[nodiscard]] bool isDirty() const;

    /// Resets to an empty project with default settings and V1/V2/A1/A2 tracks.
    void newProject();
    /// Writes the project to @p path (JSON, see core/readme.md) and makes it the current file.
    /// Returns false and sets @p error on failure.
    bool save(const QString& path, QString* error = nullptr);
    /// Writes a copy to @p path without changing filePath() or the modified state (autosave).
    /// @p extra is stored as the top-level "autosave" object (ignored by load()).
    bool saveCopy(
        const QString& path, QString* error = nullptr, const QJsonObject& extra = {}) const;
    /// Loads a project file, replacing the current contents and clearing the undo history.
    /// On failure the current project is unchanged and @p error describes the problem.
    bool load(const QString& path, QString* error = nullptr);
    /// Crash recovery: loads the autosave file @p autosavePath as if it were the document
    /// @p documentPath (empty = a never-saved project). Relative media paths resolve against
    /// the document's folder, filePath() becomes @p documentPath and the project is marked
    /// modified, so the user decides whether to save the recovered state over the file.
    bool loadRecovered(
        const QString& autosavePath, const QString& documentPath, QString* error = nullptr);

    // ---- Editing protocol ----

    /// Starts an undoable edit labelled @p label (shown in Edit ▸ Undo). Nesting-safe.
    void beginEdit(const QString& label);
    /// Finishes the edit; the outermost call pushes one undo command if anything changed and
    /// notifies the timeline.
    void endEdit();
    /// Abandons the edit. When the outermost edit closes (by endEdit() or cancelEdit()), the
    /// state from the outermost beginEdit() is restored and nothing is pushed.
    void cancelEdit();
    /// True while an edit is open.
    [[nodiscard]] bool isEditing() const { return m_editDepth > 0; }

    /// Undoes the last command (ignored while an edit is open).
    void undo();
    /// Redoes the next command (ignored while an edit is open).
    void redo();

    /// Imports files into the media pool as one undoable step. Returns ids + warnings.
    ImportResult importMedia(const QStringList& paths, const QString& binId = {});

    /// Restores a timeline + media pool state (used by undo commands; not undoable itself).
    void restoreState(const TimelineData& timeline, const MediaPoolState& pool);

signals:
    /// The modified state changed.
    void dirtyChanged(bool dirty);
    /// The project name changed.
    void nameChanged(const QString& name);
    /// The file path changed (save as, load, new).
    void filePathChanged(const QString& path);
    /// The whole project was replaced (new or load); views should rebuild themselves.
    void projectReset();

private:
    bool loadFrom(const QString& path, const QString& documentPath, QString* error);
    void setFilePath(const QString& path);
    void setSettingsDirty(bool dirty);
    void emitDirtyIfChanged();

    MediaPool* m_mediaPool = nullptr;
    Timeline* m_timeline = nullptr;
    QUndoStack* m_undoStack = nullptr;
    QString m_name;
    QString m_filePath;
    bool m_settingsDirty = false; ///< non-undoable changes (rename) since the last save
    bool m_lastDirty = false;

    int m_editDepth = 0;
    bool m_editCancelled = false;
    QString m_editLabel;
    TimelineData m_editTimelineBefore;
    MediaPoolState m_editPoolBefore;
};

} // namespace kk
