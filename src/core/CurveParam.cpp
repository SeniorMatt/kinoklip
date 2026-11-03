// SPDX-License-Identifier: GPL-3.0-or-later
// CurveParam implementation: parsing, formatting and monotone cubic interpolation.
#include "core/CurveParam.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace kk::curve {

Points identity()
{
    return {{0.0, 0.0}, {1.0, 1.0}};
}

QString identityText()
{
    return QStringLiteral("0,0 1,1");
}

Points normalized(Points points)
{
    for (Point& p : points) {
        p.x = std::clamp(p.x, 0.0, 1.0);
        p.y = std::clamp(p.y, 0.0, 1.0);
    }
    std::stable_sort(
        points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    Points out;
    out.reserve(points.size());
    for (const Point& p : points) {
        if (!out.isEmpty() && p.x - out.last().x < kMinGap)
            continue;
        out.append(p);
        if (out.size() == kMaxPoints)
            break;
    }
    return out.size() < kMinPoints ? identity() : out;
}

Points parse(const QString& text)
{
    Points points;
    const QStringList pairs = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& pair : pairs) {
        const qsizetype comma = pair.indexOf(QLatin1Char(','));
        if (comma <= 0)
            return identity();
        bool okX = false, okY = false;
        const double x = pair.left(comma).toDouble(&okX); // QString::toDouble is C locale
        const double y = pair.mid(comma + 1).toDouble(&okY);
        if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y))
            return identity();
        points.append({x, y});
    }
    return normalized(std::move(points));
}

QString format(const Points& points)
{
    QStringList pairs;
    pairs.reserve(points.size());
    auto number = [](double v) {
        // 4 decimals, trailing zeros dropped ("0.5", "1", "0.1234").
        return QString::number(std::round(v * 10000.0) / 10000.0, 'g', 6);
    };
    for (const Point& p : points)
        pairs.append(number(p.x) + QLatin1Char(',') + number(p.y));
    return pairs.join(QLatin1Char(' '));
}

bool isIdentity(const Points& points)
{
    for (const Point& p : points) {
        if (std::abs(p.x - p.y) > 1e-9)
            return false;
    }
    // Every point on the diagonal and the ends at 0 and 1: the interpolation is the diagonal.
    return !points.isEmpty() && points.first().x <= 1e-9 && points.last().x >= 1.0 - 1e-9;
}

namespace {

/// Fritsch - Carlson tangents of the points (monotone where the data is monotone).
QVector<double> tangents(const Points& p)
{
    const qsizetype n = p.size();
    QVector<double> slope(n - 1);
    for (qsizetype i = 0; i + 1 < n; ++i)
        slope[i] = (p[i + 1].y - p[i].y) / (p[i + 1].x - p[i].x);
    QVector<double> m(n);
    m[0] = slope[0];
    m[n - 1] = slope[n - 2];
    for (qsizetype i = 1; i + 1 < n; ++i)
        m[i] = slope[i - 1] * slope[i] <= 0 ? 0.0 : 0.5 * (slope[i - 1] + slope[i]);
    // Limit the tangents so each segment stays within its two points (no overshoot).
    for (qsizetype i = 0; i + 1 < n; ++i) {
        if (slope[i] == 0.0) {
            m[i] = m[i + 1] = 0.0;
            continue;
        }
        const double a = m[i] / slope[i], b = m[i + 1] / slope[i];
        const double h = a * a + b * b;
        if (h > 9.0) {
            const double t = 3.0 / std::sqrt(h);
            m[i] = t * a * slope[i];
            m[i + 1] = t * b * slope[i];
        }
    }
    return m;
}

double hermite(const Points& p, const QVector<double>& m, double x)
{
    if (x <= p.first().x)
        return p.first().y;
    if (x >= p.last().x)
        return p.last().y;
    qsizetype i = 0;
    while (i + 2 < p.size() && x > p[i + 1].x)
        ++i;
    const double h = p[i + 1].x - p[i].x;
    const double t = (x - p[i].x) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p[i].y + (t3 - 2 * t2 + t) * h * m[i]
        + (-2 * t3 + 3 * t2) * p[i + 1].y + (t3 - t2) * h * m[i + 1];
}

} // namespace

double evaluate(const Points& points, double x)
{
    const Points p = points.size() < kMinPoints ? identity() : points;
    return std::clamp(hermite(p, tangents(p), x), 0.0, 1.0);
}

void sample(const Points& points, float* out, int n)
{
    const Points p = points.size() < kMinPoints ? identity() : points;
    const QVector<double> m = tangents(p);
    for (int i = 0; i < n; ++i)
        out[i] = float(std::clamp(hermite(p, m, double(i) / double(n - 1)), 0.0, 1.0));
}

} // namespace kk::curve
