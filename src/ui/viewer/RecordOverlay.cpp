// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/viewer/RecordOverlay.h"

#include "ui/audio/VoiceOverController.h"
#include "ui/viewer/ViewerSurface.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPainter>

#include <algorithm>

namespace kk {

RecordOverlay::RecordOverlay(VoiceOverController* controller, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_controller(controller)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("RecordOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    hide();
    connect(controller, &VoiceOverController::stateChanged, this, [this] { sync(); });
    connect(controller, &VoiceOverController::countInTick, this, [this] { sync(); });
}

void RecordOverlay::sync()
{
    m_count = m_controller->state() == VoiceOverController::State::CountIn
        ? m_controller->countInRemaining()
        : 0;
    m_recording = m_controller->state() == VoiceOverController::State::Recording;
    setVisible(m_count > 0 || m_recording);
    if (isVisible()) {
        setGeometry(m_surface->widget()->rect());
        raise();
        update();
    }
}

bool RecordOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

void RecordOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (m_count > 0) {
        const int side = std::min(width(), height());
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(std::max(24, side / 3));
        p.setFont(f);
        const QRect disc(
            width() / 2 - side / 5, height() / 2 - side / 5, 2 * side / 5, 2 * side / 5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawEllipse(disc);
        p.setPen(Qt::white);
        p.drawText(disc, Qt::AlignCenter, QString::number(m_count));
    }
    if (m_recording) {
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        const QRect box(12, 12, 74, 26);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 4, 4);
        p.setBrush(QColor(0xe0, 0x30, 0x30));
        p.drawEllipse(QPoint(box.left() + 14, box.center().y()), 6, 6);
        p.setPen(Qt::white);
        p.drawText(box.adjusted(26, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
            QCoreApplication::translate("kk::RecordOverlay", "REC"));
    }
}

} // namespace kk
