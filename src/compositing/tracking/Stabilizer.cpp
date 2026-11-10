// SPDX-License-Identifier: GPL-3.0-or-later
// Stabilizer implementation. See the header for the method.
#include "compositing/tracking/Stabilizer.h"

#include "compositing/tracking/Homography.h"
#include "compositing/tracking/PlanarTracker.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

using stabilize::Mat3;
using stabilize::Mode;

constexpr double kRegionMargin = 0.1; ///< the measured region keeps this fraction free at each edge
constexpr double kSearchFraction = 0.06; ///< how far a patch may move between frames, of the width
constexpr int kMinSearch = 8;
constexpr int kGridSize = 9;
constexpr double kInlierPixels = 1.5;

double median(std::vector<double> v)
{
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(mid), v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        const double lower = *std::max_element(v.begin(), v.begin() + std::ptrdiff_t(mid));
        m = 0.5 * (m + lower);
    }
    return m;
}

/// Pixels -> normalised picture coordinates.
Vec2 normalised(Vec2 p, int width, int height)
{
    return {(p.x - 0.5 * (width - 1)) / width, (p.y - 0.5 * (height - 1)) / width};
}

/// The numbers stored for the path entry of @p m in @p mode.
void pathParams(Mode mode, const Mat3& m, float* out)
{
    switch (mode) {
    case Mode::Translation:
        out[0] = float(m.m[2]);
        out[1] = float(m.m[5]);
        return;
    case Mode::Similarity:
        out[0] = float(m.m[2]);
        out[1] = float(m.m[5]);
        out[2] = float(std::atan2(m.m[3], m.m[0]));
        out[3] = float(std::log(std::max(1e-9, std::hypot(m.m[0], m.m[3]))));
        return;
    case Mode::Perspective:
        break;
    }
    const double w = std::abs(m.m[8]) > 1e-12 ? m.m[8] : 1.0;
    for (size_t i = 0; i < 8; ++i)
        out[i] = float(m.m[i] / w);
}

/// The path entry's matrix, rebuilt from its parameters so the chained path stays in the model.
Mat3 modelMatrix(Mode mode, const float* p)
{
    Mat3 r;
    switch (mode) {
    case Mode::Translation:
        return Mat3::translation(p[0], p[1]);
    case Mode::Similarity: {
        const double s = std::exp(double(p[3]));
        const double c = s * std::cos(double(p[2])), n = s * std::sin(double(p[2]));
        r.m = {c, -n, double(p[0]), n, c, double(p[1]), 0, 0, 1};
        return r;
    }
    case Mode::Perspective:
        break;
    }
    for (size_t i = 0; i < 8; ++i)
        r.m[i] = double(p[i]);
    r.m[8] = 1.0;
    return r;
}

/// Keeps the pairs whose distance from the model's answer is at most @p limit (normalised units).
template<typename Map>
void keepClose(std::vector<Vec2>& from, std::vector<Vec2>& to, const Map& map, double limit)
{
    std::vector<Vec2> f, t;
    for (size_t i = 0; i < from.size(); ++i) {
        const Vec2 q = map(from[i]);
        if (std::hypot(q.x - to[i].x, q.y - to[i].y) <= limit) {
            f.push_back(from[i]);
            t.push_back(to[i]);
        }
    }
    from = std::move(f);
    to = std::move(t);
}

/// Median distance of the pairs from the model's answer.
template<typename Map>
double medianResidual(const std::vector<Vec2>& from, const std::vector<Vec2>& to, const Map& map)
{
    std::vector<double> r;
    for (size_t i = 0; i < from.size(); ++i) {
        const Vec2 q = map(from[i]);
        r.push_back(std::hypot(q.x - to[i].x, q.y - to[i].y));
    }
    return median(r);
}

constexpr int kTrimRounds = 4;
constexpr size_t kMinFitPairs = 8;
constexpr double kTrimFactor = 2.5; ///< pairs further than this many median residuals are dropped
constexpr double kTrimFloorPixels = 0.3;

} // namespace

