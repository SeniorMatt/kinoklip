// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController — File menu behaviour: new, open, save, save as, import, "save changes?".
#pragma once

#include <QObject>

class QWidget;

namespace kk {

struct AppContext;

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
    AppContext& m_ctx;
    QWidget* m_window;
};

} // namespace kk
