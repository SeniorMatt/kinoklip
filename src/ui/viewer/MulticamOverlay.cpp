// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamOverlay — angle grid chrome and input of the multicam viewer.
#include "ui/viewer/MulticamOverlay.h"

#include "core/Multicam.h"
#include "ui/viewer/ViewerSurface.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

namespace kk {

namespace {

// Frame colours of the active angle (Resolve's convention: red = both, blue = video, green =
// audio).
const QColor kBoth(0xe0, 0x3c, 0x31);
const QColor kVideo(0x3d, 0x8e, 0xf0);
const QColor kAudio(0x3c, 0xc0, 0x5a);
constexpr int kActiveFrame = 3; ///< px width of the active angle's frame
constexpr int kLabelPad = 4; ///< px around the angle label
constexpr int kArrowSize = 22; ///< px of the page arrow buttons

} // namespace

MulticamOverlay::MulticamOverlay(ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_surface(surface)
{
    setObjectName(QStringLiteral("MulticamOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    hide();
}

void MulticamOverlay::setGrid(const QStringList& names, int columns, int page)
{
    if (names == m_names && columns == m_columns && page == m_page)
        return;
    m_names = names;
    m_columns = std::clamp(columns, 1, 4);
    m_page = std::max(0, page);
    update();
}

void MulticamOverlay::setActive(int videoAngle, int audioAngle)
{
    if (videoAngle == m_video && audioAngle == m_audio)
        return;
    m_video = videoAngle;
    m_audio = audioAngle;
    update();
}

void MulticamOverlay::setMessage(const QString& text)
{
    if (text == m_message)
        return;
    m_message = text;
    update();
}

void MulticamOverlay::refresh()
{
    setGeometry(m_surface->widget()->rect());
    update();
}

int MulticamOverlay::pageCount() const
{
    const int cells = m_columns * m_columns;
    return std::max(1, (int(m_names.size()) + cells - 1) / cells);
}

QRect MulticamOverlay::cellRect(int angle) const
{
    const int cells = m_columns * m_columns;
    const int index = angle - m_page * cells;
    if (angle < 0 || angle >= m_names.size() || index < 0 || index >= cells)
        return {};
    const QRect pic = m_surface->pictureRect();
    const int col = index % m_columns;
    const int row = index / m_columns;
    // Integer edges shared by neighbours (no gaps or overlaps between cells).
    const int x0 = pic.left() + pic.width() * col / m_columns;
    const int x1 = pic.left() + pic.width() * (col + 1) / m_columns;
    const int y0 = pic.top() + pic.height() * row / m_columns;
    const int y1 = pic.top() + pic.height() * (row + 1) / m_columns;
    return QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1));
}

int MulticamOverlay::angleAt(const QPoint& pos) const
{
    const QRect pic = m_surface->pictureRect();
    if (!pic.contains(pos) || pic.width() <= 0 || pic.height() <= 0)
        return -1;
    const int col = (pos.x() - pic.left()) * m_columns / pic.width();
    const int row = (pos.y() - pic.top()) * m_columns / pic.height();
    return multicam::angleAtCell(int(m_names.size()), m_columns, m_page, col, row);
}

QRect MulticamOverlay::pageArrowRect(int delta) const
{
    const QRect pic = m_surface->pictureRect();
    const int x = pic.right() - (delta < 0 ? 2 : 1) * (kArrowSize + 4);
    return QRect(x, pic.top() + 4, kArrowSize, kArrowSize);
}

void MulticamOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont font = p.font();
    font.setBold(true);
    p.setFont(font);
    const QFontMetrics fm(font);
    if (!m_message.isEmpty()) {
        p.setPen(QColor(0xd0, 0xd0, 0xd4));
        p.drawText(m_surface->pictureRect(), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        return;
    }
    const int cells = m_columns * m_columns;
    for (int i = 0; i < cells; ++i) {
        const int angle = m_page * cells + i;
        if (angle >= m_names.size())
            break;
        const QRect cell = cellRect(angle);
        p.setPen(QColor(0, 0, 0, 200));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cell);
        if (angle == m_hover)
            p.fillRect(cell, QColor(255, 255, 255, 28));
        // Label: angle number and name on a dark plate, bottom left.
        const QString text = QStringLiteral("%1  %2").arg(angle + 1).arg(m_names[angle]);
        const QString elided = fm.elidedText(text, Qt::ElideRight, cell.width() - 4 * kLabelPad);
        const QRect plate(cell.left() + kLabelPad, cell.bottom() - fm.height() - 3 * kLabelPad,
            fm.horizontalAdvance(elided) + 2 * kLabelPad, fm.height() + 2 * kLabelPad);
        p.fillRect(plate, QColor(0, 0, 0, 150));
        p.setPen(QColor(0xf2, 0xf2, 0xf4));
        p.drawText(plate, Qt::AlignCenter, elided);
    }
    // Active angle frames last, so they are on top of the neighbours' thin frames.
    auto frame = [&](int angle, const QColor& color, int inset) {
        const QRect cell = cellRect(angle);
        if (cell.isEmpty())
            return;
        p.setPen(QPen(color, kActiveFrame, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        const int half = kActiveFrame / 2 + inset;
        p.drawRect(cell.adjusted(half, half, -half - 1, -half - 1));
    };
    if (m_video >= 0 && m_video == m_audio) {
        frame(m_video, kBoth, 0);
    } else {
        frame(m_video, kVideo, 0);
        frame(m_audio, kAudio, m_audio == m_video ? kActiveFrame : 0);
    }
    if (pageCount() > 1) {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const int delta : {-1, 1}) {
            const QRect r = pageArrowRect(delta);
            p.fillRect(r, QColor(0, 0, 0, 150));
            p.setPen(QPen(QColor(0xf2, 0xf2, 0xf4), 2));
            const QPointF c = QRectF(r).center();
            const qreal d = delta * 4.0;
            p.drawPolyline(QPolygonF({c + QPointF(-d, -6), c + QPointF(d, 0), c + QPointF(-d, 6)}));
        }
        const QString pageText = tr("Page %1 / %2").arg(m_page + 1).arg(pageCount());
        const QRect left = pageArrowRect(-1);
        const int w = fm.horizontalAdvance(pageText) + 2 * kLabelPad;
        const QRect plate(left.left() - w - 4, left.top(), w, left.height());
        p.fillRect(plate, QColor(0, 0, 0, 150));
        p.drawText(plate, Qt::AlignCenter, pageText);
    }
}

void MulticamOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_message.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    if (pageCount() > 1) {
        for (const int delta : {-1, 1}) {
            if (pageArrowRect(delta).contains(pos)) {
                emit pageRequested(delta);
                return;
            }
        }
    }
    const int angle = angleAt(pos);
    if (angle >= 0)
        emit angleClicked(angle);
    event->accept();
}

void MulticamOverlay::mouseMoveEvent(QMouseEvent* event)
{
    const int hover = m_message.isEmpty() ? angleAt(event->position().toPoint()) : -1;
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void MulticamOverlay::leaveEvent(QEvent* event)
{
    m_hover = -1;
    update();
    QWidget::leaveEvent(event);
}

bool MulticamOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        refresh();
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
