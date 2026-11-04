// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController — File menu behaviour: new, open, save, save as, import, "save changes?",
// and single-file projects (ProjectPackaging.cpp: the open container, Pack / Unpack / Compact /
// Verify, copying imported media in).
#pragma once

#include "ui/common/MediaStore.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <memory>

class QMenu;
class QWidget;

namespace kk {

struct AppContext;
class AutosaveController;
class ProjectLibrary;
class ProjectLock;
class MediaPoolPanel;
class ProjectManagerWindow;

namespace pack {
class ProjectPackage;
}

/// Implements the document commands (file.new, file.open, file.save, file.save_as,
/// file.import_media, file.pack_project, file.unpack_project, file.compact_project,
/// file.verify_project) on top of Project, with the usual dialogs. Connects itself to the
/// registry actions on construction.
///
/// **Single-file projects** (`.kinoklip`, src/pack): while one is open the controller holds
/// its ProjectPackage (writable) and its ProjectLock for the project's lifetime; the document
/// is loaded and saved through the container, autosaves stay outside it. A file that is
/// locked by another window or process, or not writable, opens read-only (Save goes to Save
/// As). The controller is the AppContext's MediaStore: imports into a single-file project are
/// copied into the file first.
class ProjectController : public QObject, public MediaStore
{
    Q_OBJECT
public:
    /// @p window parents the dialogs.
    ProjectController(AppContext& ctx, QWidget* window);
    /// Closes the Project Manager before the library it shows goes away; releases the open
    /// single-file project.
    ~ProjectController() override;

    /// If the project has unsaved changes, asks Save / Discard / Cancel (dialogs parented to
    /// @p dialogParent, default the main window). Returns true if the caller may proceed
    /// (saved or discarded).
    bool maybeSave(QWidget* dialogParent = nullptr);
    /// Opens @p path (after maybeSave() if @p askToSave), a linked `.kkproj` or a single-file
    /// `.kinoklip`. Shows an error dialog on failure.
    bool openFile(const QString& path, bool askToSave = true, QWidget* dialogParent = nullptr);
    /// Start-up "reopen the last project": opens the most recent project that still exists.
    /// Returns false (nothing opened) if there is none or it fails to load.
    bool openLastProject();

    /// The project library at prefs::projectLibraryDirectory() (created on first use, and
    /// again when the preference changed).
    ProjectLibrary* projectLibrary();
    /// The Project Manager window, or null before it was first shown.
    [[nodiscard]] ProjectManagerWindow* projectManager() const;

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
    /// The Project Manager renamed or moved @p from to @p to: recent entries follow.
    void followMovedProject(const QString& from, const QString& to);
    /// Empties the recent projects list.
    static void clearRecentProjects();
    /// Length of the recent projects list.
    static constexpr int kMaxRecentProjects = 10;

    // ---- Single-file projects (ProjectPackaging.cpp) ----

    /// True while the open project is a single-file project.
    [[nodiscard]] bool isSingleFile() const;
    /// True if the open project cannot be saved in place (locked elsewhere, read-only file).
    [[nodiscard]] bool isReadOnly() const { return m_readOnly; }
    /// The open single-file project's container (null for a linked project).
    [[nodiscard]] pack::ProjectPackage* package() const { return m_package.get(); }

    /// Closes the open container's file (the lock stays held), e.g. before a rewrite of the
    /// file (Compact, Repair). package() is null until reopenPackage().
    void releasePackage();
    /// Opens the container of the open single-file project again after releasePackage();
    /// false (and read-only) if that fails.
    bool reopenPackage(QString* error = nullptr);

    // MediaStore
    [[nodiscard]] bool storesMedia() const override;
    [[nodiscard]] QString containerPath() const override;
    bool storeMedia(QStringList& paths, QWidget* parent) override;

    /// Save As into a single-file project at @p target (Pack of the current state, also from
    /// a never-saved project); the new file becomes the open project. False if cancelled or
    /// failed (the user was told).
    bool saveAsSingleFile(const QString& target);
    /// Save As into a linked project @p kkprojTarget (Unpack: the media are written to
    /// `<target folder>/media/...`); the linked project becomes the open project.
    bool saveAsLinked(const QString& kkprojTarget);

public slots:
    /// File > Project Manager (Shift+1), and at start-up: shows the Project Manager over the
    /// window (window-modal, never blocking the caller). Opening a project there closes it.
    void showProjectManager();
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
    /// File ▸ Import Media Folder… (folder chooser, options, folder → bins in the MediaPool).
    void importMediaFolder();
    /// File > Import Subtitle: .srt / .vtt files into the media pool.
    void importSubtitle();
    /// File > Pack Project...: saves, then copies the project and every media file it uses
    /// into a new single-file project (progress, Cancel); reports media left linked and offers
    /// to open the new file.
    void packProject();
    /// File > Unpack Project...: writes the media of the open single-file project into a
    /// chosen folder and continues there as a linked project.
    void unpackProject();
    /// File > Compact Project...: saves, shows the current and the estimated size and the
    /// unused media, then rewrites the file without them and without old saves.
    void compactProject();
    /// File > Verify Project File: checks every byte of the single-file project; offers
    /// Repair when the file is damaged after a crash.
    void verifyProject();

signals:
    /// The open project became single-file / linked or read-only / writable.
    void storageChanged();

private:
    void fillRecentMenu(QMenu* menu);
    /// Opens the single-file project @p path: lock, container, document.
    bool openContainer(const QString& path, QString* error, QWidget* dialogParent);
    /// Holds the container of @p path again without loading its document (after recovery).
    bool attachContainer(const QString& path, QString* error);
    /// Writes the current state into the open container.
    bool saveContainer(QString* error);
    /// Releases the container and its lock (a linked or new project is open now).
    void releaseContainer();
    /// Enables the File menu's single-file commands for the open project's kind.
    void updatePackageActions();
    /// Shows @p text as a non-blocking information box over @p parent (notices).
    void notify(QWidget* parent, const QString& title, const QString& text);
    /// The media pool on the page shown (any when none is visible; null without one).
    [[nodiscard]] MediaPoolPanel* shownMediaPool() const;

    AppContext& m_ctx;
    QWidget* m_window;
    AutosaveController* m_autosave = nullptr;
    ProjectLibrary* m_library = nullptr; ///< owned (child)
    QPointer<ProjectManagerWindow> m_manager; ///< child widget of m_window
    std::unique_ptr<pack::ProjectPackage> m_package; ///< open single-file project, or null
    std::unique_ptr<ProjectLock> m_lock; ///< held while m_package is writable
    bool m_readOnly = false;
};

} // namespace kk
