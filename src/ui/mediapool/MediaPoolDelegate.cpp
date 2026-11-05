// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolDelegate implementation.
#include "ui/mediapool/MediaPoolDelegate.h"

#include "core/ClipColors.h"
#include "ui/common/SchemeColors.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace kk {

namespace {

constexpr int kPadding = 6; ///< around the poster inside the tile
constexpr int kNameHeight = 18;
constexpr int kFlagSize = 8; ///< px, flag markers on the poster
/// Narrower posters leave out the "Linked" badge (the usage dot and box glyph stay).
constexpr int kLinkedBadgeMinWidth = 112;

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
    // The glyphs are drawn for the default poster height; smaller posters shrink them.
    const qreal scale = std::min(1.0, r.height() / qreal(MediaPoolModel::kPosterSize.height()));
    if (scale < 1.0) {
        p->translate(c);
        p->scale(scale, scale);
        p->translate(-c);
    }
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
    case MediaPoolModel::FolderKind: {
        // A folder: a tab on the top left of a wide body.
        QPainterPath folder;
        const QRectF body(c.x() - 24, c.y() - 12, 48, 30);
        folder.moveTo(body.left(), body.top() - 6);
        folder.lineTo(body.left() + 16, body.top() - 6);
        folder.lineTo(body.left() + 21, body.top());
        folder.lineTo(body.right(), body.top());
        folder.lineTo(body.right(), body.bottom());
        folder.lineTo(body.left(), body.bottom());
        folder.closeSubpath();
        ink.setAlphaF(0.6f);
        p->setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QColor fill = pal.color(QPalette::Highlight);
        fill.setAlphaF(0.25f);
        p->setBrush(fill);
        p->drawPath(folder);
        break;
    }
    case MediaPoolModel::MulticamKind: {
        // A 2x2 grid of angle frames.
        for (int i = 0; i < 4; ++i) {
            const qreal x = c.x() - 21 + (i % 2) * 22;
            const qreal y = c.y() - 14 + (i / 2) * 15;
            p->drawRoundedRect(QRectF(x, y, 20, 13), 1.5, 1.5);
        }
        break;
    }
    case MediaPoolModel::SubtitleKind: {
        // A caption box with two text lines.
        p->drawRoundedRect(QRectF(c.x() - 20, c.y() - 14, 40, 28), 3, 3);
        p->drawLine(QPointF(c.x() - 12, c.y() + 1), QPointF(c.x() + 12, c.y() + 1));
        p->drawLine(QPointF(c.x() - 8, c.y() + 7), QPointF(c.x() + 8, c.y() + 7));
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
    , m_poster(MediaPoolModel::kPosterSize)
{ }

QSize MediaPoolDelegate::tileSize(QSize poster)
{
    return {poster.width() + 2 * kPadding, poster.height() + 2 * kPadding + kNameHeight};
}

QRect MediaPoolDelegate::nameRect(const QRect& tile)
{
    // Below the poster: the last kNameHeight px above the bottom padding (+2 px gap).
    return {tile.left() + kPadding, tile.top() + tile.height() - kPadding - kNameHeight + 2,
        tile.width() - 2 * kPadding, kNameHeight};
}

QSize MediaPoolDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return tileSize(m_poster);
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
    // Badges and plates over the poster use the scheme's complementary (dark) set; offline in
    // negative, proxy badges in link / neutral. Flag and clip colours are user data (fixed).
    const SchemeColors& sc = schemeColors();
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
        tile.left() + kPadding, tile.top() + kPadding, m_poster.width(), m_poster.height());
    const QPixmap poster = index.data(MediaPoolModel::PosterRole).value<QPixmap>();
    const int kind = index.data(MediaPoolModel::KindRole).toInt();
    if (poster.isNull()) {
        p->save(); // the glyph may scale the painter
        paintPlaceholder(p, posterRect, kind, pal);
        p->restore();
    } else {
        p->fillRect(posterRect, Qt::black); // letterbox of the poster: video content
        const QSize size = poster.size() / poster.devicePixelRatio();
        const QPoint topLeft(posterRect.left() + (posterRect.width() - size.width()) / 2,
            posterRect.top() + (posterRect.height() - size.height()) / 2);
        p->save();
        p->setClipRect(posterRect); // a poster of the previous size, until it is re-scaled
        p->drawPixmap(topLeft, poster);
        p->restore();
    }

    // Offline: red wash and label over the poster.
    const bool offline = index.data(MediaPoolModel::OfflineRole).toBool();
    if (offline) {
        p->fillRect(posterRect, withAlpha(sc.negative.darker(130), 140));
        QFont bold = option.font;
        bold.setBold(true);
        p->setFont(bold);
        p->setPen(sc.overlayText);
        p->drawText(posterRect, Qt::AlignCenter,
            QFontMetrics(bold).elidedText(
                tr("Media Offline"), Qt::ElideRight, posterRect.width() - 4));
    }

