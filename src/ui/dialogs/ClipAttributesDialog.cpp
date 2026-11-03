// SPDX-License-Identifier: GPL-3.0-or-later
// ClipAttributesDialog implementation and the apply / edit helpers.
#include "ui/dialogs/ClipAttributesDialog.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"
#include "ui/common/TimecodeFormat.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace kk {

namespace {

/// Pixel aspect ratios offered besides "Auto" (the file's own): common NLE presets.
struct ParPreset
{
    const char* label;
    Rational value;
};
const ParPreset kParPresets[] = {
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "Square"), {1, 1}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "4:3 Standard Definition (NTSC)"), {10, 11}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "16:9 Standard Definition (NTSC)"), {40, 33}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "4:3 Standard Definition (PAL)"), {12, 11}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "16:9 Standard Definition (PAL)"), {16, 11}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "Anamorphic 1.33x"), {4, 3}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "Anamorphic 1.5x"), {3, 2}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "Anamorphic 1.8x"), {9, 5}},
    {QT_TRANSLATE_NOOP("kk::ClipAttributesDialog", "Anamorphic 2x"), {2, 1}},
};

/// Item data of a pixel aspect combo entry: num * kParScale + den (0 = Auto).
constexpr int kParScale = 100000;

int parKey(Rational r)
{
    return r.isValid() ? r.num * kParScale + r.den : 0;
}

Rational parFromKey(int key)
{
    return key > 0 ? Rational {key / kParScale, key % kParScale} : Rational {0, 1};
}

QString ratioText(Rational r)
{
    return QStringLiteral("%1").arg(r.toDouble(), 0, 'f', 4);
}

} // namespace

ClipAttributesDialog::ClipAttributesDialog(const MediaItem& item, int count, QWidget* parent)
    : QDialog(parent)
    , m_initial(item.attributes)
{
    setObjectName(QStringLiteral("ClipAttributesDialog"));
    setWindowTitle(count > 1 ? tr("Clip Attributes (%1 clips)").arg(count) : tr("Clip Attributes"));
    const media::MediaInfo& info = item.info;
    m_channels = info.audioChannels;
    m_tcRate = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};

    m_tabs = new QTabWidget;
    m_tabs->addTab(buildVideoTab(item), tr("Video"));
    m_tabs->addTab(buildAudioTab(item), tr("Audio"));
    m_tabs->addTab(buildTimecodeTab(item), tr("Timecode"));
    if (!info.hasVideo) {
        m_tabs->setTabEnabled(0, false);
        m_tabs->setTabToolTip(0, tr("The media has no video"));
        m_tabs->setCurrentIndex(1);
    }
    if (!info.hasAudio) {
        m_tabs->setTabEnabled(1, false);
        m_tabs->setTabToolTip(1, tr("The media has no audio"));
    }

    auto* name = new QLabel(item.name);
    name->setObjectName(QStringLiteral("ClipAttributesName"));
    // Where the media is: stored in a single-file project or a linked file.
    auto* location = new QLabel(tr("Location: %1").arg(mediaLocationText(item.path)));
    location->setObjectName(QStringLiteral("ClipAttributesLocation"));
    location->setEnabled(false); // dim, like a form note
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(name);
    layout->addWidget(location);
    layout->addWidget(m_tabs);
    layout->addWidget(buttons);
}

