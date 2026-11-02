// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineAnimator implementation. modelChanged() is one pass over the new clips with hash
// lookups into the previous state (O(clips)); painting queries are O(1) per drawn clip.
#include "ui/timeline/TimelineAnimator.h"

#include "core/EditOps.h"
#include "ui/common/UiAnimation.h"
#include "ui/timeline/TimelineLayout.h"

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

using uianim::Kind;

/// More new + removed clips than this in one change is a load or a mass edit: not animated
/// (it would only be noise, and ghosts cost painting time).
constexpr int kMaxAppearing = 200;
/// Timelines with more clips than this are never animated (the per-change diff stays cheap).
constexpr int kMaxClips = 50000;
/// Blade cut: the halves start this far apart (px) and settle together.
constexpr double kSplitGapPx = 4.0;
/// Appearing clips grow from this fraction of their height and opacity.
constexpr double kAppearScale = 0.72;
constexpr double kAppearOpacity = 0.2;
/// Ghosts of removed clips start at this opacity.
constexpr double kGhostOpacity = 0.75;
/// The blade flash line covers the clip height during this fraction of its duration.
constexpr double kWipeFraction = 0.35;
constexpr int kFallbackFrameMs = 16;

double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

/// Linear progress of an animation of @p kind started at @p startMs.
double progress(Kind kind, int64_t startMs, int64_t nowMs)
{
    return std::clamp(double(nowMs - startMs) / uianim::duration(kind), 0.0, 1.0);
}

const std::array<TrackType, 3> kTypes = {TrackType::Video, TrackType::Audio, TrackType::Subtitle};

/// The clip of @p track that covers @p like's frames and shows the same media (kind, media,
/// name), if any.
const ClipData* coveringClip(const TrackData* track, const ClipData& like)
{
    if (!track)
        return nullptr;
    const auto [first, last]
        = TimelineLayout::clipRange(*track, double(like.start), double(like.end()));
    for (size_t i = first; i < last; ++i) {
        const ClipData& c = track->clips[i];
        if (c.id != like.id && c.kind == like.kind && c.mediaId == like.mediaId
            && c.name == like.name && c.start <= like.start && c.end() >= like.end())
            return &c;
    }
    return nullptr;
}

} // namespace

TimelineAnimator::TimelineAnimator(std::function<void()> repaint)
    : m_repaint(std::move(repaint))
{
    m_timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&m_timer, &QTimer::timeout, [this] {
        if (m_repaint)
            m_repaint();
    });
}

// ---- Model changes ----

void TimelineAnimator::index(const TimelineData& data)
{
    m_where.clear();
    for (const TrackType type : kTypes) {
        const auto& tracks = data.tracks(type);
        for (int t = 0; t < int(tracks.size()); ++t) {
            const auto& clips = tracks[size_t(t)].clips;
            for (int i = 0; i < int(clips.size()); ++i)
                m_where.insert(clips[size_t(i)].id, Where {TrackRef {type, t}, i});
        }
    }
}

void TimelineAnimator::resync(
    std::shared_ptr<const TimelineData> now, quint64 uid, quint64 revision)
{
    m_clips.clear();
    m_ghosts.clear();
    m_maxShift = 0;
    m_prev = std::move(now);
    m_uid = uid;
    m_revision = revision;
    m_stale = false;
    if (m_prev)
        index(*m_prev);
}

void TimelineAnimator::clear()
{
    m_prev.reset();
    m_where.clear();
    m_stale = true;
    m_clips.clear();
    m_ghosts.clear();
    m_flashes.clear();
    m_flashViews.clear();
    m_pulseStartMs = -1;
    m_snapInMs = -1;
    m_snapOut.reset();
    m_glide.stop();
    m_active = false;
    m_timer.stop();
}

void TimelineAnimator::finishAll()
{
    m_clips.clear();
    m_ghosts.clear();
    m_flashes.clear();
    m_flashViews.clear();
    m_pulseStartMs = -1;
    m_snapInMs = -1;
    m_snapOut.reset();
    m_glide.stop();
    m_active = false;
    m_timer.stop();
    if (m_repaint)
        m_repaint();
}

