// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER metadata implementation: a form filled from MediaInfo.
#include "ui/mediapool/MetadataPanel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"

#include <QFormLayout>
#include <QLabel>
#include <QStackedWidget>

namespace cw {

MetadataPanel::MetadataPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Metadata"), parent)
    , m_ctx(ctx)
{
    m_stack = new QStackedWidget;
    m_stack->addWidget(createPlaceholderContent(tr("Select a clip in the media pool.")));
    auto* formHost = new QWidget;
    m_form = new QFormLayout(formHost);
    m_form->setContentsMargins(12, 10, 12, 10);
    m_form->setLabelAlignment(Qt::AlignRight);
    m_stack->addWidget(formHost);
    setContentWidget(m_stack);
}

void MetadataPanel::showMedia(const QString& mediaId)
{
    const MediaItem* item = mediaId.isEmpty() ? nullptr : m_ctx.project->mediaPool()->item(mediaId);
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    if (!item) {
        m_stack->setCurrentIndex(0);
        return;
    }
    const media::MediaInfo& info = item->info;
    auto addRow = [this](const QString& label, const QString& value) {
        auto* field = new QLabel(value);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse);
        field->setWordWrap(true);
        m_form->addRow(label, field);
    };
    addRow(tr("Name"), item->name);
    addRow(tr("Path"), item->path);
    if (info.hasVideo) {
        addRow(tr("Resolution"), tr("%1 × %2").arg(info.width).arg(info.height));
        if (info.frameRate.isValid())
            addRow(tr("Frame rate"), QString::number(info.frameRate.toDouble(), 'f', 3));
        addRow(tr("Video codec"), info.videoCodec);
    }
    if (info.hasAudio) {
        addRow(tr("Audio"), tr("%1 Hz, %2 ch").arg(info.audioSampleRate).arg(info.audioChannels));
        addRow(tr("Audio codec"), info.audioCodec);
    }
    if (info.durationUs > 0) {
        const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
        addRow(tr("Duration"), timecode::format(usToFrames(info.durationUs, fps), fps));
    }
    addRow(tr("Container"), info.container);
    m_stack->setCurrentIndex(1);
}

} // namespace cw