QWidget* ClipAttributesDialog::buildVideoTab(const MediaItem& item)
{
    const media::MediaInfo& info = item.info;
    const ClipAttributes& a = item.attributes;
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    // Pixel aspect ratio: Auto shows the file's own value.
    m_par = new QComboBox;
    m_par->setObjectName(QStringLiteral("PixelAspect"));
    const Rational fileSar
        = info.sampleAspectRatio.isValid() ? info.sampleAspectRatio : Rational {1, 1};
    m_par->addItem(tr("Auto (file: %1)").arg(ratioText(fileSar)), 0);
    for (const ParPreset& p : kParPresets) {
        m_par->addItem(
            QStringLiteral("%1 (%2)").arg(tr(p.label), ratioText(p.value)), parKey(p.value));
    }
    if (a.pixelAspect.isValid() && m_par->findData(parKey(a.pixelAspect)) < 0)
        m_par->addItem(tr("Custom (%1)").arg(ratioText(a.pixelAspect)), parKey(a.pixelAspect));
    m_par->setCurrentIndex(std::max(0, m_par->findData(parKey(a.pixelAspect))));
    m_par->setToolTip(tr("How wide one pixel is. With proxy media the original's shape is used "
                         "(proxies have square pixels)."));
    form->addRow(tr("Pixel aspect ratio:"), m_par);

    // Data levels.
    auto* levels = new QWidget;
    auto* levelsRow = new QHBoxLayout(levels);
    levelsRow->setContentsMargins(0, 0, 0, 0);
    m_levels = new QButtonGroup(this);
    const std::pair<DataLevels, QString> levelChoices[] = {{DataLevels::Auto, tr("Auto")},
        {DataLevels::Video, tr("Video")}, {DataLevels::Full, tr("Full")}};
    for (const auto& [value, label] : levelChoices) {
        auto* b = new QRadioButton(label);
        b->setObjectName(QStringLiteral("Levels_") + dataLevelsToString(value));
        b->setChecked(a.levels == value);
        m_levels->addButton(b, int(value));
        levelsRow->addWidget(b);
    }
    levelsRow->addStretch();
    levels->setToolTip(tr("Video: 16 - 235 is black to white (most cameras). Full: 0 - 255. "
                          "Applied by the GPU renderer (not in the software fallback)."));
    form->addRow(tr("Data levels:"), levels);

    // Field order and deinterlacing.
    m_fieldOrder = new QComboBox;
    m_fieldOrder->setObjectName(QStringLiteral("FieldOrder"));
    m_fieldOrder->addItem(tr("Auto (upper field first)"), int(FieldOrder::Auto));
    m_fieldOrder->addItem(tr("Progressive"), int(FieldOrder::Progressive));
    m_fieldOrder->addItem(tr("Upper field first"), int(FieldOrder::UpperFirst));
    m_fieldOrder->addItem(tr("Lower field first"), int(FieldOrder::LowerFirst));
    m_fieldOrder->setCurrentIndex(std::max(0, m_fieldOrder->findData(int(a.fieldOrder))));
    form->addRow(tr("Field dominance:"), m_fieldOrder);
    m_deinterlace = new QCheckBox(tr("Enable deinterlacing"));
    m_deinterlace->setObjectName(QStringLiteral("Deinterlace"));
    m_deinterlace->setChecked(a.deinterlace);
    m_deinterlace->setToolTip(tr("Shows the first field only, interpolating the lines of the "
                                 "other one (no combing on motion; half the vertical detail). "
                                 "GPU renderer only."));
    form->addRow(QString(), m_deinterlace);
    const auto syncDeinterlace = [this] {
        const bool progressive
            = FieldOrder(m_fieldOrder->currentData().toInt()) == FieldOrder::Progressive;
        m_deinterlace->setEnabled(!progressive);
    };
    connect(m_fieldOrder, &QComboBox::currentIndexChanged, this, syncDeinterlace);
    syncDeinterlace();

    // Rotation on top of the file's own.
    m_rotation = new QComboBox;
    m_rotation->setObjectName(QStringLiteral("Rotation"));
    for (const int deg : {0, 90, 180, 270})
        m_rotation->addItem(deg == 0 ? tr("None") : tr("%1 degrees clockwise").arg(deg), deg);
    m_rotation->setCurrentIndex(std::max(0, m_rotation->findData(normalizedRotation(a.rotation))));
    if (info.rotation != 0)
        m_rotation->setToolTip(
            tr("Added to the file's own rotation (%1 degrees).").arg(info.rotation));
    form->addRow(tr("Rotation:"), m_rotation);
    return page;
}