FrameMotion estimateFrameMotion(const GrayImage& previous, const GrayImage& next, Mode mode)
{
    FrameMotion out;
    if (previous.isEmpty() || next.isEmpty() || previous.width != next.width
        || previous.height != next.height)
        return out;
    const int w = previous.width, h = previous.height;
    PlanarSettings settings;
    settings.gridSize = kGridSize;
    settings.searchRadius = std::max(kMinSearch, int(kSearchFraction * w));
    settings.minCorrelation = 0.5;
    settings.minInlierFraction = 0.2;
    settings.refreshBelow = 0.0; // the pattern is the previous frame, nothing to refresh
    settings.inlierThreshold = kInlierPixels * std::max(1.0, w / 640.0);
    PlanarTracker tracker(settings);
    const double mx = kRegionMargin * w, my = kRegionMargin * h;
    const Quad region {{{mx, my}, {w - 1 - mx, my}, {w - 1 - mx, h - 1 - my}, {mx, h - 1 - my}}};
    if (!tracker.reset(previous, region))
        return out;
    PlanarResult found = tracker.track(next);
    if (found.ok) {
        // A second look starts from the first answer (the patches are warped by it), which
        // removes the bias of matching turned and scaled patches without the warp.
        const PlanarResult again = tracker.track(next);
        if (again.ok && again.inlierFrom.size() >= 3)
            found = again;
    }
    if (!found.ok || found.inlierFrom.size() < 3)
        return out;

    std::vector<Vec2> from, to;
    from.reserve(found.inlierFrom.size());
    to.reserve(found.inlierFrom.size());
    for (size_t i = 0; i < found.inlierFrom.size(); ++i) {
        from.push_back(normalised(found.inlierFrom[i], w, h));
        to.push_back(normalised(found.inlierTo[i], w, h));
    }
    out.inliers = int(from.size());
    switch (mode) {
    case Mode::Translation: {
        std::vector<double> dx, dy;
        for (size_t i = 0; i < from.size(); ++i) {
            dx.push_back(to[i].x - from[i].x);
            dy.push_back(to[i].y - from[i].y);
        }
        out.m = Mat3::translation(median(dx), median(dy));
        out.ok = true;
        return out;
    }
    case Mode::Similarity: {
        auto sim = similarityFromPoints(from, to);
        // Trimmed least squares: pairs that disagree with the fit (the edge of a moving object,
        // a wrong match) are dropped and the fit repeated, so they cannot bias it.
        for (int round = 0; sim && round < kTrimRounds && from.size() > kMinFitPairs; ++round) {
            auto map = [&](Vec2 p) { return sim->map(p); };
            const double limit
                = std::max(kTrimFloorPixels / w, kTrimFactor * medianResidual(from, to, map));
            const size_t before = from.size();
            std::vector<Vec2> f = from, t = to;
            keepClose(f, t, map, limit);
            if (f.size() < kMinFitPairs || f.size() == before)
                break;
            const auto refit = similarityFromPoints(f, t);
            if (!refit)
                break;
            from = std::move(f);
            to = std::move(t);
            sim = refit;
        }
        if (!sim)
            return out;
        const double c = sim->scale * std::cos(sim->angleRad);
        const double s = sim->scale * std::sin(sim->angleRad);
        out.m.m = {c, -s, sim->dx, s, c, sim->dy, 0, 0, 1};
        out.ok = true;
        return out;
    }
    case Mode::Perspective:
        break;
    }
    // Perspective: the homography of the agreeing patches, refitted in normalised coordinates
    // (better conditioned than the pixel homography and free of the pixel offset).
    auto h2 = homographyFromPoints(from, to);
    for (int round = 0; h2 && round < kTrimRounds && from.size() > kMinFitPairs; ++round) {
        auto map = [&](Vec2 p) { return h2->map(p); };
        const double limit
            = std::max(kTrimFloorPixels / w, kTrimFactor * medianResidual(from, to, map));
        const size_t before = from.size();
        std::vector<Vec2> f = from, t = to;
        keepClose(f, t, map, limit);
        if (f.size() < kMinFitPairs || f.size() == before)
            break;
        const auto refit = homographyFromPoints(f, t);
        if (!refit)
            break;
        from = std::move(f);
        to = std::move(t);
        h2 = refit;
    }
    if (!h2 || !h2->isFinite())
        return out;
    const Homography n = h2->normalised();
    out.m.m = n.m;
    out.ok = true;
    return out;
}

StabilizeOutcome stabilizeAnalyse(const StabilizeRequest& request, const GrayFrameFn& frameAt,
    const StabilizeProgressFn& progress, const std::atomic<bool>* cancel)
{
    StabilizeOutcome outcome;
    if (request.lastFrame <= request.firstFrame || !request.fps.isValid()) {
        outcome.error = QStringLiteral("the clip shows fewer than two different source frames");
        return outcome;
    }
    const int total = int(request.lastFrame - request.firstFrame + 1);
    const int pc = stabilize::paramCount(request.mode);
    auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

    GrayImage previous = frameAt(request.firstFrame);
    if (previous.isEmpty()) {
        outcome.error = QStringLiteral("the picture could not be read");
        return outcome;
    }
    const double aspect = double(previous.height) / double(previous.width);
    std::vector<float> path(size_t(total) * size_t(pc), 0.0f);
    Mat3 camera; // identity: the first frame
    pathParams(request.mode, camera, &path[0]);
    if (progress)
        progress(1, total);
    for (int i = 1; i < total; ++i) {
        if (cancelled()) {
            outcome.cancelled = true;
            return outcome;
        }
        GrayImage next = frameAt(request.firstFrame + i);
        if (next.isEmpty()) {
            outcome.error = QStringLiteral("frame %1 of the source could not be read")
                                .arg(request.firstFrame + i);
            return outcome;
        }
        const FrameMotion motion = estimateFrameMotion(previous, next, request.mode);
        ++outcome.pairs;
        if (motion.ok) {
            camera = motion.m * camera;
        } else {
            ++outcome.lost;
        }
        float* out = &path[size_t(i) * size_t(pc)];
        pathParams(request.mode, camera, out);
        camera = modelMatrix(request.mode, out); // chain in the model (no drift off it)
        previous = std::move(next);
        if (progress)
            progress(i + 1, total);
    }
    outcome.data = std::make_shared<const stabilize::Data>(
        request.mode, request.fps, request.firstFrame, aspect, std::move(path));
    return outcome;
}

} // namespace kk::tracking