    // Duration badge in the bottom-right corner of the poster; left out when the poster is
    // too small for it (zoomed-out grid).
    const QString duration = index.data(MediaPoolModel::DurationTextRole).toString();
    if (!duration.isEmpty()) {
        QFont badgeFont = option.font;
        badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.8);
        const QFontMetrics fm(badgeFont);
        const QRect text(0, 0, fm.horizontalAdvance(duration) + 8, fm.height() + 2);
        if (text.width() <= posterRect.width() - 6 && text.height() * 2 <= posterRect.height()) {
            const QRect badge = text.translated(
                posterRect.right() - text.width() - 3, posterRect.bottom() - text.height() - 3);
            p->setFont(badgeFont);
            p->setPen(Qt::NoPen);
            p->setBrush(sc.overlay);
            p->drawRoundedRect(QRectF(badge), 3, 3);
            p->setPen(sc.overlayText);
            p->drawText(badge, Qt::AlignCenter, duration);
        }
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
        const QColor badgeColor = withAlpha(problem ? sc.neutral : sc.link, 220);
        p->setBrush(badgeColor);
        p->drawRoundedRect(QRectF(badge), 3, 3);
        p->setPen(sc.textOn(badgeColor));
        p->drawText(badge, Qt::AlignCenter, linkBadge);
    }

    // Flags: small squares along the top-right edge of the poster (one per flag, Resolve
    // shows flags as coloured markers on the thumbnail).
    const uint flags = index.data(MediaPoolModel::FlagsRole).toUInt();
    if (flags != 0) {
        int x = posterRect.right() - kFlagSize - 3;
        for (int i = int(flagPalette().size()) - 1; i >= 0 && x > posterRect.left(); --i) {
            if (!(flags & (1u << i)))
                continue;
            const QRectF mark(x, posterRect.top() + 3, kFlagSize, kFlagSize);
            p->setPen(QPen(withAlpha(sc.shadow, 160), 1));
            p->setBrush(flagColor(i));
            p->drawRoundedRect(mark, 1.5, 1.5);
            x -= kFlagSize + 2;
        }
    }

    // Usage: a small dot in the bottom-left corner of items used in a timeline.
    if (index.data(MediaPoolModel::UsageRole).toInt() > 0) {
        p->setPen(QPen(withAlpha(sc.shadow, 170), 1));
        p->setBrush(sc.overlayText);
        p->drawEllipse(QPointF(posterRect.left() + 7.5, posterRect.bottom() - 6.5), 3.0, 3.0);
    }

    // Where the media is, in a single-file project (where both can be mixed): a small box
    // glyph for media stored in the project file, a "Linked" badge for files on disk. Next to
    // the usage dot in the bottom-left corner.
    const int location = index.data(MediaPoolModel::LocationBadgeRole).toInt();
    if (location == MediaPoolModel::StoredLocation) {
        const QRectF box(posterRect.left() + 14.5, posterRect.bottom() - 11.5, 9, 8);
        p->setPen(QPen(withAlpha(sc.shadow, 170), 1));
        p->setBrush(withAlpha(sc.overlay, 200));
        p->drawRoundedRect(box.adjusted(-2, -2, 2, 2), 2, 2);
        p->setPen(QPen(sc.overlayText, 1.2));
        p->setBrush(Qt::NoBrush);
        p->drawRect(box);
        p->drawLine(QPointF(box.left(), box.top() + 3), QPointF(box.right(), box.top() + 3));
    } else if (location == MediaPoolModel::LinkedLocation
        && posterRect.width() >= kLinkedBadgeMinWidth) {
        QFont small = option.font;
        small.setPointSizeF(small.pointSizeF() * 0.75);
        p->setFont(small);
        const QFontMetrics fm(small);
        const QString text = tr("Linked");
        const QRect r(0, 0, fm.horizontalAdvance(text) + 8, fm.height() + 2);
        const QRect badge
            = r.translated(posterRect.left() + 13, posterRect.bottom() - r.height() - 3);
        const QColor color = withAlpha(sc.neutral, 220);
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawRoundedRect(QRectF(badge), 3, 3);
        p->setPen(sc.textOn(color));
        p->drawText(badge, Qt::AlignCenter, text);
    }

    // Selection outline around the poster.
    if (selected) {
        p->setPen(QPen(pal.color(QPalette::Highlight), 2));
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(posterRect).adjusted(-1, -1, 1, 1));
    }

    // Name, elided, on the clip colour when there is one (Resolve colours the name label).
    p->setFont(option.font);
    const QRect nameRect = MediaPoolDelegate::nameRect(tile);
    const QColor clipColor = index.data(MediaPoolModel::ClipColorRole).value<QColor>();
    if (clipColor.isValid()) {
        p->setPen(Qt::NoPen);
        p->setBrush(clipColor);
        p->drawRoundedRect(QRectF(nameRect).adjusted(0, 1, 0, -1), 3, 3);
    }
    const QString name = option.fontMetrics.elidedText(
        index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, nameRect.width());
    p->setPen(offline ? sc.negative
            : clipColor.isValid()
            ? sc.textOn(clipColor)
            : pal.color(selected ? QPalette::HighlightedText : QPalette::Text));
    p->drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter, name);
    p->restore();
}

} // namespace kk