void ClipAttributesDialog::fillChannels(QComboBox* box, bool withMix)
{
    box->clear();
    for (int c = 0; c < std::max(m_channels, 1); ++c)
        box->addItem(tr("Channel %1").arg(c + 1), c);
    if (withMix)
        box->addItem(tr("All channels (mix)"), kMixAllChannels);
    box->addItem(tr("Mute"), kMutedChannel);
}

QWidget* ClipAttributesDialog::buildAudioTab(const MediaItem& item)
{
    const ClipAttributes& a = item.attributes;
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    auto* source = new QLabel(
        m_channels > 0 ? tr("%n source channel(s)", nullptr, m_channels) : tr("No audio channels"));
    form->addRow(tr("Source:"), source);

    m_format = new QComboBox;
    m_format->setObjectName(QStringLiteral("ChannelFormat"));
    m_format->addItem(tr("Default (as in the file)"), int(ChannelFormat::Auto));
    m_format->addItem(tr("Stereo (choose the channels)"), int(ChannelFormat::Stereo));
    m_format->addItem(tr("Mono (one channel on both sides)"), int(ChannelFormat::Mono));
    m_format->setCurrentIndex(std::max(0, m_format->findData(int(a.channelFormat))));
    form->addRow(tr("Format:"), m_format);

    m_left = new QComboBox;
    m_left->setObjectName(QStringLiteral("LeftChannel"));
    m_right = new QComboBox;
    m_right->setObjectName(QStringLiteral("RightChannel"));
    m_leftLabel = new QLabel(tr("Left:"));
    m_rightLabel = new QLabel(tr("Right:"));
    form->addRow(m_leftLabel, m_left);
    form->addRow(m_rightLabel, m_right);
    m_swap = new QPushButton(tr("Swap Left and Right"));
    m_swap->setObjectName(QStringLiteral("SwapChannels"));
    form->addRow(QString(), m_swap);

    fillChannels(m_left, a.channelFormat == ChannelFormat::Mono);
    fillChannels(m_right, false);
    m_left->setCurrentIndex(std::max(0, m_left->findData(a.left)));
    m_right->setCurrentIndex(std::max(0, m_right->findData(m_channels > 1 ? a.right : 0)));

    connect(m_format, &QComboBox::currentIndexChanged, this, [this] {
        // Mono offers the mix of all channels for its one source.
        const QVariant left = m_left->currentData();
        fillChannels(m_left, ChannelFormat(m_format->currentData().toInt()) == ChannelFormat::Mono);
        m_left->setCurrentIndex(std::max(0, m_left->findData(left)));
        updateAudioEnabled();
    });
    connect(m_swap, &QPushButton::clicked, this, [this] {
        const QVariant l = m_left->currentData();
        const QVariant r = m_right->currentData();
        m_left->setCurrentIndex(std::max(0, m_left->findData(r)));
        m_right->setCurrentIndex(std::max(0, m_right->findData(l)));
    });
    updateAudioEnabled();
    return page;
}

void ClipAttributesDialog::updateAudioEnabled()
{
    const auto format = ChannelFormat(m_format->currentData().toInt());
    const bool custom = format != ChannelFormat::Auto;
    m_left->setEnabled(custom);
    m_leftLabel->setEnabled(custom);
    m_leftLabel->setText(format == ChannelFormat::Mono ? tr("Channel:") : tr("Left:"));
    const bool stereo = format == ChannelFormat::Stereo;
    m_right->setEnabled(stereo);
    m_rightLabel->setEnabled(stereo);
    m_swap->setEnabled(stereo);
    const QString why = custom ? QString() : tr("Choose Stereo or Mono to route the channels");
    m_left->setToolTip(why);
    m_right->setToolTip(stereo ? QString() : tr("Only used by the Stereo format"));
}

