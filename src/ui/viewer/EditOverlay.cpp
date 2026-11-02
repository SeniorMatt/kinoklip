// SPDX-License-Identifier: GPL-3.0-or-later
// EditOverlay — edit type tiles over the timeline viewer.
#include "ui/viewer/EditOverlay.h"

#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerSurface.h"

#include <QEvent>
#include <QPainter>

#include <algorithm>

namespace kk {

namespace {
/// Space between tiles and around them (px).
constexpr int kGap = 8;
/// Tiles in the first row; the rest go into the second.
constexpr int kFirstRow = 4;
/// Corner radius of a tile.
constexpr double kRadius = 4.0;
/// Opacity of the tiles over the picture (0..255): normal and hovered.
constexpr int kTileAlpha = 200;
constexpr int kHoverAlpha = 235;
/// Dimming of the picture behind the tiles (0..255).
constexpr int kDimAlpha = 110;
} // namespace

EditOverlay::EditOverlay(ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_surface(surface)
{
    setObjectName(QStringLiteral("EditOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    // Drag events go to the surface underneath and from there to the viewer panel.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    hide();
}

void EditOverlay::setTiles(const QVector<Tile>& tiles)
{
    m_tiles = tiles;
    m_hover = -1;
    update();
}

void EditOverlay::setHover(const QPoint& pos)
{
    const int hover = tileAt(pos);
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

QRect EditOverlay::tileRect(int index) const
{
    const int count = int(m_tiles.size());
    if (index < 0 || index >= count)
        return {};
    const int firstRow = std::min(count, kFirstRow);
    const int secondRow = count - firstRow;
    const int rows = secondRow > 0 ? 2 : 1;
    const int row = index < firstRow ? 0 : 1;
    const int inRow = row == 0 ? firstRow : secondRow;
    const int col = row == 0 ? index : index - firstRow;
    const int h = (height() - (rows + 1) * kGap) / rows;
    const int w = (width() - (inRow + 1) * kGap) / inRow;
    return QRect(kGap + col * (w + kGap), kGap + row * (h + kGap), std::max(w, 1), std::max(h, 1));
}

int EditOverlay::tileAt(const QPoint& pos) const
{
    for (int i = 0; i < int(m_tiles.size()); ++i)
        if (tileRect(i).contains(pos))
            return i;
    return -1;
}

void EditOverlay::refresh()
{
    setGeometry(m_surface->widget()->rect());
    update();
}

bool EditOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        refresh();
    return QWidget::eventFilter(watched, event);
}

void EditOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const SchemeColors& c = schemeColors();
    p.fillRect(rect(), withAlpha(c.shadow, kDimAlpha));
    QFont title = font();
    title.setBold(true);
    const QFont keyFont = font();
    for (int i = 0; i < int(m_tiles.size()); ++i) {
        const QRectF r = QRectF(tileRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool hot = i == m_hover;
        p.setPen(QPen(hot ? c.focus : c.line, hot ? 2.0 : 1.0));
        p.setBrush(hot ? withAlpha(mixColors(c.overlay, c.focus, 0.35), kHoverAlpha)
                       : withAlpha(c.overlay, kTileAlpha));
        p.drawRoundedRect(r, kRadius, kRadius);
        const Tile& t = m_tiles[i];
        const QFontMetrics fm(title);
        const int lineH = fm.height();
        const QRectF labelRect(r.left(), r.center().y() - lineH, r.width(), lineH);
        p.setFont(title);
        p.setPen(c.overlayText);
        p.drawText(
            labelRect, Qt::AlignCenter, fm.elidedText(t.label, Qt::ElideRight, int(r.width()) - 8));
        if (!t.key.isEmpty()) {
            p.setFont(keyFont);
            p.setPen(mixColors(c.overlayText, c.overlay, 0.35));
            p.drawText(
                QRectF(r.left(), r.center().y() + 2, r.width(), lineH), Qt::AlignCenter, t.key);
        }
    }
}

} // namespace kk
