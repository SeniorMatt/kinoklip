// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel, part: the "Media Offline" notice over the picture when the clip shown (source
// clip, or the top visible clip at the timeline viewer's playhead) has no file on disk.
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/ScrubBar.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QLabel>

namespace kk {

void ViewerPanel::setupOfflineNotice()
{
    m_offlineNotice = new QLabel(m_surface->widget());
    m_offlineNotice->setObjectName(QStringLiteral("MediaOfflineNotice"));
    m_offlineNotice->setAlignment(Qt::AlignCenter);
    m_offlineNotice->setWordWrap(true);
    m_offlineNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_offlineNotice->hide();
    if (m_ctx.project) {
        connect(m_ctx.project->mediaPool(), &MediaPool::offlineChanged, this,
            [this] { updateOfflineNotice(); });
        connect(m_ctx.project->mediaPool(), &MediaPool::changed, this,
            [this] { updateOfflineNotice(); });
    }
}

QString ViewerPanel::offlineClipName(int64_t frame) const
{
    if (!m_ctx.project)
        return {};
    const MediaPool* pool = m_ctx.project->mediaPool();
    auto nameOf = [pool](const QString& mediaId) -> QString {
        const MediaItem* item = pool->item(mediaId);
        return item ? item->name : QString();
    };
    if (m_mode == Mode::Source)
        return !m_sourceMediaId.isEmpty() && pool->isOffline(m_sourceMediaId)
            ? nameOf(m_sourceMediaId)
            : QString();
    if (!m_boundTimeline || pool->offlineCount() == 0)
        return {};
    // The top visible clip decides what the viewer shows (two binary searches per track).
    const TimelineData& data = m_boundTimeline->data();
    for (auto it = data.videoTracks.crbegin(); it != data.videoTracks.crend(); ++it) {
        if (!it->enabled)
            continue;
        const ClipData* clip = it->clipAt(frame);
        if (!clip || !clip->enabled)
            continue;
        if (clip->kind == ClipKind::Media && pool->isOffline(clip->mediaId))
            return clip->name.isEmpty() ? nameOf(clip->mediaId) : clip->name;
        return {};
    }
    return {};
}

void ViewerPanel::updateOfflineNotice()
{
    if (!m_offlineNotice)
        return;
    const int64_t frame = m_mode == Mode::Timeline && m_boundTimeline ? m_boundTimeline->playhead()
        : m_scrub                                                     ? m_scrub->position()
                                                                      : 0;
    const QString name = offlineClipName(frame);
    if (name.isEmpty()) {
        m_offlineNotice->hide();
        return;
    }
    const QString text = tr("Media Offline\n%1").arg(name);
    if (m_offlineNotice->text() != text)
        m_offlineNotice->setText(text);
    const SchemeColors& c = schemeColors();
    m_offlineNotice->setStyleSheet(
        QStringLiteral("QLabel { background: %1; color: %2; font-weight: bold; padding: 12px; }")
            .arg(c.negative.darker(150).name(), c.textOn(c.negative.darker(150)).name()));
    m_offlineNotice->resize(m_surface->widget()->size());
    m_offlineNotice->show();
    m_offlineNotice->raise();
}

} // namespace kk
