// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController — File menu behaviour: new, open, save, save as, import, "save changes?".
#pragma once

#include <QObject>
#include <QStringList>

class QMenu;
class QWidget;

namespace kk {

struct AppContext;
class AutosaveController;

/// Implements the document commands (file.new, file.open, file.save, file.save_as,
/// file.import_media) on top of Project, with the usual dialogs. Connects itself to the
/// registry actions on construction.
class ProjectController : public QObject
{
    Q_OBJECT
public:
    /// @p window parents the dialogs.
    ProjectController(AppContext& ctx, QWidget* window);

    /// If the project has unsaved changes, asks Save / Discard / Cancel.
    /// Returns true if the caller may proceed (saved or discarded).
    bool maybeSave();
    /// Opens @p path (after maybeSave() if @p askToSave). Shows an error dialog on failure.
    bool openFile(const QString& path, bool askToSave = true);

    /// Autosave of this window (for crash recovery); may stay null (tests, screenshots).
    void setAutosave(AutosaveController* autosave) { m_autosave = autosave; }
    /// Start-up crash recovery: for each autosave left by a session that ended abnormally
    /// (AutosaveController::findRecoverable()), asks Restore / Discard / Not Now. With
    /// @p onlyDocument set (project given on the command line), only that project's autosave
    /// is offered. Returns true if a project was restored (the caller then skips opening).
    bool offerRecovery(const QString& onlyDocument = {});

    /// File > Open Recent: fills @p menu with the recent projects each time it opens.
    void attachRecentMenu(QMenu* menu);
    /// Recent project files, most recent first (at most kMaxRecentProjects).
    [[nodiscard]] static QStringList recentProjects();
    /// Puts @p path at the top of the recent projects list.
    static void addRecentProject(const QString& path);
    /// Empties the recent projects list.
    static void clearRecentProjects();
    /// Length of the recent projects list.
    static constexpr int kMaxRecentProjects = 10;

public slots:
    /// File ▸ New Project.
    void newProject();
    /// File ▸ Open Project… (file dialog).
    void open();
    /// File ▸ Save Project (Save As if the project has no file yet). Returns true on success.
    bool save();
    /// File ▸ Save Project As… Returns true on success.
    bool saveAs();
    /// File ▸ Import Media… (multi-file dialog → MediaPool).
    void importMedia();

private:
    void fillRecentMenu(QMenu* menu);

    AppContext& m_ctx;
    QWidget* m_window;
    AutosaveController* m_autosave = nullptr;
};

} // namespace kk
