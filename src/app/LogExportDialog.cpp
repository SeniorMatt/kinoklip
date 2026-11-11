// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/LogExportDialog.h"

#include "app/LogExport.h"
#include "platform/FileDialogs.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/Metrics.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

#include <memory>

namespace kk {

LogExportDialog::LogExportDialog(const QString& logText, QWidget* parent)
    : QDialog(parent)
    , m_text(logText)
    , m_contents(logexport::contentsDescription())
{
    setObjectName(QStringLiteral("LogExportDialog"));
    setWindowTitle(tr("Export Log File"));
    setMinimumWidth(560);
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(metrics::kSpaceM);
    auto* about = new QLabel(m_contents);
    about->setObjectName(QStringLiteral("LogContents"));
    about->setWordWrap(true);
    about->setTextFormat(Qt::PlainText);
    layout->addWidget(about);
    auto* preview = new QPlainTextEdit;
    preview->setObjectName(QStringLiteral("LogPreview"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preview->setPlainText(m_text);
    preview->setMinimumHeight(220);
    layout->addWidget(preview, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* save = buttons->addButton(tr("Save Log File..."), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("SaveLogButton"));
    save->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    dialogsizing::fit(this);
}

QString LogExportDialog::contentsText() const
{
    return m_contents;
}

bool LogExportDialog::run(QWidget* parent)
{
    // The text is made now, so what is shown is what is saved.
    LogExportDialog dialog(logexport::buildCurrent(), parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    const QString path = platform::getSaveFileName(parent, tr("Export Log File"),
        tr("Text files (*.txt)"), QStringLiteral("paths/lastLogDir"), logexport::defaultFileName());
    if (path.isEmpty())
        return false;
    QString target = path;
    if (QFileInfo(target).suffix().isEmpty())
        target += QStringLiteral(".txt");
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(dialog.logText().toUtf8()) < 0
        || !file.commit()) {
        QMessageBox::warning(parent, tr("Export Log File"),
            tr("The log file could not be written to %1: %2")
                .arg(QDir::toNativeSeparators(target), file.errorString()));
        return false;
    }
    QMessageBox::information(parent, tr("Export Log File"),
        tr("The log file was written to %1. Attach it to your bug report.")
            .arg(QDir::toNativeSeparators(target)));
    return true;
}

} // namespace kk
