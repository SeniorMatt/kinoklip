// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimGrammar.h"

#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace kk::vimgrammar {

namespace {

const QStringList kMotions {QStringLiteral("frame_prev"), QStringLiteral("frame_next"),
    QStringLiteral("second_prev"), QStringLiteral("second_next"), QStringLiteral("timeline_start"),
    QStringLiteral("timeline_end"), QStringLiteral("goto_frame"), QStringLiteral("first_clip"),
    QStringLiteral("clip_next"), QStringLiteral("clip_prev"), QStringLiteral("clip_end"),
    QStringLiteral("clip_end_prev"), QStringLiteral("edit_next"), QStringLiteral("edit_prev"),
    QStringLiteral("edit_end"), QStringLiteral("edit_end_prev"), QStringLiteral("marker_next"),
    QStringLiteral("marker_prev"), QStringLiteral("keyframe_next"), QStringLiteral("keyframe_prev"),
    QStringLiteral("track_down"), QStringLiteral("track_up"), QStringLiteral("track_first"),
    QStringLiteral("track_last")};

const QStringList kObjects {QStringLiteral("obj_iw"), QStringLiteral("obj_aw"),
    QStringLiteral("obj_iW"), QStringLiteral("obj_aW"), QStringLiteral("obj_ip"),
    QStringLiteral("obj_ap"), QStringLiteral("obj_im"), QStringLiteral("obj_ir"),
    QStringLiteral("obj_it")};

int clampTrack(const VimHost& host, int track)
{
    return std::clamp(track, 0, std::max(0, host.trackCount() - 1));
}

/// The first value > @p frame, or -1.
int64_t firstAfter(const QVector<int64_t>& sorted, int64_t frame)
{
    const auto it = std::upper_bound(sorted.begin(), sorted.end(), frame);
    return it == sorted.end() ? -1 : *it;
}

/// The last value < @p frame, or -1.
int64_t lastBefore(const QVector<int64_t>& sorted, int64_t frame)
{
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), frame);
    return it == sorted.begin() ? -1 : *(it - 1);
}

int64_t trackEnd(const QVector<VimClipSpan>& spans)
{
    return spans.isEmpty() ? 0 : spans.last().end;
}

VimRange framesRange(int64_t start, int64_t end, int track, bool all)
{
    VimRange r;
    r.kind = VimRange::Kind::Frames;
    r.start = start;
    r.end = end;
    r.trackFirst = r.trackLast = track;
    r.allTracks = all;
    return r;
}

VimRange clipsRange(const QVector<quint64>& ids)
{
    VimRange r;
    r.kind = VimRange::Kind::Clips;
    r.clips = ids;
    return r;
}

/// Index of the clip covering @p frame, or -1.
int indexAt(const QVector<VimClipSpan>& spans, int64_t frame)
{
    for (int i = 0; i < spans.size(); ++i) {
        if (spans[i].start <= frame && frame < spans[i].end)
            return i;
    }
    return -1;
}

} // namespace

bool isMotion(const QString& name)
{
    return kMotions.contains(name);
}

bool isObject(const QString& name)
{
    return kObjects.contains(name);
}

VimClipSpan clipAt(const VimHost& host, int track, int64_t frame)
{
    const QVector<VimClipSpan> spans = host.clipSpans(track);
    const int i = indexAt(spans, frame);
    return i < 0 ? VimClipSpan {} : spans[i];
}

QVector<quint64> clipsUnder(const VimHost& host, int firstTrack, int lastTrack, int64_t frame)
{
    QVector<quint64> ids;
    const int a = clampTrack(host, std::min(firstTrack, lastTrack));
    const int b = clampTrack(host, std::max(firstTrack, lastTrack));
    for (int t = a; t <= b; ++t) {
        const VimClipSpan c = clipAt(host, t, frame);
        if (c.id)
            ids.append(c.id);
    }
    return ids;
}

QVector<quint64> clipsTouching(
    const VimHost& host, int firstTrack, int lastTrack, int64_t start, int64_t end)
{
    QVector<quint64> ids;
    const int a = clampTrack(host, std::min(firstTrack, lastTrack));
    const int b = clampTrack(host, std::max(firstTrack, lastTrack));
    for (int t = a; t <= b; ++t) {
        for (const VimClipSpan& c : host.clipSpans(t)) {
            if (c.start < end && c.end > start)
                ids.append(c.id);
        }
    }
    return ids;
}

