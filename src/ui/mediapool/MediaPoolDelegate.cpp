// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolDelegate implementation.
#include "ui/mediapool/MediaPoolDelegate.h"

#include "ui/mediapool/MediaPoolModel.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace kk {

namespace {

constexpr int kPadding = 6; ///< around the poster inside the tile
constexpr int kNameHeight = 18;
const QSize kPoster = MediaPoolModel::kPosterSize;

/// Neutral placeholder for items without a poster (pending, audio, failed decode): a dark tile
/// with a simple line drawing of a film frame, a waveform or a landscape.
void paintPlaceholder(QPainter* p, const QRect& r, int kind, const QPalette& pal)
{
    p->fillRect(r, pal.color(QPalette::Base).darker(115));
    QColor ink = pal.color(QPalette::WindowText);
    ink.setAlphaF(0.35f);
    p->setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    const QPointF c = QRectF(r).center();
    switch (kind) {
    case MediaPoolModel::TimelineKind:
    case MediaPoolModel::CompoundKind: {
        // Track bars of a timeline; a compound clip frames them in a clip outline.
        const qreal w = 36;
        const qreal rows[3][2] = {{0, 20}, {8, 32}, {4, 26}};
        for (int i = 0; i < 3; ++i) {
            const qreal y = c.y() - 8 + i * 8;
            p->drawLine(
                QPointF(c.x() - w / 2 + rows[i][0], y), QPointF(c.x() - w / 2 + rows[i][1], y));
        }
        if (kind == MediaPoolModel::CompoundKind)
            p->drawRoundedRect(QRectF(c.x() - 22, c.y() - 14, 44, 28), 2, 2);
        break;
    }
    case MediaPoolModel::AudioKind: {
        // Symmetric bars like a waveform.
        const qreal heights[] = {6, 14, 22, 12, 26, 16, 8, 18, 10, 4};
        qreal x = c.x() - 4.5 * 6;
        for (qreal h : heights) {
            p->drawLine(QPointF(x, c.y() - h / 2), QPointF(x, c.y() + h / 2));
            x += 6;
        }
        break;
    }
    case MediaPoolModel::ImageKind: {
        const QRectF frame(c.x() - 18, c.y() - 13, 36, 26);
        p->drawRoundedRect(frame, 2, 2);
        QPainterPath mountain;
        mountain.moveTo(frame.left() + 3, frame.bottom() - 3);
        mountain.lineTo(frame.left() + 13, frame.top() + 11);
        mountain.lineTo(frame.left() + 20, frame.bottom() - 9);
        mountain.lineTo(frame.left() + 25, frame.top() + 14);
        mountain.lineTo(frame.right() - 3, frame.bottom() - 3);
        p->drawPath(mountain);
        p->drawEllipse(QPointF(frame.right() - 8, frame.top() + 7), 2.5, 2.5);
        break;
    }
    default: {
        // Film frame with sprocket holes.
        const QRectF frame(c.x() - 20, c.y() - 14, 40, 28);
        p->drawRoundedRect(frame, 2, 2);
        p->setBrush(ink);
        p->setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            const qreal x = frame.left() + 5 + i * 9;
            p->drawRect(QRectF(x, frame.top() + 3, 4, 3));
            p->drawRect(QRectF(x, frame.bottom() - 6, 4, 3));
        }
        break;
    }
    }
}

} // namespace

MediaPoolDelegate::MediaPoolDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{ }

QSize MediaPoolDelegate::tileSize()
{
    return {kPoster.width() + 2 * kPadding, kPoster.height() + 2 * kPadding + kNameHeight};
}

QSize MediaPoolDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return tileSize();
}

