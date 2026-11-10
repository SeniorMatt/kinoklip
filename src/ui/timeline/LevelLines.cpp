// SPDX-License-Identifier: GPL-3.0-or-later
// LevelLines — level line / fade handle geometry (see LevelLines.h).
#include "ui/timeline/LevelLines.h"

#include "core/ClipAttributes.h"
#include "core/LevelOps.h"
#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::levellines {

namespace {
/// Margin between the area edges and the line's extreme positions (px).
constexpr double kInset = 2.0;
} // namespace

QRectF levelArea(const QRectF& r)
{
    const double band = r.height() >= kMinBandRow ? kNameBand : r.height();
    const QRectF area(r.left(), r.top() + band, r.width(), r.height() - band);
    return area.height() < kMinAreaHeight ? QRectF() : area;
}

double yForDb(const QRectF& area, double db)
{
    // Linear in dB: +24 dB (the maximum) at the top, -60 dB and below at the bottom.
    const double t = (kTopDb - std::clamp(db, kBottomDb, kTopDb)) / (kTopDb - kBottomDb);
    return area.top() + kInset + t * (area.height() - 2 * kInset);
}

double dbForY(const QRectF& area, double y)
{
    const double h = area.height() - 2 * kInset;
    if (h <= 0)
        return 0.0;
    const double t = std::clamp((y - area.top() - kInset) / h, 0.0, 1.0);
    if (t >= 1.0)
        return levelops::kMinVolumeDb; // dragged to the bottom: silence
    return kTopDb - t * (kTopDb - kBottomDb);
}

double dbPerPixel(const QRectF& area)
{
    const double h = area.height() - 2 * kInset;
    return h > 0 ? (kTopDb - kBottomDb) / h : 0.0;
}

LevelHit hitLevel(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, QPointF pos)
{
    const QRectF area = levelArea(clipRect);
    if (area.isEmpty() || !area.adjusted(0, -kLineGrab, 0, kLineGrab).contains(pos))
        return {};
    // Keys first: the nearest within the grab distance.
    const auto it = clip.audio.values.constFind(keys::VolumeDb);
    if (it != clip.audio.values.cend()) {
        double best = kKeyGrab + 1;
        int64_t bestFrame = 0;
        for (const Keyframe& k : it->keys) {
            if (k.frame < 0 || k.frame >= clip.duration)
                continue; // trimmed away: not drawn, not grabbable
            const QPointF p(layout.frameToX(double(clip.start + k.frame)), yForDb(area, k.value));
            const double d = std::max(std::abs(p.x() - pos.x()), std::abs(p.y() - pos.y()));
            if (d < best) {
                best = d;
                bestFrame = k.frame;
            }
        }
        if (best <= kKeyGrab)
            return {LevelHit::Kind::Key, bestFrame};
    }
    const double localFrame = layout.xToFrame(pos.x()) - double(clip.start);
    const double lineY = yForDb(area, levelops::volumeDbAt(clip, localFrame));
    if (std::abs(lineY - pos.y()) > kLineGrab)
        return {};
    const int64_t frame = std::clamp<int64_t>(
        int64_t(std::floor(localFrame)), 0, std::max<int64_t>(0, clip.duration - 1));
    return {LevelHit::Kind::Line, frame};
}

// Fade length as drawn: never longer than the clip (nor, with the other fade, than its length),
// so bad data (an old file) still shows handles inside the clip where they can be grabbed.
static int64_t shownFade(const ClipData& clip, bool head)
{
    const int64_t room = std::max<int64_t>(0, clip.duration);
    const int64_t in = std::clamp<int64_t>(clip.fadeInFrames, 0, room);
    const int64_t out = std::clamp<int64_t>(clip.fadeOutFrames, 0, room);
    return head ? std::min(in, room - std::min(out, room)) : std::min(out, room - in);
}