QVector<int64_t> editPoints(const VimHost& host)
{
    QVector<int64_t> points;
    for (int t = 0; t < host.trackCount(); ++t) {
        for (const VimClipSpan& c : host.clipSpans(t)) {
            points.append(c.start);
            points.append(c.end);
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::optional<Motion> motion(
    const VimHost& host, const QString& name, VimPos from, int count, bool hasCount)
{
    const int n = std::max(1, count);
    Motion m;
    m.to = from;
    // The clips of the cursor track, only for the motions that look at them.
    const QVector<VimClipSpan> spans = name.startsWith(QLatin1String("clip_"))
        ? host.clipSpans(from.track)
        : QVector<VimClipSpan>();
    const int64_t f = from.frame;

    if (name == QLatin1String("frame_prev")) {
        m.to.frame = std::max<int64_t>(0, f - n);
    } else if (name == QLatin1String("frame_next")) {
        m.to.frame = f + n;
    } else if (name == QLatin1String("second_prev")) {
        m.to.frame = std::max<int64_t>(0, f - int64_t(n) * std::llround(host.framesPerSecond()));
    } else if (name == QLatin1String("second_next")) {
        m.to.frame = f + int64_t(n) * std::llround(host.framesPerSecond());
    } else if (name == QLatin1String("timeline_start")) {
        m.to.frame = 0;
    } else if (name == QLatin1String("timeline_end")) {
        m.to.frame = std::max(f, host.timelineEnd());
    } else if (name == QLatin1String("goto_frame")) {
        m.to.frame = hasCount ? count : 0;
    } else if (name == QLatin1String("first_clip")) {
        int64_t first = -1;
        for (int t = 0; t < host.trackCount(); ++t) {
            const QVector<VimClipSpan> s = host.clipSpans(t);
            if (!s.isEmpty() && (first < 0 || s.first().start < first))
                first = s.first().start;
        }
        if (first < 0)
            return std::nullopt;
        m.to.frame = first;
    } else if (name == QLatin1String("clip_next")) {
        int64_t at = f;
        for (int i = 0; i < n; ++i) {
            int64_t next = -1;
            for (const VimClipSpan& c : spans) {
                if (c.start > at) {
                    next = c.start;
                    break;
                }
            }
            if (next < 0) {
                // No more clips: the end of the last one, like `w` on the last word of a line.
                at = std::max(at, trackEnd(spans));
                break;
            }
            at = next;
        }
        m.to.frame = at;
    } else if (name == QLatin1String("clip_prev")) {
        int64_t at = f;
        for (int i = 0; i < n; ++i) {
            int64_t prev = -1;
            for (const VimClipSpan& c : spans) {
                if (c.start < at)
                    prev = c.start;
            }
            if (prev < 0)
                break;
            at = prev;
        }
        m.to.frame = at;
    } else if (name == QLatin1String("clip_end")) {
        int64_t at = f;
        for (int i = 0; i < n; ++i) {
            int64_t next = -1;
            for (const VimClipSpan& c : spans) {
                if (c.end - 1 > at) {
                    next = c.end - 1;
                    break;
                }
            }
            if (next < 0)
                break;
            at = next;
        }
        m.to.frame = at;
        m.type = Motion::Type::Inclusive;
    } else if (name == QLatin1String("clip_end_prev")) {
        int64_t at = f;
        for (int i = 0; i < n; ++i) {
            int64_t prev = -1;
            for (const VimClipSpan& c : spans) {
                if (c.end - 1 < at)
                    prev = c.end - 1;
            }
            if (prev < 0)
                break;
            at = prev;
        }
        m.to.frame = at;
        m.type = Motion::Type::Inclusive;
    } else if (name == QLatin1String("edit_next") || name == QLatin1String("edit_prev")
        || name == QLatin1String("edit_end") || name == QLatin1String("edit_end_prev")) {
        const QVector<int64_t> points = editPoints(host);
        int64_t at = f;
        m.allTracks = true;
        for (int i = 0; i < n; ++i) {
            int64_t target = -1;
            if (name == QLatin1String("edit_next")) {
                target = firstAfter(points, at);
            } else if (name == QLatin1String("edit_prev")) {
                target = lastBefore(points, at);
            } else if (name == QLatin1String("edit_end")) {
                // the last frame before the next edit point after the frame after the cursor
                const int64_t p = firstAfter(points, at + 1);
                target = p < 0 ? -1 : p - 1;
            } else {
                // the last frame before the edit point at or before the cursor
                const int64_t p = lastBefore(points, at + 1);
                target = p < 1 ? -1 : p - 1;
            }
            if (target < 0)
                break;
            at = target;
        }
        m.to.frame = at;
        if (name == QLatin1String("edit_end") || name == QLatin1String("edit_end_prev"))
            m.type = Motion::Type::Inclusive;
    } else if (name == QLatin1String("marker_next") || name == QLatin1String("marker_prev")
        || name == QLatin1String("keyframe_next") || name == QLatin1String("keyframe_prev")) {
        const bool marker = name.startsWith(QLatin1String("marker"));
        const QVector<int64_t> frames = marker ? host.markerFrames() : host.keyframeFrames();
        const bool forward = name.endsWith(QLatin1String("next"));
        int64_t at = f;
        for (int i = 0; i < n; ++i) {
            const int64_t target = forward ? firstAfter(frames, at) : lastBefore(frames, at);
            if (target < 0)
                break;
            at = target;
        }
        if (at == f && frames.isEmpty())
            return std::nullopt;
        m.to.frame = at;
    } else if (name == QLatin1String("track_down")) {
        m.to.track = clampTrack(host, from.track + n);
        m.type = Motion::Type::Lines;
    } else if (name == QLatin1String("track_up")) {
        m.to.track = clampTrack(host, from.track - n);
        m.type = Motion::Type::Lines;
    } else if (name == QLatin1String("track_first")) {
        m.to.track = 0;
        m.type = Motion::Type::Lines;
    } else if (name == QLatin1String("track_last")) {
        m.to.track = hasCount ? clampTrack(host, count - 1) : std::max(0, host.trackCount() - 1);
        m.type = Motion::Type::Lines;
    } else {
        return std::nullopt;
    }
    return m;
}

VimRange rangeOfMotion(const VimHost& host, VimPos from, const Motion& motion)
{
    if (motion.type == Motion::Type::Lines)
        return clipsRange(clipsUnder(host, from.track, motion.to.track, from.frame));
    const int64_t lo = std::min(from.frame, motion.to.frame);
    int64_t hi = std::max(from.frame, motion.to.frame);
    if (motion.type == Motion::Type::Inclusive)
        ++hi;
    return framesRange(lo, hi, from.track, motion.allTracks);
}

std::optional<VimRange> object(const VimHost& host, const QString& name, VimPos at, int count)
{
    const int n = std::max(1, count);
    const int64_t f = at.frame;
    const QVector<VimClipSpan> spans = host.clipSpans(at.track);
    const int i = indexAt(spans, f);

    if (name == QLatin1String("obj_iw")) {
        if (i >= 0) {
            QVector<quint64> ids;
            for (int k = i; k < spans.size() && k < i + n; ++k)
                ids.append(spans[k].id);
            return clipsRange(ids);
        }
        // In a gap: the gap (nothing after the last clip).
        int64_t from = 0;
        int64_t to = -1;
        for (const VimClipSpan& c : spans) {
            if (c.end <= f)
                from = c.end;
            if (c.start > f) {
                to = c.start;
                break;
            }
        }
        if (to <= from)
            return std::nullopt;
        return framesRange(from, to, at.track, false);
    }
    if (name == QLatin1String("obj_aw")) {
        if (i >= 0) {
            const int last = std::min<int>(spans.size() - 1, i + n - 1);
            const int64_t end = last + 1 < spans.size() ? spans[last + 1].start : spans[last].end;
            return framesRange(spans[i].start, end, at.track, false);
        }
        // In a gap: the gap and the next clip.
        int64_t from = 0;
        int next = -1;
        for (int k = 0; k < spans.size(); ++k) {
            if (spans[k].end <= f)
                from = spans[k].end;
            if (spans[k].start > f) {
                next = k;
                break;
            }
        }
        if (next < 0)
            return std::nullopt;
        return framesRange(
            from, spans[std::min<int>(spans.size() - 1, next + n - 1)].end, at.track, false);
    }
    if (name == QLatin1String("obj_iW") || name == QLatin1String("obj_aW")) {
        if (i < 0)
            return std::nullopt;
        return clipsRange(host.linkedClips(spans[i].id));
    }
    if (name == QLatin1String("obj_ip") || name == QLatin1String("obj_ap")) {
        if (i < 0)
            return std::nullopt;
        int a = i;
        int b = i;
        while (a > 0 && spans[a - 1].end == spans[a].start)
            --a;
        while (b + 1 < spans.size() && spans[b].end == spans[b + 1].start)
            ++b;
        if (name == QLatin1String("obj_ip")) {
            QVector<quint64> ids;
            for (int k = a; k <= b; ++k)
                ids.append(spans[k].id);
            return clipsRange(ids);
        }
        const int64_t end = b + 1 < spans.size() ? spans[b + 1].start : spans[b].end;
        return framesRange(spans[a].start, end, at.track, false);
    }
    if (name == QLatin1String("obj_im")) {
        const QVector<int64_t> markers = host.markerFrames();
        int64_t from = 0;
        for (const int64_t m : markers) {
            if (m <= f)
                from = m;
        }
        int64_t to = std::max(host.timelineEnd(), f + 1);
        const int64_t next = firstAfter(markers, f);
        if (next >= 0)
            to = next;
        if (to <= from)
            return std::nullopt;
        return framesRange(from, to, at.track, true);
    }
    if (name == QLatin1String("obj_ir")) {
        const auto [in, out] = host.markedRange();
        if (in < 0 || out <= in)
            return std::nullopt;
        return framesRange(in, out, at.track, true);
    }
    if (name == QLatin1String("obj_it")) {
        const QVector<quint64> ids = clipsUnder(host, 0, host.trackCount() - 1, f);
        if (ids.isEmpty())
            return std::nullopt;
        return clipsRange(ids);
    }
    return std::nullopt;
}

} // namespace kk::vimgrammar
