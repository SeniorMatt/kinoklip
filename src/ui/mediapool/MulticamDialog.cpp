// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamDialog — the New Multicam Clip dialog.
#include "ui/mediapool/MulticamDialog.h"

#include "core/Plural.h"
#include "ui/mediapool/MulticamSync.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace kk {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("kk::MulticamDialog", text, disambiguation, n);
}

const char* const kSyncKey = "multicam/angleSync";
const char* const kNamesKey = "multicam/angleNamesFromClips";
const char* const kMoveKey = "multicam/moveSourceClips";

} // namespace

std::optional<MulticamOptions> askMulticamOptions(
    QWidget* parent, int clipCount, const QString& suggestion)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("NewMulticamDialog"));
    dialog.setWindowTitle(tr("New Multicam Clip"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(
        plural(clipCount, tr("Creates a multicam clip with %n angle from the selected clip."),
            tr("Creates a multicam clip with %n angles from the selected clips.")));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(suggestion);
    name->setObjectName(QStringLiteral("MulticamName"));
    name->selectAll();
    form->addRow(tr("Multicam clip name"), name);

    auto* sync = new QComboBox;
    sync->setObjectName(QStringLiteral("MulticamSync"));
    // Order as in Resolve's dialog: timecode first; Kinoklip remembers the last choice.
    const multicam::SyncMethod methods[]
        = {multicam::SyncMethod::Timecode, multicam::SyncMethod::Waveform,
            multicam::SyncMethod::InPoints, multicam::SyncMethod::Start};
    for (const multicam::SyncMethod m : methods) {
        QString text = syncMethodName(m);
        if (m == multicam::SyncMethod::Waveform)
            text = tr("Sound (audio waveform)");
        sync->addItem(text, int(m));
    }
    QSettings settings;
    const int stored
        = settings.value(QLatin1String(kSyncKey), int(multicam::SyncMethod::Waveform)).toInt();
    sync->setCurrentIndex(std::max(0, sync->findData(stored)));
    form->addRow(tr("Angle sync"), sync);

    auto* names = new QComboBox;
    names->setObjectName(QStringLiteral("MulticamAngleNames"));
    names->addItem(tr("Clip Name"), true);
    names->addItem(tr("Angle Number"), false);
    names->setCurrentIndex(settings.value(QLatin1String(kNamesKey), true).toBool() ? 0 : 1);
    form->addRow(tr("Angle name"), names);
    layout->addLayout(form);

    auto* move = new QCheckBox(tr("Move source clips to \"Original Clips\" bin"));
    move->setObjectName(QStringLiteral("MulticamMoveSources"));
    move->setChecked(settings.value(QLatin1String(kMoveKey), false).toBool());
    layout->addWidget(move);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    MulticamOptions options;
    options.name = name->text().trimmed();
    options.sync = multicam::SyncMethod(sync->currentData().toInt());
    options.namesFromClips = names->currentData().toBool();
    options.moveSources = move->isChecked();
    settings.setValue(QLatin1String(kSyncKey), int(options.sync));
    settings.setValue(QLatin1String(kNamesKey), options.namesFromClips);
    settings.setValue(QLatin1String(kMoveKey), options.moveSources);
    return options;
}

} // namespace kk
