// SPDX-License-Identifier: GPL-3.0-or-later
// FileDialogs — the one entry point for every open/save/folder dialog of the app.
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace kk::platform {

/// What a file dialog asks for. Every dialog of the app goes through runFileDialog() (or the
/// wrappers below), which shows the desktop's own file chooser through the XDG desktop portal
/// (the KDE/Dolphin dialog on Plasma, GTK's on GNOME) when one is available, and Qt's widget
/// dialog otherwise. See src/platform/readme.md.
struct FileDialogRequest
{
    enum class Mode {
        OpenFile, ///< one existing file
        OpenFiles, ///< one or more existing files
        SaveFile, ///< a file name to write (the dialog confirms overwriting)
        Directory, ///< an existing folder
    };
    Mode mode = Mode::OpenFile;
    QString title;
    /// Qt filter syntax: "Media (*.mp4 *.mov);;All files (*)". Ignored for Directory.
    QString filter;
    /// Start folder (or, for SaveFile, a full suggested path). Empty: the remembered folder of
    /// settingsKey, else the home folder.
    QString directory;
    /// SaveFile: suggested file name (used when directory is a folder or empty).
    QString fileName;
    /// QSettings key that remembers the folder of the last choice (e.g. "paths/lastMediaDir").
    /// Empty: nothing is remembered.
    QString settingsKey;
};

/// Which implementation shows the dialogs.
enum class FileDialogBackend {
    Portal, ///< org.freedesktop.portal.FileChooser (the desktop's native dialog)
    Qt, ///< QFileDialog (Qt's widget dialog under the bundle's "generic" platform theme)
};

/// Pure backend choice (unit-tested): @p override is $KINOKLIP_FILE_DIALOG ("qt": always Qt's
/// dialog; "portal": the portal when there is one, and no Qt dialog if a portal call fails;
/// empty/"auto": portal, Qt's dialog if it fails); @p portalVersion is the FileChooser interface
/// version (0 = no portal); folder pickers need version 3.
[[nodiscard]] FileDialogBackend chooseFileDialogBackend(
    const QString& override, uint portalVersion, FileDialogRequest::Mode mode);

/// Backend that runFileDialog() would use now (probes the session bus once, then cached).
[[nodiscard]] FileDialogBackend currentFileDialogBackend(FileDialogRequest::Mode mode);

/// Shows the dialog modal to @p parent's window. Returns the chosen absolute paths (one for
/// every mode but OpenFiles), or an empty list if cancelled. Remembers the folder in
/// request.settingsKey on success. GUI thread only.
[[nodiscard]] QStringList runFileDialog(QWidget* parent, const FileDialogRequest& request);

/// Tests: while set, runFileDialog() returns what @p handler returns instead of showing a
/// dialog (empty list = cancelled). Pass an empty function to restore the real dialogs.
void setFileDialogHandlerForTests(std::function<QStringList(const FileDialogRequest&)> handler);

/// Folder the dialog would start in: request.directory, else the remembered folder, else home.
[[nodiscard]] QString startDirectory(const FileDialogRequest& request);

// ---- Convenience wrappers (empty string / list = cancelled) ----
[[nodiscard]] QString getOpenFileName(QWidget* parent, const QString& title, const QString& filter,
    const QString& settingsKey, const QString& directory = {});
[[nodiscard]] QStringList getOpenFileNames(QWidget* parent, const QString& title,
    const QString& filter, const QString& settingsKey, const QString& directory = {});
/// @p suggestedName: file name proposed in the start folder.
[[nodiscard]] QString getSaveFileName(QWidget* parent, const QString& title, const QString& filter,
    const QString& settingsKey, const QString& suggestedName, const QString& directory = {});
[[nodiscard]] QString getExistingDirectory(QWidget* parent, const QString& title,
    const QString& settingsKey, const QString& directory = {});

} // namespace kk::platform
