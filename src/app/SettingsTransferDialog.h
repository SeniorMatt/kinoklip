// SPDX-License-Identifier: GPL-3.0-or-later
// SettingsTransferDialog — Export Settings... and Import Settings... (spec section 61), reached
// from Preferences > User > Import and Export. The work is done by SettingsTransfer.h.
#pragma once

#include "app/SettingsTransfer.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;

namespace kk {

class SettingsPageRegistry;

/// A small dialog with two looks. Export: says what the file will and will not contain and has
/// the recent-projects checkbox (off). Import: lists what the chosen file contains, warns that
/// the current settings are replaced, names the backup that will be written, and has the same
/// checkbox (off, enabled only when the file has a list).
class SettingsTransferDialog final : public QDialog
{
    Q_OBJECT
public:
    enum class Mode { Export, Import };

    /// Export dialog; @p path is where the file will be written (shown in the text).
    static SettingsTransferDialog* forExport(const QString& path, QWidget* parent = nullptr);
    /// Import dialog for an already checked file.
    static SettingsTransferDialog* forImport(const QString& path,
        const settingstransfer::Parsed& parsed, const settingstransfer::Locations& locations,
        QWidget* parent = nullptr);

    [[nodiscard]] bool includeRecentProjects() const;
    /// The text of the contents list (tests).
    [[nodiscard]] QString contentsText() const;

    /// File dialog + export dialog + write. Returns true if a file was written.
    static bool runExport(QWidget* parent);
    /// File dialog + check + import dialog + apply + result message. True if applied.
    static bool runImport(QWidget* parent);

private:
    SettingsTransferDialog(Mode mode, QWidget* parent);
    QCheckBox* m_recent = nullptr;
    QPlainTextEdit* m_contents = nullptr;
};

/// Adds the "Import and Export" page to Preferences > User.
void registerSettingsTransferPage(SettingsPageRegistry& pages);

} // namespace kk
