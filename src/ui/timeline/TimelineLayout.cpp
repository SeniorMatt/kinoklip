// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineLayout implementation: row stacking, zoom maths and hit-testing.
#include "ui/timeline/TimelineLayout.h"

#include <algorithm>
#include <cmath>

namespace kk {

using namespace timelinemetrics;

void TimelineLayout::setHeightScale(double scale)
{
    m_heightScale = std::clamp(scale, kMinHeightScale, kMaxHeightScale);
}

void TimelineLayout::rebuild(const TimelineData& data)
{
    // Scaled row height; at least the smallest track height so the header buttons fit.
    auto scaled
        = [this](int h) { return std::max(kMinTrackHeight, int(std::lround(h * m_heightScale))); };
    m_rows.clear();
    m_rows.reserve(data.subtitleTracks.size() + data.videoTracks.size() + data.audioTracks.size());
    int y = 0;
    // Subtitle tracks sit above every video track (like Resolve): ST(n) first, ST1 last.
    for (int i = int(data.subtitleTracks.size()) - 1; i >= 0; --i) {
        const TrackData& t = data.subtitleTracks[size_t(i)];
        const int h = scaled(int(std::lround(trackHeight(t) * m_videoHeightScale)));
        m_rows.push_back({TrackRef {TrackType::Subtitle, i}, y, h, 0, t.id});
        y += h;
    }
    // Video tracks grow upwards from the divider: V(n) first, V1 last.
    for (int i = int(data.videoTracks.size()) - 1; i >= 0; --i) {
        const TrackData& t = data.videoTracks[size_t(i)];
        const int lane = m_laneHeights.value(t.id, 0);
        const int h = scaled(int(std::lround(trackHeight(t) * m_videoHeightScale))) + lane;
        m_rows.push_back({TrackRef {TrackType::Video, i}, y, h, lane, t.id});
        y += h;
    }
    m_videoHeight = y;
    y += kSectionGap;
    for (int i = 0; i < int(data.audioTracks.size()); ++i) {
        const TrackData& t = data.audioTracks[size_t(i)];
        const int lane = m_laneHeights.value(t.id, 0);
        const int h = scaled(int(std::lround(trackHeight(t) * m_audioHeightScale))) + lane;
        m_rows.push_back({TrackRef {TrackType::Audio, i}, y, h, lane, t.id});
        y += h;
    }
    m_contentHeight = y;
    m_scrollY = std::clamp(m_scrollY, 0, maxScrollY());
}

std::optional<ScrollAnchor> TimelineLayout::scrollAnchor() const
{
    if (m_scrollY <= 0)
        return std::nullopt;
    // The first row reaching below the top edge (the divider gap belongs to the row above).
    for (const TimelineRow& row : m_rows) {
        if (row.top + row.height > m_scrollY || &row == &m_rows.back())
            return ScrollAnchor {row.trackId, m_scrollY - row.top};
    }
    return std::nullopt;
}

void TimelineLayout::restoreScrollAnchor(const ScrollAnchor& anchor)
{
    for (const TimelineRow& row : m_rows) {
        if (row.trackId == anchor.trackId) {
            setScrollY(row.top + anchor.offset);
            return;
        }
    }
}

int TimelineLayout::trackHeight(const TrackData& track)
{
    if (track.height > 0)
        return std::clamp(track.height, kMinTrackHeight, kMaxTrackHeight);
    switch (track.type) {
    case TrackType::Video:
        return kDefaultVideoHeight;
    case TrackType::Audio:
        return kDefaultAudioHeight;
    case TrackType::Subtitle:
        break;
    }
    return kDefaultSubtitleHeight;
}

// ---- Horizontal ----

double TimelineLayout::setPixelsPerFrame(double pixelsPerFrame, double anchorX)
{
    const double anchorFrame = xToFrame(anchorX);
    m_pixelsPerFrame = std::clamp(pixelsPerFrame, kMinPixelsPerFrame, kMaxPixelsPerFrame);
    // Solve frameToX(anchorFrame) == anchorX for the scroll.
    m_scrollX = std::max(0.0, kHeaderWidth + anchorFrame * m_pixelsPerFrame - anchorX);
    return m_scrollX;
}

int64_t TimelineLayout::nearestFrameAtX(double x) const
{
    return std::max<int64_t>(0, std::llround(xToFrame(x)));
}

int64_t TimelineLayout::frameAtX(double x) const
{
    return std::max<int64_t>(0, int64_t(std::floor(xToFrame(x))));
}

std::pair<int64_t, int64_t> TimelineLayout::visibleFrames() const
{
    const int64_t first = std::max<int64_t>(0, int64_t(std::floor(xToFrame(kHeaderWidth))));
    const int64_t last = int64_t(std::ceil(xToFrame(m_viewSize.width())));
    return {first, std::max(first, last)};
}

double TimelineLayout::fitPixelsPerFrame(int64_t frames) const
{
    // 5 % margin on the right so the last clip end is visible and grabbable.
    const double width = std::max(50.0, trackAreaWidth() * 0.95);
    return std::clamp(
        width / double(std::max<int64_t>(frames, 1)), kMinPixelsPerFrame, kMaxPixelsPerFrame);
}

// ---- Vertical ----

int TimelineLayout::verticalOffset() const
{
    const int area = rowAreaHeight();
    if (m_rowsFromTop || m_contentHeight >= area)
        return 0;
    // Put the divider at the vertical centre, as far as the rows allow.
    return std::clamp(area / 2 - m_videoHeight, 0, area - m_contentHeight);
}

int TimelineLayout::dividerY() const
{
    return kRulerHeight + verticalOffset() + m_videoHeight - m_scrollY;
}

QRect TimelineLayout::rowRect(int index) const
{
    if (index < 0 || index >= int(m_rows.size()))
        return {};
    const TimelineRow& row = m_rows[size_t(index)];
    return QRect(
        0, kRulerHeight + verticalOffset() + row.top - m_scrollY, m_viewSize.width(), row.height);
}

int TimelineLayout::rowAtY(double y) const
{
    if (y < kRulerHeight)
        return -1;
    const double contentY = y - kRulerHeight - verticalOffset() + m_scrollY;
    // Rows are sorted by top: binary search for the last row starting at or above contentY.
    auto it = std::upper_bound(m_rows.cbegin(), m_rows.cend(), contentY,
        [](double v, const TimelineRow& r) { return v < r.top; });
    if (it == m_rows.cbegin())
        return -1;
    --it;
    if (contentY >= it->top + it->height)
        return -1; // in the divider or below the last row
    return int(it - m_rows.cbegin());
}

int TimelineLayout::rowOf(TrackRef ref) const
{
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].ref == ref)
            return int(i);
    }
    return -1;
}

