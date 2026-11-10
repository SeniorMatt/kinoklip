// SPDX-License-Identifier: GPL-3.0-or-later
// LogExportDialog — Help > Export Log File...: says what the file contains, shows the whole text
// so it can be read before sending, and saves it where the user chooses (spec section 64).
#pragma once

#include <QDialog>

class QPlainTextEdit;

namespace kk {

class LogExportDialog final : public QDialog
{
    Q_OBJECT
public:
    /// @p logText is the file's text (logexport::buildCurrent()), shown in full.
    explicit LogExportDialog(const QString& logText, QWidget* parent = nullptr);

    /// The explanation above the text (tests).
    [[nodiscard]] QString contentsText() const;
    /// The text that would be saved.
    [[nodiscard]] QString logText() const { return m_text; }

    /// The dialog, then a save dialog (suggested name kinoklip-log-<date>.txt), then the write.
    /// True if a file was written.
    static bool run(QWidget* parent);

private:
    QString m_text;
    QString m_contents;
};

} // namespace kk