QRectF fadeHandleRect(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, bool head)
{
    if (clipRect.width() < kMinFadeClipWidth || clipRect.height() < 2 * kFadeHandle)
        return {};
    const double ppf = layout.pixelsPerFrame();
    const double x = head ? clipRect.left() + double(shownFade(clip, true)) * ppf
                          : clipRect.right() - double(shownFade(clip, false)) * ppf;
    // At the corner the square sits inside the clip; once a fade exists it is centred on the
    // fade's end, so it marks exactly where the fade reaches full level.
    const bool atCorner = shownFade(clip, head) == 0;
    const double left = atCorner ? (head ? x + 1 : x - kFadeHandle - 1) : x - kFadeHandle / 2;
    return QRectF(left, clipRect.top() + 1, kFadeHandle, kFadeHandle);
}

int hitFadeHandle(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, QPointF pos)
{
    // A little extra grab room around the small squares.
    constexpr double kGrab = 2.0;
    if (fadeHandleRect(clip, clipRect, layout, true)
            .adjusted(-kGrab, -kGrab, kGrab, kGrab)
            .contains(pos))
        return 1;
    if (fadeHandleRect(clip, clipRect, layout, false)
            .adjusted(-kGrab, -kGrab, kGrab, kGrab)
            .contains(pos))
        return 2;
    return 0;
}

double fadeAreaTop(const QRectF& clipRect)
{
    return clipRect.top() + 1;
}

double fadeAreaBottom(const QRectF& clipRect)
{
    return clipRect.bottom() - 1;
}

QPointF fadeCurvePoint(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, bool head, double t)
{
    const double ppf = layout.pixelsPerFrame();
    const double frames = double(shownFade(clip, head));
    const double curve = head ? clip.fadeInCurve : clip.fadeOutCurve;
    const double dx = t * frames * ppf;
    const double x = head ? clipRect.left() + dx : clipRect.right() - dx;
    const double top = fadeAreaTop(clipRect);
    const double bottom = fadeAreaBottom(clipRect);
    return {x, bottom - fadeShape(t, curve) * (bottom - top)};
}

QRectF fadeCurveHandleRect(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, bool head)
{
    const int64_t frames = shownFade(clip, head);
    if (frames <= 0 || clipRect.width() < kMinFadeClipWidth
        || double(frames) * layout.pixelsPerFrame() < kMinCurveFadeWidth
        || fadeAreaBottom(clipRect) - fadeAreaTop(clipRect) < kMinCurveHeight)
        return {};
    const QPointF c = fadeCurvePoint(clip, clipRect, layout, head, 0.5);
    return QRectF(c.x() - kCurveHandle / 2, c.y() - kCurveHandle / 2, kCurveHandle, kCurveHandle);
}

int hitFadeCurveHandle(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, QPointF pos)
{
    constexpr double kGrab = 2.0;
    for (const bool head : {true, false}) {
        const QRectF r = fadeCurveHandleRect(clip, clipRect, layout, head);
        if (!r.isEmpty() && r.adjusted(-kGrab, -kGrab, kGrab, kGrab).contains(pos))
            return head ? 1 : 2;
    }
    return 0;
}

double fadeCurveForY(const QRectF& clipRect, double y)
{
    const double top = fadeAreaTop(clipRect);
    const double bottom = fadeAreaBottom(clipRect);
    // Level of the fade's middle, kept inside what the curve range can reach (4^-1 ... 4^1).
    const double lo = std::pow(0.5, 4.0);
    const double hi = std::pow(0.5, 0.25);
    const double level = std::clamp((bottom - y) / std::max(1.0, bottom - top), lo, hi);
    // level = 0.5^(4^curve)  ->  curve = log4(ln(level) / ln(0.5))
    double curve = std::log(std::log(level) / std::log(0.5)) / std::log(4.0);
    curve = std::clamp(curve, -1.0, 1.0);
    return std::abs(curve) < kCurveSnap ? 0.0 : curve;
}

} // namespace kk::levellines
