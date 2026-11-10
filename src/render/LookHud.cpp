// SPDX-License-Identifier: GPL-3.0-or-later
// LookHud — see LookHud.h. Sizes are written for a 1080 pixel high picture and scaled by
// content height / 1080 (the "unit" u), so a look has the same proportions at any resolution.
#include "render/LookHud.h"

#include "core/VideoEffectKeys.h"

#include <QDateTime>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace kk::render::lookhud {

namespace {

namespace k = fxkeys;
namespace c = fxchoice;

constexpr int kBlinkHalfPeriods = 2; ///< lit/dark changes per second

double num(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

QString str(const ParamSet& p, const QString& key, const QString& fallback)
{
    const QVariant v = p.staticValue(key);
    return v.isValid() ? v.toString() : fallback;
}

bool flag(const ParamSet& p, const QString& key, bool fallback)
{
    const QVariant v = p.staticValue(key);
    return v.isValid() ? v.toBool() : fallback;
}

QColor colorOf(const ParamSet& p, const QString& key, const QColor& fallback)
{
    const QVariant v = p.staticValue(key);
    if (v.metaType().id() == QMetaType::QColor)
        return v.value<QColor>();
    if (v.isValid() && QColor::isValidColorName(v.toString()))
        return QColor::fromString(v.toString());
    return fallback;
}

/// Drawing state shared by the helpers of one overlay.
struct Ctx
{
    QPainter& g;
    QRectF r; ///< the picture
    double u = 1.0; ///< design unit: picture height / 1080, times Text Size
    double ui = 1.0; ///< design unit that ignores Text Size (marks, icons)
    QColor color;
    QString family;
    const ParamSet& p;
    const Frame& f;

    QPointF at(double x, double y) const { return {r.left() + x * ui, r.top() + y * ui}; }
    QPointF fromRight(double x, double y) const { return {r.right() - x * ui, r.top() + y * ui}; }
    QPointF fromBottom(double x, double y) const
    {
        return {r.left() + x * ui, r.bottom() - y * ui};
    }
    QPointF fromCorner(double x, double y) const
    {
        return {r.right() - x * ui, r.bottom() - y * ui};
    }
};

QColor withAlpha(QColor col, double alpha)
{
    col.setAlphaF(std::clamp(col.alphaF() * alpha, 0.0, 1.0));
    return col;
}

QFont fontFor(const Ctx& c, double px, bool bold)
{
    QFont font(c.family);
    font.setPixelSize(std::max(4, int(std::lround(px))));
    font.setBold(bold);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

/// Draws @p s with its anchor point at @p anchor (alignment @p al horizontally, centred
/// vertically) and a soft dark edge so it reads on any picture. @return the width drawn.
double drawText(Ctx& c, QPointF anchor, const QString& s, double designPx,
    Qt::Alignment al = Qt::AlignLeft, bool bold = true, const QColor& col = QColor())
{
    const double px = designPx * c.u;
    const QFont font = fontFor(c, px, bold);
    const QFontMetricsF fm(font);
    const double w = fm.horizontalAdvance(s);
    double x = anchor.x();
    if (al & Qt::AlignRight)
        x -= w;
    else if (al & Qt::AlignHCenter)
        x -= w / 2;
    const double baseline = anchor.y() + (fm.ascent() - fm.descent()) / 2.0;
    QPainterPath path;
    path.addText(QPointF(x, baseline), font, s);
    c.g.setFont(font);
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(QColor(0, 0, 0, 120));
    c.g.save();
    c.g.translate(px * 0.05, px * 0.05);
    c.g.drawPath(path);
    c.g.restore();
    c.g.setBrush(col.isValid() ? col : c.color);
    c.g.drawPath(path);
    return w;
}

QPen linePen(const Ctx& c, double widthDesign, const QColor& col = QColor())
{
    QPen pen(col.isValid() ? col : c.color, std::max(1.6, widthDesign * c.ui));
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::MiterJoin);
    return pen;
}

/// A dark edge under a line so thin white lines stay visible on bright pictures.
void strokeLine(Ctx& c, QPointF a, QPointF b, double widthDesign, const QColor& col = QColor())
{
    QPen dark = linePen(c, widthDesign * 1.9, QColor(0, 0, 0, 90));
    c.g.setPen(dark);
    c.g.drawLine(a, b);
    c.g.setPen(linePen(c, widthDesign, col));
    c.g.drawLine(a, b);
}

/// Four L-shaped corner marks of the rectangle @p r, @p len long.
void cornerMarks(Ctx& c, const QRectF& r, double lenDesign, double widthDesign)
{
    const double len = lenDesign * c.ui;
    const struct
    {
        QPointF corner;
        int dx, dy;
    } corners[] = {{r.topLeft(), 1, 1}, {r.topRight(), -1, 1}, {r.bottomLeft(), 1, -1},
        {r.bottomRight(), -1, -1}};
    for (const auto& k : corners) {
        strokeLine(c, k.corner, k.corner + QPointF(k.dx * len, 0), widthDesign);
        strokeLine(c, k.corner, k.corner + QPointF(0, k.dy * len), widthDesign);
    }
}

void filledCircle(Ctx& c, QPointF centre, double radiusDesign, const QColor& col)
{
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(col);
    c.g.drawEllipse(centre, radiusDesign * c.ui, radiusDesign * c.ui);
}

/// Battery icon with its left end at @p topLeft; @p level percent fills it.
void batteryIcon(Ctx& c, QPointF topLeft, double level, double scale = 1.0)
{
    const double w = 62 * c.ui * scale, h = 30 * c.ui * scale, line = 3 * c.ui * scale;
    const QRectF body(topLeft, QSizeF(w, h));
    c.g.setBrush(Qt::NoBrush);
    c.g.setPen(QPen(QColor(0, 0, 0, 90), line * 1.9));
    c.g.drawRoundedRect(body, 4 * c.ui * scale, 4 * c.ui * scale);
    c.g.setPen(QPen(c.color, line));
    c.g.drawRoundedRect(body, 4 * c.ui * scale, 4 * c.ui * scale);
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(c.color);
    c.g.drawRect(
        QRectF(body.right() + line, body.center().y() - h * 0.2, 5 * c.ui * scale, h * 0.4));
    const double fill = std::clamp(level / 100.0, 0.0, 1.0);
    c.g.setBrush(level <= 15.0 ? QColor(235, 70, 60) : c.color);
    const double inset = line * 1.8;
    c.g.drawRect(
        QRectF(body.left() + inset, body.top() + inset, (w - 2 * inset) * fill, h - 2 * inset));
}

QString monoFamily(const ParamSet& p)
{
    const QString chosen = str(p, k::LookFont, QString());
    return chosen.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : chosen;
}

QString sansFamily(const ParamSet& p)
{
    const QString chosen = str(p, k::LookFont, QString());
    return chosen.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()
                            : chosen;
}

double seconds(const Frame& f)
{
    return f.fps > 0 ? double(f.localFrame) / f.fps : 0.0;
}

// ---- CCTV ----

void paintCctv(Ctx& c)
{
    const int64_t fr = c.f.localFrame;
    cornerMarks(c, c.r.adjusted(40 * c.ui, 40 * c.ui, -40 * c.ui, -40 * c.ui), 64, 3);
    drawText(c, c.at(76, 82), str(c.p, k::CameraName, QStringLiteral("CAM 01")), 42);
    if (flag(c.p, k::RecDot, true)) {
        const QPointF anchor = c.fromRight(76, 82);
        const double w = drawText(c, anchor, QStringLiteral("REC"), 42, Qt::AlignRight);
        if (blinkOn(fr, c.f.fps))
            filledCircle(
                c, QPointF(anchor.x() - w - 30 * c.ui, anchor.y()), 14, QColor(235, 40, 40));
    }
    drawText(c, c.fromBottom(76, 82), cctvTimestamp(c.p, fr, c.f.fps), 42);
}

// ---- Drone Overlay ----

QString lengthText(double metres, bool imperial)
{
    return imperial ? QStringLiteral("%1 ft").arg(metres * 3.28084, 0, 'f', 1)
                    : QStringLiteral("%1 m").arg(metres, 0, 'f', 1);
}

void compassBar(Ctx& c, double heading)
{
    const double spanDeg = 90.0; // shown across the bar
    const double halfWidth = 300 * c.ui;
    const double cx = c.r.center().x(), y = c.r.top() + 78 * c.ui;
    const double perDeg = 2 * halfWidth / spanDeg;
    const int first = int(std::floor(heading - spanDeg / 2.0));
    const int last = int(std::ceil(heading + spanDeg / 2.0));
    for (int d = first; d <= last; ++d) {
        if (d % 5 != 0)
            continue;
        const double x = cx + (d - heading) * perDeg;
        const bool major = d % 15 == 0;
        const double fade = 1.0 - std::pow(std::abs(x - cx) / halfWidth, 4.0);
        if (fade <= 0.0)
            continue;
        const QColor col = withAlpha(c.color, fade);
        strokeLine(
            c, QPointF(x, y), QPointF(x, y + (major ? 22 : 12) * c.ui), major ? 3.5 : 2.5, col);
        if (major) {
            const int norm = ((d % 360) + 360) % 360;
            static const char* const names[] = {"N", "E", "S", "W"};
            const QString label
                = norm % 90 == 0 ? QString::fromLatin1(names[norm / 90]) : QString::number(norm);
            drawText(c, QPointF(x, y + 46 * c.ui), label, norm % 90 == 0 ? 34 : 26,
                Qt::AlignHCenter, true, col);
        }
    }
    QPainterPath marker;
    marker.moveTo(cx - 9 * c.ui, y - 20 * c.ui);
    marker.lineTo(cx + 9 * c.ui, y - 20 * c.ui);
    marker.lineTo(cx, y - 4 * c.ui);
    marker.closeSubpath();
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(c.color);
    c.g.drawPath(marker);
}

void paintDrone(Ctx& c)
{
    const int64_t fr = c.f.localFrame;
    const bool imperial = str(c.p, k::Units, c::Metric) == c::Imperial;
    if (flag(c.p, k::ShowMarks, true))
        cornerMarks(c, c.r.adjusted(36 * c.ui, 36 * c.ui, -36 * c.ui, -36 * c.ui), 80, 3);
    if (flag(c.p, k::ShowCompass, true))
        compassBar(c, num(c.p, k::Heading, fr, 45));
    // Artificial horizon: shifts with the gimbal pitch, broken in the middle around a small cross.
    {
        const double pitch = num(c.p, k::GimbalPitch, fr, -12);
        const double y = c.r.center().y() + pitch * 9 * c.ui;
        const double half = 300 * c.ui, gap = 70 * c.ui;
        if (y > c.r.top() + 140 * c.ui && y < c.r.bottom() - 140 * c.ui) {
            for (const int side : {-1, 1}) {
                strokeLine(c, QPointF(c.r.center().x() + side * gap, y),
                    QPointF(c.r.center().x() + side * half, y), 2);
                strokeLine(c, QPointF(c.r.center().x() + side * half, y),
                    QPointF(c.r.center().x() + side * half, y + 14 * c.ui), 2);
            }
        }
        const QPointF m = c.r.center();
        const double a = 16 * c.ui, b = 6 * c.ui;
        for (const QPointF d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)})
            strokeLine(c, m + d * b, m + d * a, 3);
    }
    // Left readouts.
    const struct
    {
        const char* label;
        QString value;
    } left[] = {
        {"ALT", lengthText(num(c.p, k::Altitude, fr, 120), imperial)},
        {"H.S",
            imperial ? QStringLiteral("%1 mph").arg(
                           num(c.p, k::GroundSpeed, fr, 12.5) * 2.23694, 0, 'f', 1)
                     : QStringLiteral("%1 m/s").arg(num(c.p, k::GroundSpeed, fr, 12.5), 0, 'f', 1)},
        {"DIST", lengthText(num(c.p, k::HomeDistance, fr, 340), imperial)},
    };
    double y = 170;
    for (const auto& row : left) {
        drawText(c, c.at(76, y), QString::fromLatin1(row.label), 26, Qt::AlignLeft, true,
            withAlpha(c.color, 0.8));
        drawText(c, c.at(176, y), row.value, 36, Qt::AlignLeft);
        y += 52;
    }
    // Top left: recording time. Top right: pitch.
    filledCircle(
        c, c.at(88, 84), 10, blinkOn(fr, c.f.fps) ? QColor(235, 40, 40) : QColor(120, 30, 30));
    drawText(c, c.at(112, 84), elapsedText(fr, c.f.fps), 32);
    drawText(c, c.fromRight(76, 84),
        QStringLiteral("%1°").arg(num(c.p, k::GimbalPitch, fr, -12), 0, 'f', 1), 32,
        Qt::AlignRight);
    // Bottom right: battery and satellites.
    const double battery = num(c.p, k::BatteryLevel, fr, 76);
    batteryIcon(c, c.fromCorner(76 + 62 + 115, 30 + 14), battery);
    drawText(c, c.fromCorner(76, 30), QStringLiteral("%1%").arg(int(std::lround(battery))), 32,
        Qt::AlignRight);
    drawText(c, c.fromCorner(76, 86),
        QStringLiteral("GPS  %1").arg(int(std::lround(num(c.p, k::Satellites, fr, 14)))), 32,
        Qt::AlignRight);
    // Bottom left: home mark.
    drawText(c, c.fromBottom(76, 56),
        QStringLiteral("HOME  %1°").arg(int(std::lround(num(c.p, k::Heading, fr, 45) + 180)) % 360),
        32);
}