std::optional<TimelineAnimator::Geom> TimelineAnimator::displayedGeom(
    const ClipData& clip, TrackRef track, const TimelineLayout& layout, double t) const
{
    const int row = layout.rowOf(track);
    if (row < 0)
        return std::nullopt;
    const QRectF r = layout.clipRect(row, clip);
    Geom g {double(clip.start), double(clip.end()), r.top() + layout.scrollY(), r.height()};
    const auto it = m_clips.constFind(clip.id);
    if (it != m_clips.cend() && (it->flags & HasFrom)) {
        const double e = uianim::ease(Kind::Move, t);
        g.start = lerp(it->from.start, g.start, e);
        g.end = lerp(it->from.end, g.end, e);
        g.top = lerp(it->from.top, g.top, e);
        g.height = lerp(it->from.height, g.height, e);
    }
    return g;
}

double TimelineAnimator::clipProgress(int64_t nowMs) const
{
    if (m_clips.isEmpty() && m_ghosts.empty())
        return 1.0;
    return progress(Kind::Move, m_clipStartMs, nowMs);
}

void TimelineAnimator::modelChanged(std::shared_ptr<const TimelineData> now, quint64 uid,
    quint64 revision, const TimelineLayout& oldLayout)
{
    if (!now)
        return;
    if (m_stale || !m_prev || uid != m_uid || m_where.size() > kMaxClips) {
        resync(std::move(now), uid, revision);
        return;
    }
    const int64_t nowMs = uianim::nowMs();
    const double t = clipProgress(nowMs);
    const TimelineData& prev = *m_prev;
    const TimelineData& next = *now;

    QHash<quint64, ClipAnim> anims;
    std::vector<Ghost> ghosts;
    QHash<quint64, Where> where;
    where.reserve(m_where.size());
    std::vector<std::pair<quint64, Flash>> splits; ///< left half id, flash at the cut
    int appearing = 0;
    double maxShift = 0;

    for (const TrackType type : kTypes) {
        const auto& tracks = next.tracks(type);
        for (int ti = 0; ti < int(tracks.size()); ++ti) {
            const TrackRef ref {type, ti};
            const auto& clips = tracks[size_t(ti)].clips;
            for (int i = 0; i < int(clips.size()); ++i) {
                const ClipData& c = clips[size_t(i)];
                where.insert(c.id, Where {ref, i});
                const auto old = m_where.constFind(c.id);
                if (old != m_where.cend()) {
                    const ClipData& oc = prev.track(old->track)->clips[size_t(old->index)];
                    const bool running = m_clips.contains(c.id);
                    if (!running && old->track == ref && oc.start == c.start
                        && oc.duration == c.duration)
                        continue; // unchanged and still: nothing to do
                    const auto from = displayedGeom(oc, old->track, oldLayout, t);
                    if (!from)
                        continue; // its old track had no row: just show it
                    ClipAnim a {*from, HasFrom};
                    anims.insert(c.id, a);
                    maxShift = std::max({maxShift, std::abs(from->start - double(c.start)),
                        std::abs(from->end - double(c.end()))});
                    continue;
                }
                // A clip that is new to this state. A ghost of the same id (undo of a
                // delete) slides back from where the ghost is.
                const auto ghost = std::find_if(m_ghosts.begin(), m_ghosts.end(),
                    [&c](const Ghost& g) { return g.clip.id == c.id; });
                if (ghost != m_ghosts.end()) {
                    anims.insert(c.id,
                        ClipAnim {
                            Geom {ghost->start, ghost->end, ghost->top, ghost->height}, HasFrom});
                    maxShift = std::max({maxShift, std::abs(ghost->start - double(c.start)),
                        std::abs(ghost->end - double(c.end()))});
                    continue;
                }
                // A piece of an old clip (blade, overwrite into the middle of a clip): the
                // picture was already there, so it must not fade in.
                if (const ClipData* cover = coveringClip(prev.track(ref), c)) {
                    // A blade cut: the old clip now ends here and the two form a through edit.
                    if (i > 0 && clips[size_t(i) - 1].id == cover->id
                        && clips[size_t(i) - 1].end() == c.start
                        && editops::canJoin(next, cover->id, c.id)) {
                        anims.insert(c.id, ClipAnim {{}, SplitRight});
                        splits.push_back({cover->id, Flash {ref, c.start, nowMs}});
                    }
                    continue;
                }
                anims.insert(c.id, ClipAnim {{}, Appear});
                ++appearing;
            }
        }
    }
    // Removed clips fade out where they were drawn, unless a clip of the new state now shows
    // that part (undo of a blade: the left half grows back over the right one).
    for (auto it = m_where.cbegin(); it != m_where.cend(); ++it) {
        if (where.contains(it.key()))
            continue;
        const ClipData& oc = prev.track(it->track)->clips[size_t(it->index)];
        const ClipData* cover = coveringClip(next.track(it->track), oc);
        if (cover && editops::canJoin(prev, cover->id, oc.id))
            continue;
        const auto g = displayedGeom(oc, it->track, oldLayout, t);
        if (!g)
            continue;
        ghosts.push_back(Ghost {oc, it->track, g->start, g->end, g->top, g->height});
        ++appearing;
    }
    if (appearing > kMaxAppearing) {
        resync(std::move(now), uid, revision);
        return;
    }
    // The left halves of blade cuts: no slide (the cut is instant), only the separation.
    for (auto& [leftId, flash] : splits) {
        const auto w = where.constFind(leftId);
        if (w == where.cend())
            continue;
        const ClipData& lc = next.track(w->track)->clips[size_t(w->index)];
        const int row = oldLayout.rowOf(w->track);
        const QRectF r = oldLayout.clipRect(row, lc);
        anims[leftId] = ClipAnim {
            Geom {double(lc.start), double(lc.end()), r.top() + oldLayout.scrollY(), r.height()},
            uint8_t((row >= 0 ? HasFrom : 0) | SplitLeft)};
        m_flashes.push_back(flash);
    }
    // Ghosts still fading from an earlier change keep fading (restarted with the rest).
    for (Ghost& g : m_ghosts) {
        if (!where.contains(g.clip.id))
            ghosts.push_back(std::move(g));
    }

    m_clips = std::move(anims);
    m_ghosts = std::move(ghosts);
    m_maxShift = maxShift;
    m_clipStartMs = nowMs;
    m_prev = std::move(now);
    m_where = std::move(where);
    m_revision = revision;
    if (!m_clips.isEmpty() || !m_ghosts.empty() || !m_flashes.empty())
        ensureTimer();
}

