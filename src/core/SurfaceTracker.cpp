// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SurfaceTracker.h"

#include "compositing/tracking/Homography.h"
#include "core/EffectTrack.h"
#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::surface {

namespace {

namespace k = fxkeys;

std::shared_ptr<const efftrack::Table> tableOf(const EffectInstance& fx)
{
    if (fx.type != QLatin1String(fxid::SurfaceTracker))
        return nullptr;
    return efftrack::decode(fx.params.staticValue(k::TrackData).toString());
}

int densityOf(const ParamSet& p)
{
    return std::clamp(int(std::lround(p.value(k::GridDensity, 0, 4.0))), kMinDensity, kMaxDensity);
}

} // namespace

QString signature(const ParamSet& p)
{
    return QStringLiteral("st1|%1|%2|%3|%4|%5|%6|%7")
        .arg(p.staticValue(k::TrackSource, fxchoice::TrackClip).toString())
        .arg(densityOf(p))
        .arg(p.value(k::Smoothing, 0, 50.0), 0, 'f', 1)
        .arg(p.value(k::RegionX, 0, 50.0), 0, 'f', 3)
        .arg(p.value(k::RegionY, 0, 50.0), 0, 'f', 3)
        .arg(p.value(k::RegionSize, 0, 30.0), 0, 'f', 3)
        .arg(p.value(k::RegionHeight, 0, 30.0), 0, 'f', 3);
}

bool isTracked(const EffectInstance& fx)
{
    const auto table = tableOf(fx);
    return fx.enabled && table && table->signature() == signature(fx.params)
        && table->stride() == 2 * densityOf(fx.params) * densityOf(fx.params);
}

bool needsTracking(const EffectInstance& fx)
{
    const auto table = tableOf(fx);
    return table && !isTracked(EffectInstance {fx.id, fx.type, true, fx.params});
}

bool hasActivePin(const ClipData& clip)
{
    return std::any_of(clip.effects.cbegin(), clip.effects.cend(), [](const EffectInstance& fx) {
        return fx.type == QLatin1String(fxid::SurfaceTracker)
            && fx.params.staticValue(k::SurfaceMode, fxchoice::SurfacePin).toString()
            == fxchoice::SurfacePin
            && isTracked(fx);
    });
}

bool meshDisplacement(const efftrack::Table& table, double second, Mesh* mesh)
{
    const int n = int(std::lround(std::sqrt(table.stride() / 2.0)));
    if (n < kMinDensity || 2 * n * n != table.stride())
        return false;
    std::vector<float> ref(size_t(table.stride())), cur(size_t(table.stride()));
    table.at(table.reference(), ref.data());
    table.at(second, cur.data());
    mesh->cols = mesh->rows = n;
    mesh->d.resize(size_t(table.stride()));
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n * n; ++i) {
        const float u = ref[size_t(i) * 2], v = 1.f - ref[size_t(i) * 2 + 1];
        x0 = std::min(x0, u);
        x1 = std::max(x1, u);
        y0 = std::min(y0, v);
        y1 = std::max(y1, v);
        mesh->d[size_t(i) * 2] = cur[size_t(i) * 2] - ref[size_t(i) * 2];
        mesh->d[size_t(i) * 2 + 1] = -(cur[size_t(i) * 2 + 1] - ref[size_t(i) * 2 + 1]);
    }
    if (x1 - x0 < 1e-4f || y1 - y0 < 1e-4f)
        return false;
    mesh->rest[0] = x0;
    mesh->rest[1] = y0;
    mesh->rest[2] = x1;
    mesh->rest[3] = y1;
    return true;
}

QTransform pinTransform(const ClipData& clip, int64_t localFrame, QSizeF size, Rational fps)
{
    QTransform result;
    if (!fps.isValid())
        return result;
    for (const EffectInstance& fx : clip.effects) {
        if (fx.type != QLatin1String(fxid::SurfaceTracker) || !isTracked(fx)
            || fx.params.staticValue(k::SurfaceMode, fxchoice::SurfacePin).toString()
                != fxchoice::SurfacePin)
            continue;
        const auto table = tableOf(fx);
        const double second = double(clip.sourceTimeUsAt(clip.start + localFrame, fps)) / 1e6;
        const int count = table->stride() / 2;
        std::vector<float> ref(size_t(table->stride())), cur(size_t(table->stride()));
        table->at(table->reference(), ref.data());
        table->at(second, cur.data());
        std::vector<tracking::Vec2> from, to;
        for (int i = 0; i < count; ++i) {
            from.push_back({double(ref[size_t(i) * 2]) * size.width(),
                (1.0 - double(ref[size_t(i) * 2 + 1])) * size.height()});
            to.push_back({double(cur[size_t(i) * 2]) * size.width(),
                (1.0 - double(cur[size_t(i) * 2 + 1])) * size.height()});
        }
        const auto fit = tracking::homographyFromPoints(from, to);
        if (!fit)
            continue;
        tracking::Homography h = fit->normalised();
        const double strength = std::clamp(fx.params.value(k::Strength, localFrame, 100.0), 0.0, 200.0) / 100.0;
        for (int i = 0; i < 9; ++i) {
            const double identity = (i % 4 == 0) ? 1.0 : 0.0;
            h.m[size_t(i)] = identity + (h.m[size_t(i)] - identity) * strength;
        }
        if (fx.params.staticValue(k::TrackDirection, fxchoice::DirectionFollow).toString()
            == fxchoice::DirectionLock) {
            const auto inverse = h.inverse();
            if (!inverse)
                continue;
            h = inverse->normalised();
        }
        const auto& m = h.m;
        QTransform t(m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]);
        t = t * QTransform::fromTranslate(fx.params.value(k::OffsetX, localFrame, 0.0),
                    fx.params.value(k::OffsetY, localFrame, 0.0));
        result = result * t;
    }
    return result;
}

} // namespace kk::surface