// ---- Headers ----

const std::vector<HeaderButton>& TimelineLayout::headerButtons(TrackType type)
{
    static const std::vector<HeaderButton> video {
        HeaderButton::Visible, HeaderButton::Lock, HeaderButton::AutoSelect};
    static const std::vector<HeaderButton> audio {
        HeaderButton::Solo, HeaderButton::Mute, HeaderButton::Lock, HeaderButton::AutoSelect};
    static const std::vector<HeaderButton> subtitle {
        HeaderButton::Visible, HeaderButton::Lock, HeaderButton::AutoSelect};
    switch (type) {
    case TrackType::Video:
        return video;
    case TrackType::Audio:
        return audio;
    case TrackType::Subtitle:
        break;
    }
    return subtitle;
}

const std::vector<HeaderButton>& TimelineLayout::buttonsFor(TrackType type) const
{
    static const std::vector<HeaderButton> audioPage {HeaderButton::Mute, HeaderButton::Solo,
        HeaderButton::Arm, HeaderButton::Lock, HeaderButton::AutoSelect};
    return m_audioPageHeaders && type == TrackType::Audio ? audioPage : headerButtons(type);
}

bool TimelineLayout::twoLineHeader(int row) const
{
    if (!m_audioPageHeaders || row < 0 || row >= int(m_rows.size())
        || m_rows[size_t(row)].ref.type != TrackType::Audio)
        return false;
    const QRect r = rowRect(row);
    return r.height() - m_rows[size_t(row)].laneHeight >= kTwoLineHeaderMinHeight;
}