// ---- DSLR ----

void paintDslr(Ctx& c)
{
    const int64_t fr = c.f.localFrame;
    const QString grid = str(c.p, k::GridStyle, c::GridThirds);
    const QRectF frameRect = c.r.adjusted(60 * c.ui, 50 * c.ui, -60 * c.ui, -150 * c.ui);
    const QColor faint = withAlpha(c.color, 0.5);
    if (grid != c::GridOff) {
        const int cols = grid == c::GridThirds ? 3 : 6, rows = grid == c::GridThirds ? 3 : 4;
        for (int i = 1; i < cols; ++i) {
            const double x = frameRect.left() + frameRect.width() * i / cols;
            strokeLine(c, QPointF(x, frameRect.top()), QPointF(x, frameRect.bottom()), 1.5, faint);
        }
        for (int i = 1; i < rows; ++i) {
            const double y = frameRect.top() + frameRect.height() * i / rows;
            strokeLine(c, QPointF(frameRect.left(), y), QPointF(frameRect.right(), y), 1.5, faint);
        }
    }
    cornerMarks(c, frameRect, 70, 3);
    // Focus point array: small dim rectangles in a diamond pattern.
    const QPointF centre = frameRect.center();
    c.g.setBrush(Qt::NoBrush);
    c.g.setPen(linePen(c, 2.5, withAlpha(c.color, 0.7)));
    const double dx = 150 * c.ui, dy = 110 * c.ui;
    for (int row = -1; row <= 1; ++row) {
        const int count = row == 0 ? 5 : 3;
        for (int i = 0; i < count; ++i) {
            const double x = centre.x() + (i - (count - 1) / 2.0) * dx;
            c.g.drawRect(
                QRectF(x - 13 * c.ui, centre.y() + row * dy - 10 * c.ui, 26 * c.ui, 20 * c.ui));
        }
    }
    // The active focus frame: white while searching, green once locked.
    const QPointF focus(
        centre.x() + num(c.p, k::FocusX, fr, 0) / 100.0 * frameRect.width() / 2.0 * 0.9,
        centre.y() + num(c.p, k::FocusY, fr, 0) / 100.0 * frameRect.height() / 2.0 * 0.9);
    const bool locked = flag(c.p, k::FocusLocked, true);
    const QColor focusColor = locked ? QColor(80, 235, 120) : c.color;
    const QRectF box(focus.x() - 62 * c.ui, focus.y() - 46 * c.ui, 124 * c.ui, 92 * c.ui);
    {
        const double len = 30 * c.ui;
        const struct
        {
            QPointF corner;
            int dx, dy;
        } corners[] = {{box.topLeft(), 1, 1}, {box.topRight(), -1, 1}, {box.bottomLeft(), 1, -1},
            {box.bottomRight(), -1, -1}};
        c.g.setBrush(Qt::NoBrush);
        c.g.setPen(linePen(c, 4, focusColor));
        for (const auto& k : corners) {
            c.g.drawLine(k.corner, k.corner + QPointF(k.dx * len, 0));
            c.g.drawLine(k.corner, k.corner + QPointF(0, k.dy * len));
        }
        c.g.setPen(Qt::NoPen);
        c.g.setBrush(focusColor);
        c.g.drawRect(QRectF(focus.x() - 6 * c.ui, focus.y() - 6 * c.ui, 12 * c.ui, 12 * c.ui));
    }
    // Information bar.
    const QRectF bar(c.r.left(), c.r.bottom() - 124 * c.ui, c.r.width(), 124 * c.ui);
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(QColor(0, 0, 0, 150));
    c.g.drawRect(bar);
    const double my = bar.top() + 38 * c.ui;
    drawText(c, QPointF(c.r.left() + 76 * c.ui, my),
        str(c.p, k::ShutterSpeed, QStringLiteral("1/250")), 44);
    drawText(c, QPointF(c.r.left() + 300 * c.ui, my),
        QStringLiteral("F%1").arg(num(c.p, k::Aperture, fr, 2.8), 0, 'f', 1), 44);
    drawText(c, QPointF(c.r.left() + 520 * c.ui, my),
        QStringLiteral("ISO %1").arg(int(std::lround(num(c.p, k::Iso, fr, 400)))), 44);
    drawText(c, QPointF(c.r.right() - 76 * c.ui, my),
        QStringLiteral("[%1]").arg(int(std::lround(num(c.p, k::ShotsLeft, fr, 1024)))), 44,
        Qt::AlignRight);
    // Exposure scale -3 .. +3 with a marker at the compensation.
    const double sy = bar.top() + 92 * c.ui;
    const double sx0 = c.r.center().x() - 330 * c.ui, step = 110 * c.ui;
    for (int i = -3; i <= 3; ++i) {
        const double x = sx0 + (i + 3) * step;
        strokeLine(c, QPointF(x, sy - (i % 3 == 0 ? 14 : 8) * c.ui), QPointF(x, sy), 2.5);
        if (i % 3 == 0) {
            drawText(c, QPointF(x, sy - 30 * c.ui),
                i == 0 ? QStringLiteral("0") : (i < 0 ? QStringLiteral("-") : QStringLiteral("+")),
                24, Qt::AlignHCenter);
        }
    }
    const double ev = std::clamp(num(c.p, k::ExposureComp, fr, 0), -3.0, 3.0);
    const double mx = sx0 + (ev + 3) * step;
    QPainterPath tri;
    tri.moveTo(mx - 10 * c.ui, sy + 22 * c.ui);
    tri.lineTo(mx + 10 * c.ui, sy + 22 * c.ui);
    tri.lineTo(mx, sy + 6 * c.ui);
    tri.closeSubpath();
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(c.color);
    c.g.drawPath(tri);
    const QString mode = str(c.p, k::FocusMode, c::AfSingle);
    drawText(c, QPointF(c.r.left() + 76 * c.ui, bar.top() + 92 * c.ui), mode, 34, Qt::AlignLeft,
        true, withAlpha(c.color, 0.85));
    batteryIcon(c, QPointF(c.r.right() - (76 + 62) * c.ui, bar.top() + 76 * c.ui), 80, 0.8);
}

