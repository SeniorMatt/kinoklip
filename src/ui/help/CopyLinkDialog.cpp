// SPDX-License-Identifier: GPL-3.0-or-later
// CopyLinkDialog implementation.
#include "ui/help/CopyLinkDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace kk::guide {

CopyLinkDialog::CopyLinkDialog(const QUrl& url, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("CopyLinkDialog"));
    setWindowTitle(tr("Could Not Open the Browser"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);

    auto* text = new QLabel(tr("Kinoklip could not open your web browser. Copy this link and "
                               "paste it into your browser's address bar:"));
    text->setWordWrap(true);
    layout->addWidget(text);

    m_field = new QLineEdit(url.toString(QUrl::FullyEncoded));
    m_field->setObjectName(QStringLiteral("CopyLinkField"));
    m_field->setReadOnly(true);
    m_field->setMinimumWidth(460);
    m_field->setCursorPosition(0);
    m_field->selectAll();
    layout->addWidget(m_field);

    auto* buttons = new QHBoxLayout;
    m_copy = new QPushButton(tr("&Copy Link"));
    m_copy->setObjectName(QStringLiteral("CopyLinkButton"));
    m_copy->setDefault(true);
    auto* close = new QPushButton(tr("Close"));
    buttons->addStretch(1);
    buttons->addWidget(m_copy);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_field->text());
        m_copy->setText(tr("Copied"));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
}

QString CopyLinkDialog::linkText() const
{
    return m_field->text();
}

CopyLinkDialog* CopyLinkDialog::showFor(const QUrl& url, QWidget* parent)
{
    QWidget* window = parent ? parent->window() : QApplication::activeWindow();
    auto* dialog = new CopyLinkDialog(url, window);
    dialog->setWindowModality(window ? Qt::WindowModal : Qt::ApplicationModal);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
}

} // namespace kk::guide