QRect TimelineLayout::headerMeterRect(int row) const
{
    const QRect r = rowRect(row);
    if (r.isEmpty() || !m_audioPageHeaders || m_rows[size_t(row)].ref.type != TrackType::Audio)
        return {};
    const int lane = m_rows[size_t(row)].laneHeight;
    return QRect(kHeaderWidth - 4 - kHeaderMeterWidth, r.top() + 3, kHeaderMeterWidth,
        r.height() - lane - 6);
}

QRect TimelineLayout::headerButtonRect(int row, HeaderButton button) const
{
    const QRect r = rowRect(row);
    if (r.isEmpty() || button == HeaderButton::None)
        return {};
    // Buttons sit on the first line of the header, vertically centred in small rows.
    int y = r.top() + std::min(6, (r.height() - kHeaderButton) / 2);
    if (button == HeaderButton::Target)
        return QRect(10, y, 30, kHeaderButton);
    const TrackType type = m_rows[size_t(row)].ref.type;
    // Audio page: the buttons and meter leave no room for the name on one line, so tall
    // headers put the buttons on a second line (the name keeps the first).
    if (twoLineHeader(row))
        y += kHeaderButton + kHeaderLineGap;
    const auto& buttons = buttonsFor(type);
    int x = kHeaderWidth - 8 - kHeaderButton;
    if (m_audioPageHeaders && type == TrackType::Audio)
        x -= kHeaderMeterWidth + 2; // the meter sits at the right edge
    for (HeaderButton b : buttons) {
        if (b == button)
            return QRect(x, y, kHeaderButton, kHeaderButton);
        x -= kHeaderButton + kHeaderButtonGap;
    }
    return {};
}

QRect TimelineLayout::headerNameRect(int row) const
{
    const QRect patch = headerButtonRect(row, HeaderButton::Target);
    if (patch.isEmpty())
        return {};
    // The same bounds as TimelinePainter's track name.
    int right = kHeaderWidth;
    if (twoLineHeader(row)) {
        right = kHeaderWidth - 4 - kHeaderMeterWidth;
    } else {
        for (HeaderButton b : buttonsFor(m_rows[size_t(row)].ref.type))
            right = std::min(right, headerButtonRect(row, b).left());
    }
    const int left = patch.right() + 4;
    if (right - 2 - left < 14)
        return {};
    return QRect(left, patch.top(), right - 2 - left, patch.height());
}

// ---- Clips ----

std::pair<size_t, size_t> TimelineLayout::clipRange(const TrackData& track, double f0, double f1)
{
    const auto& clips = track.clips;
    const auto first = std::upper_bound(clips.cbegin(), clips.cend(), f0,
        [](double f, const ClipData& c) { return f < double(c.end()); });
    const auto last = std::lower_bound(
        first, clips.cend(), f1, [](const ClipData& c, double f) { return double(c.start) < f; });
    return {size_t(first - clips.cbegin()), size_t(last - clips.cbegin())};
}

QRectF TimelineLayout::clipRect(int row, const ClipData& clip) const
{
    const QRect r = rowRect(row);
    const double x0 = frameToX(double(clip.start));
    const double x1 = frameToX(double(clip.end()));
    const int lane = r.isEmpty() ? 0 : m_rows[size_t(row)].laneHeight;
    return QRectF(x0, r.top() + 1, x1 - x0, r.height() - lane - 2);
}

QRect TimelineLayout::laneRect(int index) const
{
    const QRect r = rowRect(index);
    if (r.isEmpty() || m_rows[size_t(index)].laneHeight <= 0)
        return {};
    const int lane = m_rows[size_t(index)].laneHeight;
    return QRect(r.left(), r.bottom() + 1 - lane, r.width(), lane);
}

