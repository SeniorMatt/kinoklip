// SPDX-License-Identifier: GPL-3.0-or-later
// CopyLinkDialog — shown when no browser could be opened: the link to copy by hand.
#pragma once

#include <QDialog>
#include <QUrl>

class QLineEdit;
class QPushButton;

namespace kk::guide {

/// "Kinoklip could not open your web browser": the link in a selectable read-only field
/// ("CopyLinkField", fully selected), Copy Link ("CopyLinkButton": the clipboard, then
/// "Copied") and Close. Deletes itself when closed. Nothing is sent anywhere.
class CopyLinkDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CopyLinkDialog(const QUrl& url, QWidget* parent = nullptr);
    [[nodiscard]] QString linkText() const;

    /// Shows a CopyLinkDialog for @p url over @p parent's window (or the active window),
    /// window-modal, without blocking. Returns it.
    static CopyLinkDialog* showFor(const QUrl& url, QWidget* parent = nullptr);

private:
    QLineEdit* m_field = nullptr;
    QPushButton* m_copy = nullptr;
};

} // namespace kk::guide
