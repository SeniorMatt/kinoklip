// SPDX-License-Identifier: GPL-3.0-or-later
// MetadataPanel implementation: grouped read-only form filled from MediaItem::info.
#include "ui/mediapool/MetadataPanel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"

#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QStackedWidget>

namespace kk {

namespace {
constexpr Rational kFallbackFps {25, 1};
}

MetadataPanel::MetadataPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Metadata"), parent)
    , m_ctx(ctx)
{
    m_stack = new QStackedWidget;
    m_stack->addWidget(createPlaceholderContent(tr("Select a clip in the media pool.")));
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* formHost = new QWidget;
    m_form = new QFormLayout(formHost);
    m_form->setContentsMargins(12, 10, 12, 10);
    m_form->setLabelAlignment(Qt::AlignRight);
    m_form->setVerticalSpacing(5);
    scroll->setWidget(formHost);
    m_stack->addWidget(scroll);
    setContentWidget(m_stack);

    // Keep the shown item up to date (rename, undo, removal).
    MediaPool* pool = ctx.project->mediaPool();
    connect(pool, &MediaPool::changed, this, [this] { showMedia(m_mediaId); });
}

void MetadataPanel::showMedia(const QString& mediaId)
{
    m_mediaId = mediaId;
    const MediaItem* item = mediaId.isEmpty() ? nullptr : m_ctx.project->mediaPool()->item(mediaId);
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    if (!item) {
        m_stack->setCurrentIndex(0);
        return;
    }
    const media::MediaInfo& info = item->info;
    auto addSection = [this](const QString& title) {
        auto* label = new QLabel(title);
        QFont bold = label->font();
        bold.setBold(true);
        label->setFont(bold);
        label->setContentsMargins(0, m_form->rowCount() > 0 ? 8 : 0, 0, 0);
        m_form->addRow(label);
    };
    auto addRow = [this](const QString& label, const QString& value) {
        if (value.isEmpty())
            return;
        auto* field = new QLabel(value);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field->setWordWrap(true);
        auto* name = new QLabel(label);
        // Dimmed through a palette ROLE, not a copied colour: an own palette would keep the
        // previous scheme's text colour after a colour scheme switch.
        name->setForegroundRole(QPalette::PlaceholderText);
        m_form->addRow(name, field);
    };

    // The file is checked once per selection (not per paint).
    const QFileInfo file(item->path);
    addSection(tr("Clip"));
    addRow(tr("Name"), item->name);
    addRow(tr("File"), file.fileName());
    addRow(tr("Folder"), file.absolutePath());
    if (file.exists())
        addRow(tr("Size"), QLocale().formattedDataSize(file.size()));
    else
        addRow(tr("Status"), tr("Media offline (file not found)"));
    addRow(tr("Container"), info.container);
    if (info.durationUs > 0) {
        const Rational fps = info.frameRate.isValid() ? info.frameRate : kFallbackFps;
        addRow(tr("Duration"), timecode::format(usToFrames(info.durationUs, fps), fps));
    }
    if (info.hasVideo) {
        addSection(info.isStillImage ? tr("Image") : tr("Video"));
        addRow(tr("Resolution"), tr("%1 x %2").arg(info.width).arg(info.height));
        if (info.frameRate.isValid() && !info.isStillImage)
            addRow(tr("Frame rate"),
                tr("%1 fps").arg(QString::number(info.frameRate.toDouble(), 'g', 5)));
        addRow(tr("Codec"), info.videoCodec);
        if (info.rotation != 0)
            addRow(tr("Rotation"), tr("%1 degrees").arg(info.rotation));
    }
    if (info.hasAudio) {
        addSection(tr("Audio"));
        addRow(tr("Codec"), info.audioCodec);
        addRow(tr("Channels"), QString::number(info.audioChannels));
        addRow(tr("Sample rate"), tr("%1 Hz").arg(info.audioSampleRate));
    }
    if (!info.error.isEmpty()) {
        addSection(tr("Probe"));
        addRow(tr("Error"), info.error);
    }
    m_stack->setCurrentIndex(1);
}

} // namespace kk