TimelineHit TimelineLayout::hitTest(const TimelineData& data, QPointF pos) const
{
    TimelineHit hit;
    hit.frame = xToFrame(pos.x());
    if (pos.y() < kRulerHeight) {
        if (pos.x() >= kHeaderWidth)
            hit.area = TimelineHit::Area::Ruler;
        return hit;
    }
    hit.row = rowAtY(pos.y());
    if (hit.row < 0)
        return hit;
    const TimelineRow& row = m_rows[size_t(hit.row)];
    hit.track = row.ref;

    if (pos.x() < kHeaderWidth) {
        const QRect r = rowRect(hit.row);
        hit.area = pos.y() >= r.bottom() + 1 - kResizeGrabPx ? TimelineHit::Area::HeaderResize
                                                             : TimelineHit::Area::Header;
        if (hit.area == TimelineHit::Area::Header) {
            const QPoint p = pos.toPoint();
            if (headerButtonRect(hit.row, HeaderButton::Target).contains(p))
                hit.button = HeaderButton::Target;
            for (HeaderButton b : buttonsFor(row.ref.type)) {
                if (headerButtonRect(hit.row, b).contains(p))
                    hit.button = b;
            }
        }
        return hit;
    }

    hit.area = TimelineHit::Area::Track;
    const TrackData* track = data.track(row.ref);
    if (!track)
        return hit;
    // The keyframe / curve lane below the clips has no clip parts (its widget takes input).
    if (row.laneHeight > 0 && pos.y() >= rowRect(hit.row).bottom() + 1 - row.laneHeight)
        return hit;
    // Look at the clips within one grab zone of the pointer; at most a few qualify.
    const double zoneFrames = kEdgeGrabPx / m_pixelsPerFrame;
    const auto [first, last] = clipRange(*track, hit.frame - zoneFrames, hit.frame + zoneFrames);
    double bestDistance = kEdgeGrabPx + 1.0;
    for (size_t i = first; i < last; ++i) {
        const ClipData& c = track->clips[i];
        const double x0 = frameToX(double(c.start));
        const double x1 = frameToX(double(c.end()));
        const bool inside = pos.x() >= x0 && pos.x() < x1;
        // Short clips keep a body to grab: the edge zone is at most a third of the width.
        const double zone = std::min<double>(kEdgeGrabPx, (x1 - x0) / 3.0);
        if (inside && hit.part == ClipPart::None) {
            hit.area = TimelineHit::Area::Clip;
            hit.clipId = c.id;
            hit.part = ClipPart::Body;
        }
        // At a cut both neighbours are equally close: a small penalty for being outside the
        // clip makes the side the pointer is on win.
        const double outside = inside ? 0.0 : 0.5;
        const double dHead = std::abs(pos.x() - x0) + outside;
        const double dTail = std::abs(pos.x() - x1) + outside;
        // Edge zones extend a little outside the clip (into a gap) so thin edges are easy to
        // grab; inside, they must stay within the clip's own zone.
        if (dHead <= std::max(zone, 2.0) + outside && (inside || pos.x() < x0)
            && dHead < bestDistance) {
            bestDistance = dHead;
            hit.area = TimelineHit::Area::Clip;
            hit.clipId = c.id;
            hit.part = ClipPart::Head;
        }
        if (dTail <= std::max(zone, 2.0) + outside && (inside || pos.x() >= x1)
            && dTail < bestDistance) {
            bestDistance = dTail;
            hit.area = TimelineHit::Area::Clip;
            hit.clipId = c.id;
            hit.part = ClipPart::Tail;
        }
    }
    // An edge shared by two touching clips is an edit point (roll in the trim tool).
    if (hit.part == ClipPart::Head || hit.part == ClipPart::Tail) {
        const int index = track->indexOfClip(hit.clipId);
        const auto& clips = track->clips;
        if (hit.part == ClipPart::Head && index > 0
            && clips[size_t(index - 1)].end() == clips[size_t(index)].start) {
            hit.cutLeft = clips[size_t(index - 1)].id;
            hit.cutRight = hit.clipId;
        } else if (hit.part == ClipPart::Tail && index + 1 < int(clips.size())
            && clips[size_t(index + 1)].start == clips[size_t(index)].end()) {
            hit.cutLeft = hit.clipId;
            hit.cutRight = clips[size_t(index + 1)].id;
        }
    }
    return hit;
}

} // namespace kk