// ---- Video Call ----

void paintVideoCall(Ctx& c)
{
    const int64_t fr = c.f.localFrame;
    const double margin = num(c.p, k::TileMargin, fr, 48) * c.f.scale;
    const double radius = num(c.p, k::CornerRadius, fr, 28) * c.f.scale;
    const QRectF tile = c.r.adjusted(margin, margin, -margin, -margin);
    const bool speaking = flag(c.p, k::Speaking, false);
    // Frame ring: a thin neutral edge, or a thick glowing one while speaking.
    c.g.setBrush(Qt::NoBrush);
    if (speaking) {
        const QColor ring = colorOf(c.p, k::Color, QColor(0x3d, 0xd6, 0x8c));
        for (int i = 3; i >= 1; --i) {
            c.g.setPen(QPen(withAlpha(ring, 0.12 * (4 - i)), (5 + i * 5) * c.ui));
            c.g.drawRoundedRect(tile, radius, radius);
        }
        c.g.setPen(QPen(ring, 6 * c.ui));
    } else {
        c.g.setPen(QPen(QColor(255, 255, 255, 45), 2 * c.ui));
    }
    c.g.drawRoundedRect(tile, radius, radius);
    // Name label: a dark pill at the lower left of the tile.
    const QString name = str(c.p, k::ParticipantName, QStringLiteral("Alex Morgan"));
    const bool muted = flag(c.p, k::Muted, false);
    const double px = 30 * c.u;
    const QFontMetricsF fm(fontFor(c, px, false));
    const double padX = 18 * c.u, h = 54 * c.u;
    const double iconW = 34 * c.u;
    const double w = fm.horizontalAdvance(name) + 2 * padX + iconW;
    const QRectF pill(tile.left() + 22 * c.ui, tile.bottom() - 22 * c.ui - h, w, h);
    c.g.setPen(Qt::NoPen);
    c.g.setBrush(QColor(12, 14, 18, 175));
    c.g.drawRoundedRect(pill, h / 2, h / 2);
    // Microphone mark (crossed out and red when muted).
    const QPointF mic(pill.left() + padX + 9 * c.u, pill.center().y());
    const QColor micColor = muted ? QColor(240, 90, 80) : QColor(235, 235, 235);
    c.g.setBrush(micColor);
    c.g.drawRoundedRect(
        QRectF(mic.x() - 5 * c.u, mic.y() - 12 * c.u, 10 * c.u, 17 * c.u), 5 * c.u, 5 * c.u);
    c.g.setBrush(Qt::NoBrush);
    c.g.setPen(QPen(micColor, 2.2 * c.u));
    c.g.drawArc(
        QRectF(mic.x() - 9 * c.u, mic.y() - 8 * c.u, 18 * c.u, 18 * c.u), 200 * 16, 140 * 16);
    c.g.drawLine(QPointF(mic.x(), mic.y() + 10 * c.u), QPointF(mic.x(), mic.y() + 14 * c.u));
    if (muted)
        c.g.drawLine(QPointF(mic.x() - 11 * c.u, mic.y() - 14 * c.u),
            QPointF(mic.x() + 11 * c.u, mic.y() + 14 * c.u));
    drawText(c, QPointF(pill.left() + padX + iconW, pill.center().y()), name, 30, Qt::AlignLeft,
        false, QColor(250, 250, 250));
    // Connection bars at the top right of the tile, call timer at the top left.
    const int bars = int(std::lround(num(c.p, k::Signal, fr, 4)));
    for (int i = 0; i < 4; ++i) {
        const double bh = (8 + i * 6) * c.ui;
        const QRectF bar(
            tile.right() - (92 - i * 15) * c.ui, tile.top() + (22 + 26) * c.ui - bh, 9 * c.ui, bh);
        c.g.setPen(Qt::NoPen);
        c.g.setBrush(i < bars ? QColor(235, 235, 235) : QColor(255, 255, 255, 70));
        c.g.drawRoundedRect(bar, 2 * c.ui, 2 * c.ui);
    }
    if (flag(c.p, k::ShowTimer, true)) {
        const QString t = elapsedText(fr, c.f.fps);
        const QFontMetricsF tm(fontFor(c, 26 * c.u, true));
        const QRectF chip(tile.left() + 22 * c.ui, tile.top() + 22 * c.ui,
            tm.horizontalAdvance(t) + 36 * c.u, 42 * c.u);
        c.g.setPen(Qt::NoPen);
        c.g.setBrush(QColor(12, 14, 18, 175));
        c.g.drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
        filledCircle(c, QPointF(chip.left() + 16 * c.u, chip.center().y()), 5, QColor(235, 70, 60));
        drawText(c, QPointF(chip.left() + 28 * c.u, chip.center().y()), t, 26, Qt::AlignLeft, true,
            QColor(245, 245, 245));
    }
}

