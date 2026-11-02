// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolStripDelegate — paints one row of the media pool's strip view (Cut page): the clip
// name and duration above a film strip of thumbnails spread over the whole clip.
#pragma once

#include <QImage>
#include <QStyledItemDelegate>

#include <cstdint>
#include <functional>

namespace kk {

/// Strip view row painter for MediaPoolModel items. Every row has the same height (uniform
/// item sizes); the strip shows as many thumbnails as fit the row width, taken at evenly spaced
/// times of the clip so the whole clip can be judged at a glance. Thumbnails come from a
/// provider (default: media::MediaCache, asynchronous; the view repaints on thumbnailReady).
///
/// Thread-safety: GUI thread only.
class MediaPoolStripDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// What the strip needs of a media item: file path and duration (0 = still image or none).
    struct StripSource
    {
        QString path;
        int64_t durationUs = 0;
        bool hasVideo = false;
    };
    /// Looks up the item with the given id (empty path = nothing to show).
    using SourceLookup = std::function<StripSource(const QString& mediaId)>;
    /// Thumbnail of @p path at @p timeUs, @p height px high; null while pending.
    using ThumbnailSource = std::function<QImage(const QString& path, int64_t timeUs, int height)>;

    /// Creates the delegate; @p lookup resolves item ids.
    explicit MediaPoolStripDelegate(SourceLookup lookup, QObject* parent = nullptr);

    /// Replaces the thumbnail source (default: media::MediaCache::thumbnail()).
    void setThumbnailSource(ThumbnailSource source) { m_thumbnails = std::move(source); }
    /// Fixed row height in px.
    [[nodiscard]] static int rowHeight();
    /// Times (µs) of the @p count thumbnails of a clip of @p durationUs: the middle of each of
    /// @p count equal parts (all 0 for stills).
    [[nodiscard]] static QVector<int64_t> thumbnailTimes(int64_t durationUs, int count);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(
        const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    SourceLookup m_lookup;
    ThumbnailSource m_thumbnails;
};

} // namespace kk