// ---- Other effects ----

void TimelineAnimator::flashEdge(TrackRef track, int64_t frame)
{
    m_flashes.push_back(Flash {track, frame, uianim::nowMs()});
    ensureTimer();
}

void TimelineAnimator::pulseEditPoint()
{
    m_pulseStartMs = uianim::nowMs();
    ensureTimer();
}

void TimelineAnimator::playheadMoved(
    int64_t fromFrame, int64_t toFrame, bool allowed, double pixelsPerFrame)
{
    if (m_glide.moved(fromFrame, toFrame, allowed, pixelsPerFrame))
        ensureTimer();
}

void TimelineAnimator::observeSnap(std::optional<int64_t> frame)
{
    if (frame == m_snap)
        return;
    const int64_t nowMs = uianim::nowMs();
    if (frame && !m_snap) {
        m_snapInMs = nowMs;
        m_snapOut.reset();
    } else if (!frame) {
        m_snapOut = m_snap;
        m_snapOutMs = nowMs;
    }
    m_snap = frame;
    ensureTimer();
}

void TimelineAnimator::observeEditPoint(quint64 tailClip, quint64 headClip)
{
    if (tailClip == m_editTail && headClip == m_editHead)
        return;
    m_editTail = tailClip;
    m_editHead = headClip;
    if (tailClip || headClip)
        pulseEditPoint();
}

// ---- Frame ----

void TimelineAnimator::ensureTimer()
{
    if (m_timer.isActive())
        return;
    int interval = kFallbackFrameMs;
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const double hz = screen->refreshRate();
        if (hz >= 30.0)
            interval = std::max(4, int(1000.0 / hz));
    }
    m_timer.start(interval);
    m_active = true;
    if (m_repaint)
        m_repaint();
}