// ---- Video Camera ----

void paintVideoCamera(Ctx& c)
{
    const int64_t fr = c.f.localFrame;
    const bool rec = flag(c.p, k::Recording, true);
    if (flag(c.p, k::ShowMarks, true))
        cornerMarks(c, c.r.adjusted(60 * c.ui, 50 * c.ui, -60 * c.ui, -50 * c.ui), 56, 3);
    if (rec) {
        if (!flag(c.p, k::Blink, true) || blinkOn(fr, c.f.fps))
            filledCircle(c, c.at(106, 104), 15, QColor(235, 40, 40));
        drawText(c, c.at(136, 104), QStringLiteral("REC"), 42);
    } else {
        drawText(c, c.at(92, 104), QStringLiteral("STBY"), 42);
    }
    drawText(c, c.at(92, 160), str(c.p, k::FormatLabel, QStringLiteral("HD 1080 50i")), 32,
        Qt::AlignLeft, true, withAlpha(c.color, 0.9));
    const double battery = num(c.p, k::BatteryLevel, fr, 80);
    batteryIcon(c, c.fromRight(92 + 62 + 80, 90), battery);
    drawText(c, c.fromRight(92, 104), QStringLiteral("%1%").arg(int(std::lround(battery))), 34,
        Qt::AlignRight);
    drawText(c, c.fromRight(92, 160), str(c.p, k::CardRemaining, QStringLiteral("REMAIN 1h25m")),
        32, Qt::AlignRight, true, withAlpha(c.color, 0.9));
    // Timecode, lower left; zoom bar, lower right.
    drawText(c, c.fromBottom(92, 108), cameraTimecode(c.p, fr, c.f.fps), 56);
    {
        const double zoom = std::clamp(num(c.p, k::ZoomLevel, fr, 20) / 100.0, 0.0, 1.0);
        const double w = 340 * c.ui;
        const QPointF a = c.fromCorner(92 + 36, 108);
        const double x0 = a.x() - w, y = a.y();
        drawText(c, QPointF(x0 - 14 * c.ui, y), QStringLiteral("W"), 28, Qt::AlignRight);
        drawText(c, QPointF(x0 + w + 14 * c.ui, y), QStringLiteral("T"), 28, Qt::AlignLeft);
        strokeLine(c, QPointF(x0, y), QPointF(x0 + w, y), 3, withAlpha(c.color, 0.6));
        strokeLine(c, QPointF(x0, y), QPointF(x0 + w * zoom, y), 6);
        filledCircle(c, QPointF(x0 + w * zoom, y), 10, c.color);
    }
    if (flag(c.p, k::ShowFocusFrame, true)) {
        const QPointF m = c.r.center();
        cornerMarks(
            c, QRectF(m.x() - 130 * c.ui, m.y() - 90 * c.ui, 260 * c.ui, 180 * c.ui), 36, 3);
    }
}

} // namespace

