// SPDX-License-Identifier: GPL-3.0-or-later
// BugReportDialog — Help > Report a Bug or Inconsistency…: choose the kind of report, read
// exactly what would be shared, then open GitHub's form with or without it.
#pragma once

#include "app/BugReport.h"

#include <QDialog>

class QButtonGroup;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace kk {

/// The report dialog. The system information is read when the dialog opens, in memory, to
/// show it; nothing leaves the computer unless the user presses "Open GitHub with This Info"
/// (the browser opens the prefilled form, which they still review and submit on GitHub) or
/// "Copy System Info" (the clipboard). URLs open through guide::openUrl (platform::openUrl;
/// tests replace it with guide::setUrlOpenerForTesting()).
class BugReportDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p info is what would be shared (bugreport::collect() in the app; fixed text in tests).
    explicit BugReportDialog(bugreport::SystemInfo info, QWidget* parent = nullptr);

    [[nodiscard]] bugreport::Kind kind() const;
    void setKind(bugreport::Kind kind);
    /// The text in the read-only box: exactly what the "with this info" link carries.
    [[nodiscard]] QString previewText() const;

    QPushButton* shareButton() const { return m_share; }
    QPushButton* withoutButton() const { return m_without; }
    QPushButton* copyButton() const { return m_copy; }
    QPushButton* cancelButton() const { return m_cancel; }

private:
    void updatePreview();
    void openGitHub(bool share);

    bugreport::SystemInfo m_info;
    QButtonGroup* m_kinds = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QLabel* m_previewLabel = nullptr;
    QPushButton* m_share = nullptr;
    QPushButton* m_without = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_cancel = nullptr;
};

} // namespace kk
