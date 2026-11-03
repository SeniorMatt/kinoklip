// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/ThinProgressBar.h"

#include <QPainter>

#include <algorithm>

namespace kk {

namespace {
constexpr int kHeight = 6; ///< bar thickness (px)
} // namespace

ThinProgressBar::ThinProgressBar(QWidget* parent)
    : QProgressBar(parent)
{
    setTextVisible(false);
    setFixedHeight(kHeight);
}

double ThinProgressBar::fraction() const
{
    const double span = double(maximum()) - double(minimum());
    if (span <= 0)
        return 0.0;
    return std::clamp((double(value()) - double(minimum())) / span, 0.0, 1.0);
}

QSize ThinProgressBar::sizeHint() const
{
    return {120, kHeight};
}

QSize ThinProgressBar::minimumSizeHint() const
{
    return {kHeight * 2, kHeight};
}

void ThinProgressBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const QRectF r = QRectF(rect());
    const double radius = r.height() / 2;
    QColor groove = palette().color(QPalette::WindowText);
    groove.setAlphaF(0.18);
    p.setBrush(groove);
    p.drawRoundedRect(r, radius, radius);
    const double f = fraction();
    if (f <= 0.0)
        return;
    QRectF fill = r;
    fill.setWidth(std::max(r.height(), r.width() * f)); // never thinner than round
    p.setBrush(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
        QPalette::Highlight));
    p.drawRoundedRect(fill, radius, radius);
}

} // namespace kk