bool blinkOn(int64_t localFrame, double fps)
{
    const double s = fps > 0 ? double(localFrame) / fps : 0.0;
    return int64_t(std::floor(s * kBlinkHalfPeriods)) % 2 == 0;
}

QString elapsedText(int64_t localFrame, double fps)
{
    const int64_t total
        = fps > 0 ? int64_t(std::floor(double(std::max<int64_t>(0, localFrame)) / fps)) : 0;
    const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3")
                       .arg(h)
                       .arg(m, 2, 10, QLatin1Char('0'))
                       .arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString cctvTimestamp(const ParamSet& params, int64_t localFrame, double fps)
{
    const QString start = str(params, k::StartTime, QStringLiteral("2026-03-14 22:05:00"));
    QDateTime when = QDateTime::fromString(start, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!when.isValid())
        when = QDateTime::fromString(start, Qt::ISODate);
    if (!when.isValid())
        when = QDateTime(QDate(2000, 1, 1), QTime(0, 0));
    const double s = fps > 0 ? double(localFrame) / fps : 0.0;
    when = when.addMSecs(qint64(std::floor(s * 1000.0)));
    const QString format = str(params, k::TimeFormat, QStringLiteral("yyyy-MM-dd  HH:mm:ss"));
    return when.toString(format.isEmpty() ? QStringLiteral("yyyy-MM-dd  HH:mm:ss") : format);
}

QString cameraTimecode(const ParamSet& params, int64_t localFrame, double fps)
{
    const QRegularExpression re(QStringLiteral("^(\\d+):(\\d+):(\\d+)[:;](\\d+)$"));
    const int nominal = std::max(1, int(std::lround(fps)));
    int64_t frames = 0;
    const auto m = re.match(str(params, k::StartTimecode, QStringLiteral("00:00:00:00")));
    if (m.hasMatch()) {
        frames = ((m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 60
                     + m.captured(3).toLongLong())
                * nominal
            + m.captured(4).toLongLong();
    }
    frames += std::max<int64_t>(0, localFrame);
    const int64_t ff = frames % nominal, ss = (frames / nominal) % 60,
                  mm = (frames / nominal / 60) % 60, hh = frames / nominal / 3600 % 100;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QLatin1Char('0'))
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

bool hasOverlay(const QString& id)
{
    return id == QLatin1String(fxid::LookCctv) || id == QLatin1String(fxid::LookDroneOverlay)
        || id == QLatin1String(fxid::LookDslr) || id == QLatin1String(fxid::LookVideoCall)
        || id == QLatin1String(fxid::LookVideoCamera);
}

QImage paint(const QString& id, const ParamSet& p, const Frame& f)
{
    if (!hasOverlay(id) || f.size.isEmpty())
        return {};
    QImage image(f.size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter g(&image);
    g.setRenderHints(
        QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    g.setOpacity(std::clamp(num(p, k::OverlayOpacity, f.localFrame, 100), 0.0, 100.0) / 100.0);
    const double unit = f.content.height() / 1080.0;
    const bool sans = id == QLatin1String(fxid::LookVideoCall);
    QColor color = colorOf(p, k::Color, QColor(0xf4, 0xf4, 0xf0));
    Ctx ctx {g, f.content,
        unit * std::clamp(num(p, k::TextSize, f.localFrame, 100), 25.0, 300.0) / 100.0, unit, color,
        sans ? sansFamily(p) : monoFamily(p), p, f};
    if (id == QLatin1String(fxid::LookCctv))
        paintCctv(ctx);
    else if (id == QLatin1String(fxid::LookDroneOverlay))
        paintDrone(ctx);
    else if (id == QLatin1String(fxid::LookDslr))
        paintDslr(ctx);
    else if (id == QLatin1String(fxid::LookVideoCall))
        paintVideoCall(ctx);
    else
        paintVideoCamera(ctx);
    g.end();
    return image;
}

QString cacheKey(const QString& id, const ParamSet& p, const Frame& f)
{
    QString key = QStringLiteral("%1|%2x%3|%4,%5,%6,%7|%8")
                      .arg(id)
                      .arg(f.size.width())
                      .arg(f.size.height())
                      .arg(f.content.x())
                      .arg(f.content.y())
                      .arg(f.content.width())
                      .arg(f.content.height())
                      .arg(f.scale);
    for (auto it = p.values.cbegin(); it != p.values.cend(); ++it)
        key += QLatin1Char('|') + it.key() + QLatin1Char('=')
            + QString::number(it->valueAt(f.localFrame), 'g', 9);
    for (auto it = p.statics.cbegin(); it != p.statics.cend(); ++it) {
        const QVariant& v = it.value();
        key += QLatin1Char('|') + it.key() + QLatin1Char('=')
            + (v.metaType().id() == QMetaType::QColor ? v.value<QColor>().name(QColor::HexArgb)
                                                      : v.toString());
    }
    // What changes with the clip time.
    if (id == QLatin1String(fxid::LookCctv))
        key += QLatin1Char('|') + cctvTimestamp(p, f.localFrame, f.fps);
    if (id == QLatin1String(fxid::LookVideoCamera))
        key += QLatin1Char('|') + cameraTimecode(p, f.localFrame, f.fps);
    key += QLatin1Char('|') + elapsedText(f.localFrame, f.fps)
        + (blinkOn(f.localFrame, f.fps) ? QLatin1String("+") : QLatin1String("-"));
    return key;
}

} // namespace kk::render::lookhud
