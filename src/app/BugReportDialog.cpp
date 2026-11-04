// SPDX-License-Identifier: GPL-3.0-or-later
// BugReportDialog implementation.
#include "app/BugReportDialog.h"

#include "ui/help/Guide.h"

#include <QButtonGroup>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace kk {

BugReportDialog::BugReportDialog(bugreport::SystemInfo info, QWidget* parent)
    : QDialog(parent)
    , m_info(std::move(info))
{
    setObjectName(QStringLiteral("BugReportDialog"));
    setWindowTitle(tr("Report a Bug or Inconsistency"));
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Thank you for helping. Choose what you want to tell us; "
                                "you describe it on GitHub's report form in your browser."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_kinds = new QButtonGroup(this);
    const QPair<bugreport::Kind, QString> kinds[] = {
        {bugreport::Kind::Bug, tr("&Bug: something is broken or crashes")},
        {bugreport::Kind::ResolveDifference, tr("Works &differently from DaVinci Resolve")},
        {bugreport::Kind::Idea, tr("&Idea: a feature or improvement")},
    };
    for (const auto& [kind, text] : kinds) {
        auto* radio = new QRadioButton(text);
        radio->setObjectName(QStringLiteral("BugReportKind") + QString::number(int(kind)));
        m_kinds->addButton(radio, int(kind));
        layout->addWidget(radio);
    }
    m_kinds->button(int(bugreport::Kind::Bug))->setChecked(true);

    m_previewLabel = new QLabel;
    m_previewLabel->setWordWrap(true);
    layout->addSpacing(6);
    layout->addWidget(m_previewLabel);
    m_preview = new QPlainTextEdit;
    m_preview->setObjectName(QStringLiteral("BugReportPreview"));
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setMinimumSize(560, 220);
    layout->addWidget(m_preview, 1);

    auto* note = new QLabel(tr("Kinoklip itself sends nothing. Your browser opens GitHub's form; "
                               "you review it and submit it there (you need a GitHub account)."));
    note->setObjectName(QStringLiteral("BugReportNote"));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* buttons = new QHBoxLayout;
    m_copy = new QPushButton(tr("&Copy System Info"));
    m_copy->setToolTip(tr("Copies the text above, for reporting somewhere else"));
    m_share = new QPushButton(tr("&Open GitHub with This Info"));
    m_share->setDefault(true);
    m_without = new QPushButton(tr("Open GitHub &Without It"));
    m_cancel = new QPushButton(tr("Cancel"));
    buttons->addWidget(m_copy);
    buttons->addStretch(1);
    buttons->addWidget(m_share);
    buttons->addWidget(m_without);
    buttons->addWidget(m_cancel);
    layout->addLayout(buttons);

    connect(m_kinds, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on)
            updatePreview();
    });
    connect(m_share, &QPushButton::clicked, this, [this] { openGitHub(true); });
    connect(m_without, &QPushButton::clicked, this, [this] { openGitHub(false); });
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(previewText());
        m_copy->setText(tr("Copied"));
    });
    updatePreview();
}

bugreport::Kind BugReportDialog::kind() const
{
    return bugreport::Kind(qMax(0, m_kinds->checkedId()));
}

void BugReportDialog::setKind(bugreport::Kind kind)
{
    if (QAbstractButton* b = m_kinds->button(int(kind)))
        b->setChecked(true);
}

QString BugReportDialog::previewText() const
{
    return m_preview->toPlainText();
}

void BugReportDialog::updatePreview()
{
    const bugreport::IssueLink link = bugreport::issueLink(kind(), m_info, true);
    m_preview->setPlainText(link.sharedText());
    m_previewLabel->setText(bugreport::hasEnvironment(kind())
            ? tr("This is everything that would be shared: versions and settings, nothing "
                 "personal (no names, folders, files or projects).")
            : tr("For an idea only the version would be shared:"));
}

void BugReportDialog::openGitHub(bool share)
{
    guide::openUrl(bugreport::issueLink(kind(), m_info, share).url);
    accept();
}

} // namespace kk
