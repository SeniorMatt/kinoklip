// SPDX-License-Identifier: GPL-3.0-or-later
// DragInfo — offset label drawing.
#include "ui/timeline/DragInfo.h"

#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

#include <QFontMetrics>
#include <QPainter>
#include <QRect>

#include <algorithm>

namespace kk::draginfo {

namespace {
/// Distance of the plate from the pointer (below-right, so the cursor never covers it).
constexpr int kOffsetPx = 16;
/// Inner padding of the plate.
constexpr int kPadPx = 5;
/// Corner radius of the plate.
constexpr double kRadius = 3.0;
/// Opacity of the plate over the clips (0..255).
constexpr int kPlateAlpha = 225;
} // namespace

QString formatOffset(int64_t frames, Rational fps)
{
    const QString tc = timecode::format(std::abs(frames), fps);
    return (frames < 0 ? QStringLiteral("-") : QStringLiteral("+")) + tc;
}

void paint(QPainter& p, const DragInfoLabel& label, const QFont& font, const QRect& bounds)
{
    if (label.isEmpty())
        return;
    const SchemeColors& c = schemeColors();
    const QFontMetrics fm(font);
    int width = 0;
    for (const QString& line : label.lines)
        width = std::max(width, fm.horizontalAdvance(line));
    const int lineHeight = fm.height();
    const QSize size(width + 2 * kPadPx, int(label.lines.size()) * lineHeight + 2 * kPadPx);
    QPoint at(int(label.anchor.x()) + kOffsetPx, int(label.anchor.y()) + kOffsetPx);
    // Keep the plate inside the view: flip to the other side of the pointer when needed.
    if (at.x() + size.width() > bounds.right())
        at.setX(int(label.anchor.x()) - kOffsetPx - size.width());
    if (at.y() + size.height() > bounds.bottom())
        at.setY(int(label.anchor.y()) - kOffsetPx - size.height());
    at.setX(std::max(at.x(), bounds.left()));
    at.setY(std::max(at.y(), bounds.top()));
    const QRect plate(at, size);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c.line, 1));
    p.setBrush(withAlpha(c.overlay, kPlateAlpha));
    p.drawRoundedRect(QRectF(plate).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    p.setFont(font);
    int y = plate.top() + kPadPx + fm.ascent();
    for (int i = 0; i < label.lines.size(); ++i) {
        p.setPen(i == 0 ? c.overlayText : mixColors(c.overlayText, c.overlay, 0.3));
        p.drawText(QPoint(plate.left() + kPadPx, y), label.lines[i]);
        y += lineHeight;
    }
    p.restore();
}

} // namespace kk::draginfo