void MediaPoolDelegate::updateEditorGeometry(
    QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const
{
    // Rename in place over the name line.
    const QRect r = option.rect;
    editor->setGeometry(r.left() + 2, r.bottom() - kNameHeight - 2, r.width() - 4, kNameHeight + 2);
}

void MediaPoolDelegate::paint(
    QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const QPalette& pal = option.palette;
    const QRect tile = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    // Tile background (only when selected / hovered: keeps the grid calm).
    if (selected || hover) {
        QColor bg = selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::WindowText);
        bg.setAlphaF(selected ? 0.25f : 0.06f);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(QRectF(tile).adjusted(1, 1, -1, -1), 4, 4);
    }

    // Poster area: letterboxed pixmap centred in a fixed 16:9 rect.
    const QRect posterRect(
        tile.left() + kPadding, tile.top() + kPadding, kPoster.width(), kPoster.height());
    const QPixmap poster = index.data(MediaPoolModel::PosterRole).value<QPixmap>();
    const int kind = index.data(MediaPoolModel::KindRole).toInt();
    if (poster.isNull()) {
        paintPlaceholder(p, posterRect, kind, pal);
    } else {
        p->fillRect(posterRect, Qt::black);
        const QSize size = poster.size() / poster.devicePixelRatio();
        const QPoint topLeft(posterRect.left() + (posterRect.width() - size.width()) / 2,
            posterRect.top() + (posterRect.height() - size.height()) / 2);
        p->drawPixmap(topLeft, poster);
    }

    // Offline: red wash and label over the poster.
    const bool offline = index.data(MediaPoolModel::OfflineRole).toBool();
    if (offline) {
        p->fillRect(posterRect, QColor(0xb0, 0x30, 0x30, 140));
        QFont bold = option.font;
        bold.setBold(true);
        p->setFont(bold);
        p->setPen(Qt::white);
        p->drawText(posterRect, Qt::AlignCenter, tr("Media Offline"));
    }

    // Duration badge in the bottom-right corner of the poster.
    const QString duration = index.data(MediaPoolModel::DurationTextRole).toString();
    if (!duration.isEmpty()) {
        QFont small = option.font;
        small.setPointSizeF(small.pointSizeF() * 0.8);
        p->setFont(small);
        const QFontMetrics fm(small);
        const QRect text(0, 0, fm.horizontalAdvance(duration) + 8, fm.height() + 2);
        const QRect badge = text.translated(
            posterRect.right() - text.width() - 3, posterRect.bottom() - text.height() - 3);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 170));
        p->drawRoundedRect(QRectF(badge), 3, 3);
        p->setPen(QColor(0xee, 0xee, 0xee));
        p->drawText(badge, Qt::AlignCenter, duration);
    }

    // Proxy / optimized media badge in the top-left corner ("P", "O", "P 42%" while
    // generating, "P!" amber when stale or missing).
    const QString linkBadge = index.data(MediaPoolModel::LinkBadgeRole).toString();
    if (!linkBadge.isEmpty()) {
        QFont small = option.font;
        small.setPointSizeF(small.pointSizeF() * 0.8);
        small.setBold(true);
        p->setFont(small);
        const QFontMetrics fm(small);
        const QRect text(0, 0, fm.horizontalAdvance(linkBadge) + 8, fm.height() + 2);
        const QRect badge = text.translated(posterRect.left() + 3, posterRect.top() + 3);
        const bool problem = index.data(MediaPoolModel::LinkProblemRole).toBool();
        p->setPen(Qt::NoPen);
        p->setBrush(problem ? QColor(0xd0, 0x90, 0x20, 220) : QColor(0x2a, 0x6f, 0xc9, 220));
        p->drawRoundedRect(QRectF(badge), 3, 3);
        p->setPen(Qt::white);
        p->drawText(badge, Qt::AlignCenter, linkBadge);
    }

    // Selection outline around the poster.
    if (selected) {
        p->setPen(QPen(pal.color(QPalette::Highlight), 2));
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(posterRect).adjusted(-1, -1, 1, 1));
    }

    // Name, elided.
    p->setFont(option.font);
    const QRect nameRect(
        tile.left() + kPadding, posterRect.bottom() + 3, tile.width() - 2 * kPadding, kNameHeight);
    const QString name = option.fontMetrics.elidedText(
        index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, nameRect.width());
    p->setPen(offline ? QColor(0xe0, 0x70, 0x70)
                      : pal.color(selected ? QPalette::HighlightedText : QPalette::Text));
    p->drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter, name);
    p->restore();
}

} // namespace kk