bool TimelineAnimator::tick()
{
    m_now = uianim::nowMs();
    bool any = false;
    if (!m_clips.isEmpty() || !m_ghosts.empty()) {
        const int64_t longest = std::max(uianim::duration(Kind::Move), uianim::duration(Kind::Pop));
        if (m_now - m_clipStartMs >= longest) {
            m_clips.clear();
            m_ghosts.clear();
            m_maxShift = 0;
        } else {
            any = true;
        }
    }
    m_flashViews.clear();
    if (!m_flashes.empty()) {
        const int flashMs = uianim::duration(Kind::Flash);
        std::erase_if(m_flashes, [&](const Flash& f) { return m_now - f.startMs >= flashMs; });
        for (const Flash& f : m_flashes) {
            const double t = progress(Kind::Flash, f.startMs, m_now);
            m_flashViews.push_back(FlashView {f.track, f.frame,
                uianim::ease(Kind::Move, std::min(1.0, t / kWipeFraction)),
                1.0 - uianim::ease(Kind::Flash, t)});
        }
        any = any || !m_flashes.empty();
    }
    if (m_pulseStartMs >= 0) {
        if (m_now - m_pulseStartMs >= uianim::duration(Kind::Flash))
            m_pulseStartMs = -1;
        else
            any = true;
    }
    const int fadeMs = uianim::duration(Kind::Fade);
    if (m_snapInMs >= 0 && m_now - m_snapInMs >= fadeMs)
        m_snapInMs = -1;
    if (m_snapOut && m_now - m_snapOutMs >= fadeMs)
        m_snapOut.reset();
    any = any || m_snapInMs >= 0 || m_snapOut;
    any = any || m_glide.running(m_now);
    m_active = any;
    if (!any)
        m_timer.stop();
    return any;
}

// ---- Painter queries ----

double TimelineAnimator::displayClip(
    quint64 clipId, QRectF& rect, const TimelineLayout& layout) const
{
    const auto it = m_clips.constFind(clipId);
    if (it == m_clips.cend()) {
        for (const Ghost& g : m_ghosts) {
            if (g.clip.id != clipId)
                continue;
            const double t = progress(Kind::Move, m_clipStartMs, m_now);
            const double x0 = layout.frameToX(g.start);
            const double x1 = layout.frameToX(g.end);
            const double shrink = g.height * (1.0 - kAppearScale) * 0.5 * t;
            rect = QRectF(x0, g.top - layout.scrollY() + shrink, x1 - x0, g.height - 2 * shrink);
            return kGhostOpacity * (1.0 - uianim::ease(Kind::Move, t));
        }
        return 1.0;
    }
    const ClipAnim& a = *it;
    const double t = progress(Kind::Move, m_clipStartMs, m_now);
    double opacity = 1.0;
    if (a.flags & HasFrom) {
        const double e = uianim::ease(Kind::Move, t);
        const double x0 = lerp(layout.frameToX(a.from.start), rect.left(), e);
        const double x1 = lerp(layout.frameToX(a.from.end), rect.right(), e);
        const double y = lerp(a.from.top - layout.scrollY(), rect.top(), e);
        const double h = lerp(a.from.height, rect.height(), e);
        rect = QRectF(x0, y, x1 - x0, h);
    }
    if (a.flags & Appear) {
        const double pop = uianim::ease(Kind::Pop, progress(Kind::Pop, m_clipStartMs, m_now));
        const double h = rect.height() * lerp(kAppearScale, 1.0, pop);
        rect = QRectF(rect.left(), rect.center().y() - h / 2.0, rect.width(), h);
        opacity = lerp(kAppearOpacity, 1.0, uianim::ease(Kind::Move, t));
    }
    if (a.flags & (SplitLeft | SplitRight)) {
        // The halves jump a hair apart at the cut and settle back together.
        const double gap = kSplitGapPx * (1.0 - uianim::ease(Kind::Move, t));
        const double half = std::min(gap, rect.width() / 3.0);
        if (a.flags & SplitLeft)
            rect.setRight(rect.right() - half);
        else
            rect.setLeft(rect.left() + half);
    }
    return opacity;
}

double TimelineAnimator::editPulse() const
{
    if (m_pulseStartMs < 0)
        return 0.0;
    return std::sin(std::numbers::pi * progress(Kind::Flash, m_pulseStartMs, m_now));
}

double TimelineAnimator::playheadFrame(int64_t modelFrame) const
{
    return m_glide.frame(modelFrame, m_now);
}

std::optional<std::pair<int64_t, double>> TimelineAnimator::snapLine(
    std::optional<int64_t> wanted) const
{
    if (wanted) {
        const double a = m_snapInMs >= 0 ? progress(Kind::Fade, m_snapInMs, m_now) : 1.0;
        return std::pair {*wanted, a};
    }
    if (m_snapOut)
        return std::pair {*m_snapOut, 1.0 - progress(Kind::Fade, m_snapOutMs, m_now)};
    return std::nullopt;
}

} // namespace kk
