// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MatchMove.h"

#include "core/EffectTrack.h"
#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::matchmove {

namespace {

namespace k = fxkeys;

double staticNumber(const ParamSet& p, const QString& key, double fallback)
{
    return p.value(key, 0, fallback);
}

} // namespace

QString signature(const ParamSet& p)
{
    return QStringLiteral("mm1|%1|%2|%3|%4|%5")
        .arg(p.staticValue(k::TrackSource, fxchoice::TrackClip).toString())
        .arg(p.staticValue(k::TrackKind, fxchoice::KindPosition).toString())
        .arg(staticNumber(p, k::RegionX, 50), 0, 'f', 3)
        .arg(staticNumber(p, k::RegionY, 50), 0, 'f', 3)
        .arg(staticNumber(p, k::RegionSize, 12), 0, 'f', 3);
}

namespace {

std::shared_ptr<const efftrack::Table> tableOf(const EffectInstance& fx)
{
    if (fx.type != QLatin1String(fxid::MatchMove))
        return nullptr;
    return efftrack::decode(fx.params.staticValue(k::TrackData).toString());
}

} // namespace

bool isActive(const EffectInstance& fx)
{
    if (!fx.enabled)
        return false;
    const auto table = tableOf(fx);
    return table && table->signature() == signature(fx.params);
}

bool needsTracking(const EffectInstance& fx)
{
    const auto table = tableOf(fx);
    return table && table->signature() != signature(fx.params);
}

bool hasActive(const ClipData& clip)
{
    return std::any_of(clip.effects.cbegin(), clip.effects.cend(),
        [](const EffectInstance& fx) { return fx.type == QLatin1String(fxid::MatchMove) && isActive(fx); });
}

Motion motionBetween(const float* ref, const float* cur, int stride, QSizeF size)
{
    Motion m;
    const double w = size.width(), h = size.height();
    if (stride == kPointStride) {
        m.pivotX = double(ref[0]) * w;
        m.pivotY = (1.0 - double(ref[1])) * h;
        m.dx = double(cur[0] - ref[0]) * w;
        m.dy = -double(cur[1] - ref[1]) * h;
        return m;
    }
    std::array<double, 8> a {}, b {};
    for (int i = 0; i < 4; ++i) {
        a[size_t(i) * 2] = double(ref[i * 2]) * w;
        a[size_t(i) * 2 + 1] = (1.0 - double(ref[i * 2 + 1])) * h;
        b[size_t(i) * 2] = double(cur[i * 2]) * w;
        b[size_t(i) * 2 + 1] = (1.0 - double(cur[i * 2 + 1])) * h;
    }
    double ax = 0, ay = 0, bx = 0, by = 0;
    for (int i = 0; i < 4; ++i) {
        ax += a[size_t(i) * 2] / 4;
        ay += a[size_t(i) * 2 + 1] / 4;
        bx += b[size_t(i) * 2] / 4;
        by += b[size_t(i) * 2 + 1] / 4;
    }
    // Least-squares turn and scale of the corner vectors about the centres.
    double dot = 0, cross = 0, norm = 0;
    for (int i = 0; i < 4; ++i) {
        const double px = a[size_t(i) * 2] - ax, py = a[size_t(i) * 2 + 1] - ay;
        const double qx = b[size_t(i) * 2] - bx, qy = b[size_t(i) * 2 + 1] - by;
        dot += px * qx + py * qy;
        cross += px * qy - py * qx;
        norm += px * px + py * py;
    }
    m.pivotX = ax;
    m.pivotY = ay;
    m.dx = bx - ax;
    m.dy = by - ay;
    if (norm > 1e-9 && (dot != 0 || cross != 0)) {
        m.angleDeg = std::atan2(cross, dot) * 180.0 / std::numbers::pi;
        m.scale = std::sqrt(dot * dot + cross * cross) / norm;
    }
    return m;
}

QTransform transform(const ClipData& clip, int64_t localFrame, QSizeF frameSize, Rational fps)
{
    QTransform result;
    if (!fps.isValid())
        return result;
    for (const EffectInstance& fx : clip.effects) {
        if (fx.type != QLatin1String(fxid::MatchMove) || !fx.enabled)
            continue;
        const auto table = tableOf(fx);
        if (!table || table->signature() != signature(fx.params))
            continue;
        const double second
            = double(clip.sourceTimeUsAt(clip.start + localFrame, fps)) / 1e6;
        const int stride = table->stride();
        std::array<float, 8> ref {}, cur {};
        if (stride != kPointStride && stride != kRegionStride)
            continue;
        table->at(table->reference(), ref.data());
        table->at(second, cur.data());
        Motion m = motionBetween(ref.data(), cur.data(), stride, frameSize);
        const double strength = std::clamp(fx.params.value(k::Strength, localFrame, 100.0), 0.0, 200.0) / 100.0;
        m.dx *= strength;
        m.dy *= strength;
        m.angleDeg *= strength;
        m.scale = std::pow(std::max(m.scale, 1e-6), strength);
        QTransform t;
        t.translate(m.pivotX + m.dx, m.pivotY + m.dy);
        t.rotate(m.angleDeg);
        t.scale(m.scale, m.scale);
        t.translate(-m.pivotX, -m.pivotY);
        const bool lock = fx.params.staticValue(k::TrackDirection, fxchoice::DirectionFollow).toString()
            == fxchoice::DirectionLock;
        if (lock) {
            bool invertible = false;
            const QTransform inv = t.inverted(&invertible);
            if (invertible)
                t = inv;
        }
        const double ox = fx.params.value(k::OffsetX, localFrame, 0.0);
        const double oy = fx.params.value(k::OffsetY, localFrame, 0.0);
        t = t * QTransform::fromTranslate(ox, oy);
        result = result * t;
    }
    return result;
}

} // namespace kk::matchmove
