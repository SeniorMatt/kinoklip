// SPDX-License-Identifier: GPL-3.0-or-later
// viewerfx implementation.
#include "ui/comp/ViewerFx.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::viewerfx {

QImage showChannel(const QImage& image, Channel channel)
{
    if (channel == Channel::Color || image.isNull())
        return image;
    const QImage src = image.convertToFormat(QImage::Format_ARGB32); // straight alpha
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            int v = 0;
            switch (channel) {
            case Channel::Red: v = qRed(in[x]); break;
            case Channel::Green: v = qGreen(in[x]); break;
            case Channel::Blue: v = qBlue(in[x]); break;
            case Channel::Alpha: v = qAlpha(in[x]); break;
            case Channel::Color: break;
            }
            o[x] = qRgb(v, v, v);
        }
    }
    return out;
}

QImage overChecker(const QImage& image, int square)
{
    if (image.isNull())
        return image;
    square = std::max(1, square);
    QImage out(image.size(), QImage::Format_ARGB32_Premultiplied);
    QPainter p(&out);
    for (int y = 0; y < out.height(); y += square)
        for (int x = 0; x < out.width(); x += square)
            p.fillRect(x, y, square, square,
                (((x / square) + (y / square)) & 1) ? QColor(90, 90, 90) : QColor(150, 150, 150));
    p.drawImage(0, 0, image);
    p.end();
    return out;
}

bool isSideA(QSize size, QPointF pos, double angleDeg, int x, int y)
{
    // Direction of the line (y down); A is on its left, so angle 90 (a vertical line) puts A
    // on the left of the picture.
    const double a = angleDeg * std::numbers::pi / 180.0;
    const double dx = std::cos(a), dy = -std::sin(a);
    const double px = x + 0.5 - pos.x() * size.width();
    const double py = y + 0.5 - pos.y() * size.height();
    return (dx * py - dy * px) < 0.0;
}

QImage splitWipe(const QImage& a, const QImage& b, QPointF pos, double angleDeg)
{
    if (a.isNull())
        return b;
    const QImage aa = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out = b.isNull() ? aa : b;
    if (out.size() != aa.size())
        out = out.scaled(aa.size());
    out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < aa.height(); ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(aa.constScanLine(y));
        QRgb* po = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < aa.width(); ++x)
            if (isSideA(aa.size(), pos, angleDeg, x, y))
                po[x] = pa[x];
    }
    return out;
}

} // namespace kk::viewerfx
