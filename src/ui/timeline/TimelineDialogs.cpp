// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineDialogs implementation (plain QDialog / QMessageBox layouts; the look comes from the
// application style).
#include "ui/timeline/TimelineDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace kk::timelinedialogs {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::TimelineDialogs", text);
}

QIcon swatch(const QColor& color)
{
    QPixmap pm(14, 14);
    pm.fill(color);
    return QIcon(pm);
}

} // namespace

QList<std::pair<QString, QColor>> markerColors()
{
    return {
        {tr("Blue"), QColor(0x4c, 0x9b, 0xe8)},
        {tr("Cyan"), QColor(0x3f, 0xc8, 0xd8)},
        {tr("Green"), QColor(0x4c, 0xb8, 0x4c)},
        {tr("Yellow"), QColor(0xe8, 0xc8, 0x3c)},
        {tr("Red"), QColor(0xe0, 0x4a, 0x4a)},
        {tr("Pink"), QColor(0xe8, 0x7a, 0xb4)},
        {tr("Purple"), QColor(0x9a, 0x6a, 0xd8)},
        {tr("Fuchsia"), QColor(0xc8, 0x44, 0xb4)},
        {tr("Rose"), QColor(0xe8, 0x9a, 0x9a)},
        {tr("Lavender"), QColor(0xb4, 0xa4, 0xe8)},
        {tr("Sky"), QColor(0x9a, 0xd0, 0xf0)},
        {tr("Mint"), QColor(0x8c, 0xe0, 0xb4)},
        {tr("Lemon"), QColor(0xf0, 0xe8, 0x8c)},
        {tr("Sand"), QColor(0xc8, 0xb0, 0x84)},
        {tr("Cocoa"), QColor(0x8c, 0x6a, 0x4c)},
        {tr("Cream"), QColor(0xf0, 0xe4, 0xc8)},
    };
}

HandleChoice askHandles(QWidget* parent, int64_t missingFrames, bool canShorten)
{
    QMessageBox box(QMessageBox::Question, tr("Insufficient Handles"),
        QCoreApplication::translate("kk::TimelineDialogs",
            "There is not enough media to create the transition: %n frame(s) are missing.\n\n"
            "Trim the clips so the media covers the transition?",
            nullptr, int(missingFrames)),
        QMessageBox::NoButton, parent);
    QPushButton* trim = box.addButton(tr("Trim Clips"), QMessageBox::AcceptRole);
    QPushButton* shorten
        = canShorten ? box.addButton(tr("Shorten Transition"), QMessageBox::ActionRole) : nullptr;
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(trim);
    box.exec();
    if (box.clickedButton() == trim)
        return HandleChoice::TrimClips;
    if (shorten && box.clickedButton() == shorten)
        return HandleChoice::Shorten;
    Q_UNUSED(cancel);
    return HandleChoice::Cancel;
}

bool editMarker(QWidget* parent, Marker& marker, bool clipMarker)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(clipMarker ? tr("Clip Marker") : tr("Timeline Marker"));
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(marker.name);
    auto* color = new QComboBox;
    int current = 0;
    const auto colors = markerColors();
    for (int i = 0; i < colors.size(); ++i) {
        color->addItem(swatch(colors[i].second), colors[i].first, colors[i].second);
        if (colors[i].second == marker.color)
            current = i;
    }
    if (marker.color.isValid() && color->findData(marker.color) < 0) {
        color->addItem(swatch(marker.color), marker.color.name(), marker.color);
        current = color->count() - 1;
    }
    color->setCurrentIndex(current);
    auto* note = new QPlainTextEdit(marker.note);
    note->setTabChangesFocus(true);
    form->addRow(tr("Name"), name);
    form->addRow(tr("Color"), color);
    form->addRow(tr("Notes"), note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    name->selectAll();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    marker.name = name->text();
    marker.color = color->currentData().value<QColor>();
    marker.note = note->toPlainText();
    return true;
}

std::optional<PasteAttributeSet> askPasteAttributes(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Paste Attributes"));
    const PasteAttributeSet defaults;
    auto* videoBox = new QGroupBox(tr("Video Attributes"));
    auto* transform = new QCheckBox(tr("Transform, Cropping and Composite"));
    auto* effects = new QCheckBox(tr("Effects"));
    auto* audioBox = new QGroupBox(tr("Audio Attributes"));
    auto* volume = new QCheckBox(tr("Volume"));
    auto* pan = new QCheckBox(tr("Pan"));
    auto* otherBox = new QGroupBox(tr("Other"));
    auto* speed = new QCheckBox(tr("Speed Change"));
    transform->setChecked(defaults.transform);
    effects->setChecked(defaults.effects);
    volume->setChecked(defaults.volume);
    pan->setChecked(defaults.pan);
    speed->setChecked(defaults.speed);
    (new QVBoxLayout(videoBox))->addWidget(transform);
    videoBox->layout()->addWidget(effects);
    (new QVBoxLayout(audioBox))->addWidget(volume);
    audioBox->layout()->addWidget(pan);
    (new QVBoxLayout(otherBox))->addWidget(speed);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(videoBox);
    layout->addWidget(audioBox);
    layout->addWidget(otherBox);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    PasteAttributeSet s;
    s.transform = transform->isChecked();
    s.effects = effects->isChecked();
    s.volume = volume->isChecked();
    s.pan = pan->isChecked();
    s.speed = speed->isChecked();
    return s;
}

} // namespace kk::timelinedialogs