QWidget* ClipAttributesDialog::buildTimecodeTab(const MediaItem& item)
{
    const ClipAttributes& a = item.attributes;
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    m_overrideTc = new QCheckBox(tr("Use this start timecode"));
    m_overrideTc->setObjectName(QStringLiteral("OverrideTimecode"));
    m_overrideTc->setChecked(a.overrideTimecode);
    m_timecode = new QLineEdit(timecode::format(a.startTimecodeFrames, m_tcRate));
    m_timecode->setObjectName(QStringLiteral("StartTimecode"));
    m_timecode->setEnabled(a.overrideTimecode);
    m_timecode->setToolTip(tr("Shown in the source viewer for frame 0 of the media (%1 fps).")
                               .arg(m_tcRate.toDouble(), 0, 'f', 3));
    connect(m_overrideTc, &QCheckBox::toggled, m_timecode, &QWidget::setEnabled);
    form->addRow(QString(), m_overrideTc);
    form->addRow(tr("Start timecode:"), m_timecode);
    return page;
}

ClipAttributes ClipAttributesDialog::result() const
{
    ClipAttributes a = m_initial;
    a.pixelAspect = parFromKey(m_par->currentData().toInt());
    a.levels = DataLevels(std::max(0, m_levels->checkedId()));
    a.fieldOrder = FieldOrder(m_fieldOrder->currentData().toInt());
    a.deinterlace = m_deinterlace->isChecked();
    a.rotation = m_rotation->currentData().toInt();
    a.channelFormat = ChannelFormat(m_format->currentData().toInt());
    if (a.channelFormat == ChannelFormat::Auto) {
        a.left = 0;
        a.right = 1;
    } else {
        a.left = m_left->currentData().toInt();
        a.right = a.channelFormat == ChannelFormat::Stereo ? m_right->currentData().toInt() : 1;
    }
    a.overrideTimecode = m_overrideTc->isChecked();
    if (a.overrideTimecode) {
        if (const auto f = timecode::parse(m_timecode->text(), m_tcRate))
            a.startTimecodeFrames = std::max<int64_t>(0, *f);
    } else {
        a.startTimecodeFrames = 0;
    }
    return a;
}

namespace clipattributes {

QStringList editableItems(const Project& project, const QStringList& ids)
{
    QStringList out;
    for (const QString& id : ids) {
        const MediaItem* item = project.mediaPool()->item(id);
        if (item && item->kind == MediaItemKind::File && !out.contains(id))
            out.append(id);
    }
    return out;
}

QStringList timelineTargets(const AppContext& ctx)
{
    QStringList ids;
    if (!ctx.project || !ctx.selection)
        return ids;
    const TimelineData& data = ctx.project->timeline()->data();
    for (const quint64 clipId : ctx.selection->clips()) {
        const ClipData* clip = editops::clipById(data, clipId);
        if (clip && clip->kind == ClipKind::Media && !ids.contains(clip->mediaId))
            ids.append(clip->mediaId);
    }
    return editableItems(*ctx.project, ids);
}

bool apply(Project& project, const QStringList& ids, const ClipAttributes& attributes)
{
    const QStringList items = editableItems(project, ids);
    if (items.isEmpty())
        return false;
    bool changed = false;
    project.beginEdit(QObject::tr("Clip Attributes"));
    for (const QString& id : items) {
        const MediaItem* item = project.mediaPool()->item(id);
        if (item && !(item->attributes == attributes)) {
            project.mediaPool()->setItemAttributes(id, attributes);
            changed = true;
        }
    }
    if (changed)
        project.endEdit();
    else
        project.cancelEdit();
    return changed;
}

bool edit(Project& project, const QStringList& ids, QWidget* parent)
{
    const QStringList items = editableItems(project, ids);
    if (items.isEmpty())
        return false;
    ClipAttributesDialog dialog(
        *project.mediaPool()->item(items.front()), int(items.size()), parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    return apply(project, items, dialog.result());
}

} // namespace clipattributes

} // namespace kk
