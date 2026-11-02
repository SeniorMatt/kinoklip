// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolStripDelegate implementation.
#include "ui/mediapool/MediaPoolStripDelegate.h"

#include "media/MediaCache.h"
#include "ui/common/SchemeColors.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QPainter>

#include <algorithm>

namespace kk {

namespace {

constexpr int kPadding = 4; ///< around the row content
constexpr int kNameHeight = 18; ///< name / duration line above the strip
constexpr int kStripHeight = 54; ///< thumbnail height in px
constexpr double kThumbAspect = 16.0 / 9.0; ///< thumbnail cell width / height
constexpr int kMinCells = 1;

} // namespace

MediaPoolStripDelegate::MediaPoolStripDelegate(SourceLookup lookup, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_lookup(std::move(lookup))
    , m_thumbnails([](const QString& path, int64_t timeUs, int height) {
        return media::MediaCache::instance().thumbnail(path, timeUs, height);
    })
{ }

int MediaPoolStripDelegate::rowHeight()
{
    return kNameHeight + kStripHeight + 2 * kPadding;
}

QVector<int64_t> MediaPoolStripDelegate::thumbnailTimes(int64_t durationUs, int count)
{
    QVector<int64_t> times;
    count = std::max(kMinCells, count);
    times.reserve(count);
    for (int i = 0; i < count; ++i)
        times.append(durationUs > 0 ? int64_t(double(durationUs) * (i + 0.5) / count) : 0);
    return times;
}

QSize MediaPoolStripDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const
{
    return {std::max(option.rect.width(), 200), rowHeight()};
}

void MediaPoolStripDelegate::paint(
    QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const SchemeColors& sc = schemeColors();
    const QRect row = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    p->save();
    p->fillRect(row, selected ? withAlpha(sc.highlight, 90) : sc.view);

    // Name and duration.
    const QRect nameRect(
        row.left() + kPadding, row.top() + kPadding, row.width() - 2 * kPadding, kNameHeight);
    const QString name = index.data(Qt::DisplayRole).toString();
    const QString duration = index.data(MediaPoolModel::DurationTextRole).toString();
    p->setFont(option.font);
    p->setPen(selected ? sc.highlightText : sc.text);
    p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
        option.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width() * 2 / 3));
    p->setPen(sc.textDim);
    p->drawText(nameRect, Qt::AlignRight | Qt::AlignVCenter, duration);

    // The strip: thumbnails at evenly spaced times across the clip.
    const QRect strip(
        row.left() + kPadding, nameRect.bottom() + 1, row.width() - 2 * kPadding, kStripHeight);
    p->fillRect(strip, sc.viewerSurround);
    const StripSource source
        = m_lookup ? m_lookup(index.data(MediaPoolModel::IdRole).toString()) : StripSource {};
    if (!source.path.isEmpty() && source.hasVideo && m_thumbnails) {
        const int cellWidth = std::max(1, int(kStripHeight * kThumbAspect));
        const int cells = std::max(kMinCells, strip.width() / cellWidth);
        const QVector<int64_t> times = thumbnailTimes(source.durationUs, cells);
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        for (int i = 0; i < cells; ++i) {
            const QRect cell(strip.left() + i * strip.width() / cells, strip.top(),
                strip.width() / cells, strip.height());
            const QImage thumb = m_thumbnails(source.path, times[i], int(kStripHeight * dpr));
            if (thumb.isNull())
                continue; // pending: thumbnailReady repaints the view
            const QSize size = thumb.size().scaled(cell.size(), Qt::KeepAspectRatio);
            const QRect target(cell.left() + (cell.width() - size.width()) / 2,
                cell.top() + (cell.height() - size.height()) / 2, size.width(), size.height());
            p->drawImage(target, thumb);
        }
    } else {
        // Audio-only items: a calm line across the strip instead of pictures.
        p->setPen(QPen(sc.audioClip, 1.5));
        p->drawLine(strip.left() + kPadding, strip.center().y(), strip.right() - kPadding,
            strip.center().y());
    }
    if (index.data(MediaPoolModel::OfflineRole).toBool()) {
        p->fillRect(strip, withAlpha(sc.negative.darker(130), 140));
        p->setPen(sc.overlayText);
        p->drawText(strip, Qt::AlignCenter, tr("Media Offline"));
    }
    if (selected) {
        p->setPen(QPen(sc.selection, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRect(strip.adjusted(0, 0, -1, -1));
    }
    p->restore();
}

} // namespace kk
