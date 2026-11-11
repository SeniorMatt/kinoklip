// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SettingsTransferDialog.h"

#include "platform/FileDialogs.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/Metrics.h"
#include "ui/common/SettingsPages.h"

#include <QCheckBox>
#include <QDate>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <memory>
#include <QVBoxLayout>

namespace kk {

namespace st = settingstransfer;

namespace {

QString suffixFilter()
{
    return SettingsTransferDialog::tr("Kinoklip settings (*.%1)").arg(QLatin1String(st::kFileSuffix));
}

QLabel* wrapped(const QString& text, bool dim = false)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    if (dim)
        label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

QString leftOutText()
{
    return SettingsTransferDialog::tr(
        "Not included: projects and media, caches, folders and devices of this computer (cache and "
        "proxy folders, project library, LUT folders, audio output device, last-used folders) and "
        "anything secret. Local Control is never switched on by a settings file.");
}

} // namespace

SettingsTransferDialog::SettingsTransferDialog(Mode mode, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(mode == Mode::Export ? QStringLiteral("ExportSettingsDialog")
                                       : QStringLiteral("ImportSettingsDialog"));
    setWindowTitle(mode == Mode::Export ? tr("Export Settings") : tr("Import Settings"));
    setMinimumWidth(480);
}

SettingsTransferDialog* SettingsTransferDialog::forExport(const QString& path, QWidget* parent)
{
    auto* d = new SettingsTransferDialog(Mode::Export, parent);
    auto* layout = new QVBoxLayout(d);
    layout->setSpacing(metrics::kSpaceM);
    layout->addWidget(wrapped(
        tr("All of Kinoklip's settings are written to one file: preferences, keyboard profile and "
           "custom shortcuts, window and panel layout, view options, custom keyboard profiles, "
           "guide presets, power bins and render presets.")));
    layout->addWidget(wrapped(leftOutText(), true));
    d->m_recent = new QCheckBox(tr("Also include the list of recent projects"));
    d->m_recent->setObjectName(QStringLiteral("IncludeRecentProjects"));
    d->m_recent->setToolTip(tr("The paths are only useful if the projects are in the same places on the other computer"));
    layout->addWidget(d->m_recent);
    layout->addWidget(wrapped(tr("File: %1").arg(QDir::toNativeSeparators(path)), true));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* go = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    go->setObjectName(QStringLiteral("ExportButton"));
    go->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
    layout->addWidget(buttons);
    dialogsizing::fit(d);
    return d;
}

SettingsTransferDialog* SettingsTransferDialog::forImport(const QString& path,
    const st::Parsed& parsed, const st::Locations& locations, QWidget* parent)
{
    auto* d = new SettingsTransferDialog(Mode::Import, parent);
    auto* layout = new QVBoxLayout(d);
    layout->setSpacing(metrics::kSpaceM);
    QString origin = tr("File: %1").arg(QDir::toNativeSeparators(path));
    if (!parsed.appVersion.isEmpty())
        origin += tr("\nMade by Kinoklip %1").arg(parsed.appVersion);
    if (!parsed.created.isEmpty())
        origin += tr(" on %1").arg(parsed.created);
    layout->addWidget(wrapped(origin, true));
    layout->addWidget(wrapped(tr("The file contains:")));
    d->m_contents = new QPlainTextEdit;
    d->m_contents->setObjectName(QStringLiteral("SettingsContents"));
    d->m_contents->setReadOnly(true);
    QStringList lines = parsed.summary;
    if (lines.isEmpty())
        lines.append(tr("Nothing that this version of Kinoklip can use."));
    if (parsed.localControlStaysOff)
        lines.append(tr("Local Control: stays as it is (a file cannot switch it on)"));
    if (parsed.ignoredKeys > 0)
        lines.append(tr("%n entry(ies) unknown to this version, ignored", nullptr, parsed.ignoredKeys));
    lines.append(parsed.rejected);
    d->m_contents->setPlainText(lines.join(QLatin1Char('\n')));
    d->m_contents->setMaximumHeight(160);
    layout->addWidget(d->m_contents);
    auto* replace = wrapped(tr("Importing replaces your current settings with the values in this "
                               "file; settings the file does not mention stay as they are."));
    replace->setObjectName(QStringLiteral("ReplaceWarning"));
    layout->addWidget(replace);
    layout->addWidget(wrapped(
        tr("A backup of your current settings is written first, to %1")
            .arg(QDir::toNativeSeparators(locations.backupDir)),
        true));
    layout->addWidget(wrapped(tr("Window layout, keyboard and most other settings apply after you "
                                 "restart Kinoklip."),
        true));
    d->m_recent = new QCheckBox(tr("Also take over the list of recent projects"));
    d->m_recent->setObjectName(QStringLiteral("IncludeRecentProjects"));
    d->m_recent->setEnabled(parsed.hasRecentProjects);
    layout->addWidget(d->m_recent);
    layout->addWidget(wrapped(leftOutText(), true));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* go = buttons->addButton(tr("Replace My Settings"), QDialogButtonBox::AcceptRole);
    go->setObjectName(QStringLiteral("ImportButton"));
    go->setEnabled(!parsed.settings.isEmpty() || !parsed.files.isEmpty()
        || parsed.localControlStaysOff);
    connect(buttons, &QDialogButtonBox::accepted, d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
    layout->addWidget(buttons);
    dialogsizing::fit(d);
    return d;
}

bool SettingsTransferDialog::includeRecentProjects() const
{
    return m_recent && m_recent->isEnabled() && m_recent->isChecked();
}

QString SettingsTransferDialog::contentsText() const
{
    return m_contents ? m_contents->toPlainText() : QString();
}

bool SettingsTransferDialog::runExport(QWidget* parent)
{
    const QString stamp = QDate::currentDate().toString(Qt::ISODate);
    const QString path = platform::getSaveFileName(parent, tr("Export Settings"), suffixFilter(),
        QStringLiteral("paths/lastSettingsDir"),
        QStringLiteral("kinoklip-%1.%2").arg(stamp, QLatin1String(st::kFileSuffix)));
    if (path.isEmpty())
        return false;
    QString target = path;
    if (QFileInfo(target).suffix().isEmpty())
        target += QLatin1Char('.') + QLatin1String(st::kFileSuffix);
    std::unique_ptr<SettingsTransferDialog> dialog(forExport(target, parent));
    if (dialog->exec() != QDialog::Accepted)
        return false;
    QString error;
    if (!st::exportToFile(target, st::ExportOptions {dialog->includeRecentProjects()}, &error)) {
        QMessageBox::warning(parent, tr("Export Settings"), error);
        return false;
    }
    QMessageBox::information(parent, tr("Export Settings"),
        tr("Your settings were written to %1.").arg(QDir::toNativeSeparators(target)));
    return true;
}

bool SettingsTransferDialog::runImport(QWidget* parent)
{
    const QString path = platform::getOpenFileName(parent, tr("Import Settings"), suffixFilter(),
        QStringLiteral("paths/lastSettingsDir"));
    if (path.isEmpty())
        return false;
    st::Parsed parsed;
    QString error;
    if (!st::parseFile(path, &parsed, &error)) {
        QMessageBox::warning(parent, tr("Import Settings"), error);
        return false;
    }
    const st::Locations locations = st::standardLocations();
    std::unique_ptr<SettingsTransferDialog> dialog(forImport(path, parsed, locations, parent));
    if (dialog->exec() != QDialog::Accepted)
        return false;
    st::ApplyOptions options;
    options.includeRecentProjects = dialog->includeRecentProjects();
    options.locations = locations;
    const st::ApplyResult result = st::apply(parsed, options);
    if (!result.ok) {
        QMessageBox::warning(parent, tr("Import Settings"), result.error);
        return false;
    }
    QString text = tr("Settings imported. Your previous settings are saved in %1.")
                       .arg(QDir::toNativeSeparators(result.backupPath));
    if (result.restartNeeded)
        text += tr("\n\nRestart Kinoklip to apply them.");
    if (!result.notes.isEmpty())
        text += QLatin1String("\n\n") + result.notes.join(QLatin1Char('\n'));
    QMessageBox::information(parent, tr("Import Settings"), text);
    return true;
}

// ---- Preferences page ------------------------------------------------------------------

namespace {

class TransferPage final : public SettingsPage
{
public:
    explicit TransferPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(metrics::kSpaceM);
        layout->addWidget(wrapped(
            SettingsTransferDialog::tr("Move all of Kinoklip's settings to another computer, or "
                                       "keep a copy of them, with one file.")));
        auto* row = new QHBoxLayout;
        auto* exportButton = new QPushButton(SettingsTransferDialog::tr("Export Settings..."));
        exportButton->setObjectName(QStringLiteral("ExportSettingsButton"));
        auto* importButton = new QPushButton(SettingsTransferDialog::tr("Import Settings..."));
        importButton->setObjectName(QStringLiteral("ImportSettingsButton"));
        connect(exportButton, &QPushButton::clicked, this,
            [this] { SettingsTransferDialog::runExport(window()); });
        connect(importButton, &QPushButton::clicked, this,
            [this] { SettingsTransferDialog::runImport(window()); });
        row->addWidget(exportButton);
        row->addWidget(importButton);
        row->addStretch(1);
        layout->addLayout(row);
        layout->addWidget(wrapped(leftOutText(), true));
        layout->addStretch(1);
    }

    void apply() override { }
};

} // namespace

void registerSettingsTransferPage(SettingsPageRegistry& pages)
{
    SettingsPageInfo page {QStringLiteral("transfer"),
        QCoreApplication::translate("kk::SettingsTransferDialog", "Import and Export"),
        QStringLiteral("document-export"), 170,
        [](QWidget* parent) { return new TransferPage(parent); }, QStringLiteral("user"),
        QString()};
    pages.add(std::move(page));
}

} // namespace kk
