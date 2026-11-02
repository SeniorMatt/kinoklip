// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePainter implementation. Hot loop: paintRow()/paintClip() for every visible clip.
// Rules for the hot loop: no QString formatting (names come from an elide cache), no
// QPainterPath, no per-clip heap allocations (scratch buffers are members), cheap fillRect for
// everything that can be a rectangle.
#include "ui/timeline/TimelinePainter.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "ui/common/Icons.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/NestedPreview.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace kk {

using namespace timelinemetrics;

namespace {

// ---- Colours ----
// Clip, track-strip and state colours are content colours (like clip labels): saturated mid
// tones with light text that read on light and dark colour schemes alike. Everything around
// them (lanes, headers, ruler, lines, overlays) is derived from the palette in setStyle().
const QColor kVideoClip(0x2f, 0x62, 0x96);
const QColor kAudioClip(0x2c, 0x7a, 0x58);
const QColor kTitleClip(0x80, 0x5c, 0xb0);
const QColor kSolidClip(0x6c, 0x6c, 0x78);
/// Compound clips / nested timelines (video and audio parts alike): a teal unlike the media
/// colours, so a nested clip is recognisable at a glance.
const QColor kNestedClip(0x2a, 0x7f, 0x8a);
const QColor kNestedAudioClip(0x3b, 0x77, 0x6a);
const QColor kSelection(0xff, 0xa8, 0x3a);
const QColor kPlayhead(0xe5, 0x48, 0x4d);
const QColor kSnapLine(0xf4, 0xd6, 0x4e);
const QColor kLockedOn(0xc8, 0x55, 0x4e);
const QColor kMuteOn(0xd0, 0x6a, 0x3a);
const QColor kSoloOn(0xd8, 0xb8, 0x3a);
const QColor kVideoStrip(0x3d, 0x7f, 0xc4);
const QColor kAudioStrip(0x3b, 0xa0, 0x6e);

constexpr double kBandHeight = 15; ///< clip title band (name, fx badge)
constexpr int kMinBandRow = 30; ///< rows lower than this show only the band
constexpr int kMajorTickSpacingPx = 128; ///< aim for labelled ruler ticks this far apart
constexpr int kMinMinorTickPx = 7;
constexpr int kIconSize = 14;

QString qtr(const char* text)
{
    return QCoreApplication::translate("kk::TimelinePainter", text);
}

/// Major ruler step in frames: the smallest "nice" step at least kMajorTickSpacingPx wide.
int64_t majorTickFrames(double ppf, int fpsInt)
{
    static const int kFrameSteps[] = {1, 2, 5, 10};
    static const int kSecondSteps[]
        = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400};
    for (int s : kFrameSteps) {
        if (s < fpsInt && s * ppf >= kMajorTickSpacingPx)
            return s;
    }
    for (int s : kSecondSteps) {
        if (int64_t(s) * fpsInt * ppf >= kMajorTickSpacingPx)
            return int64_t(s) * fpsInt;
    }
    return int64_t(14400) * fpsInt;
}

/// Largest subdivision of @p major whose ticks are at least kMinMinorTickPx apart (0 = none).
int64_t minorTickFrames(int64_t major, double ppf)
{
    for (int d : {10, 6, 5, 4, 2}) {
        if (major % d == 0 && (major / d) * ppf >= kMinMinorTickPx)
            return major / d;
    }
    return 0;
}

/// Linear blend of @p a towards @p b (0 = a, 1 = b).
QColor mixColor(const QColor& a, const QColor& b, double amount)
{
    const auto lerp = [amount](int x, int y) { return int(x + (y - x) * amount + 0.5); };
    return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()));
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

void drawDiamond(QPainter& p, QPointF c, double r)
{
    const QPointF pts[4]
        = {{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r}, {c.x() - r, c.y()}};
    p.drawConvexPolygon(pts, 4);
}

} // namespace

TimelinePainter::TimelinePainter()
{
    m_lines.reserve(4096);
    m_peaks.reserve(4096);
}

void TimelinePainter::setStyle(const QPalette& pal, const QFont& font)
{
    m_font = font;
    m_smallFont = font;
    m_smallFont.setPointSizeF(std::max(6.0, font.pointSizeF() - 1.5));
    m_boldFont = font;
    m_boldFont.setBold(true);
    m_fontAscent = QFontMetrics(m_smallFont).ascent();
    m_base = pal.color(QPalette::Base);
    m_window = pal.color(QPalette::Window);
    m_text = pal.color(QPalette::WindowText);
    m_textDim = pal.color(QPalette::PlaceholderText);
    m_accent = pal.color(QPalette::Highlight);
    // Lanes: the view background nudged towards the text colour, so alternate rows stay
    // distinguishable in light schemes too (lighter() does nothing on white).
    m_lane = mixColor(m_base, m_text, 0.05);
    m_laneAlt = mixColor(m_base, m_text, 0.08);
    m_line = pal.color(QPalette::Dark);
    // Playhead timecode: warm orange (NLE convention), darker on light schemes for contrast.
    m_timecode = m_window.lightness() < 128 ? QColor(0xff, 0x8a, 0x6a) : QColor(0xc4, 0x4a, 0x1c);
    m_names.clear();
    // Header icons are pre-rendered once per palette (the paint loop only blits pixmaps);
    // re-rendering here picks up the new colours after a colour scheme switch.
    auto pix = [](const char* name) { return kk::icon(QLatin1String(name)).pixmap(kIconSize); };
    m_iconLock = pix("lock");
    m_iconUnlock = pix("unlock");
    m_iconVisible = pix("visible");
    m_iconHidden = pix("hidden");
    m_iconMute = pix("mute");
    m_iconSolo = pix("solo");
}

QColor TimelinePainter::clipColor(const ClipData& clip, TrackType type)
{
    if (clip.label.isValid())
        return clip.label;
    switch (clip.kind) {
    case ClipKind::Title:
        return kTitleClip;
    case ClipKind::Solid:
        return kSolidClip;
    case ClipKind::Nested:
        return type == TrackType::Video ? kNestedClip : kNestedAudioClip;
    case ClipKind::Media:
        break;
    }
    return type == TrackType::Video ? kVideoClip : kAudioClip;
}

const QString& TimelinePainter::elidedName(const ClipData& clip, int width)
{
    NameCache& entry = m_names[clip.id];
    if (entry.width != width || entry.name != clip.name) {
        entry.name = clip.name;
        entry.width = width;
        entry.elided = QFontMetrics(m_smallFont).elidedText(clip.name, Qt::ElideRight, width);
    }
    return entry.elided;
}

QString TimelinePainter::mediaPath(const TimelinePaintInput& in, const QString& mediaId) const
{
    if (!in.mediaPool)
        return {};
    const MediaItem* item = in.mediaPool->item(mediaId);
    return item ? item->path : QString();
}

QString TimelinePainter::clipPreviewPath(const ClipData& clip, const MediaItem* item)
{
    // Nested clips: the providers resolve the pseudo path to the nested timeline's preview.
    if (clip.kind == ClipKind::Nested)
        return nestedPreviewPath(clip.nestedUid);
    return item ? item->path : QString();
}

// ---- Entry point ----

void TimelinePainter::paint(QPainter& p, const TimelinePaintInput& in)
{
    if (m_names.size() > 20000)
        m_names.clear(); // bound the cache after huge edits (ids are never reused)
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRect(QPoint(0, 0), in.layout->viewSize()), m_base);
    paintTrackArea(p, in);
    paintOverlays(p, in);
    paintHeaders(p, in);
    paintRuler(p, in);
    paintPlayhead(p, in);
}

// ---- Track area ----

void TimelinePainter::paintTrackArea(QPainter& p, const TimelinePaintInput& in)
{
    const TimelineLayout& L = *in.layout;
    const QSize view = L.viewSize();
    const QRect area(
        kHeaderWidth, kRulerHeight, view.width() - kHeaderWidth, view.height() - kRulerHeight);
    p.setClipRect(area);

    // Divider between video and audio.
    const int divider = L.dividerY();
    p.fillRect(QRect(kHeaderWidth, divider, area.width(), kSectionGap), m_line);

    for (int row = 0; row < int(L.rows().size()); ++row) {
        const QRect r = L.rowRect(row);
        if (r.bottom() < kRulerHeight || r.top() > view.height())
            continue;
        paintRow(p, in, row);
    }
    p.setClipping(false);
}

void TimelinePainter::paintRow(QPainter& p, const TimelinePaintInput& in, int row)
{
    const TimelineLayout& L = *in.layout;
    const TimelineRow& info = L.rows()[size_t(row)];
    const TrackData* track = in.data->track(info.ref);
    if (!track)
        return;
    const QRect r = L.rowRect(row);
    const int width = L.viewSize().width();
    p.fillRect(QRect(kHeaderWidth, r.top(), width - kHeaderWidth, r.height()),
        (row % 2) ? m_laneAlt : m_lane);
    p.fillRect(QRect(kHeaderWidth, r.bottom(), width - kHeaderWidth, 1), m_line);
    // In/out range: a tint on the lanes (under the clips).
    const TimelineData& d = *in.data;
    if (d.markIn >= 0 || d.markOut >= 0) {
        const double x0 = d.markIn >= 0 ? L.frameToX(double(d.markIn)) : kHeaderWidth;
        const double x1 = d.markOut >= 0 ? L.frameToX(double(d.markOut)) : width;
        QColor tint = m_accent;
        tint.setAlpha(22);
        p.fillRect(QRectF(x0, r.top(), x1 - x0, r.height() - 1), tint);
    }

    const auto [first, last]
        = TimelineLayout::clipRange(*track, L.xToFrame(kHeaderWidth), L.xToFrame(width));
    for (size_t i = first; i < last; ++i)
        paintClip(p, in, row, *track, track->clips[i]);
    paintTransitions(p, in, row, *track, first, last);

    // Hidden video / muted audio / locked tracks are dimmed as a whole.
    const bool off = info.ref.type == TrackType::Video ? !track->enabled : track->muted;
    if (off || track->locked) {
        p.fillRect(QRect(kHeaderWidth, r.top(), width - kHeaderWidth, r.height()),
            QColor(10, 10, 12, track->locked ? 70 : 110));
    }
    if (track->locked) {
        // Diagonal hatching marks locked tracks (one drawLines call, buffer reused).
        m_lines.clear();
        for (int x = kHeaderWidth - r.height(); x < width; x += 12)
            m_lines.emplace_back(x, r.bottom(), x + r.height(), r.top());
        p.setPen(withAlpha(m_text, 18));
        p.drawLines(m_lines.data(), int(m_lines.size()));
    }
}

void TimelinePainter::paintClip(QPainter& p, const TimelinePaintInput& in, int row,
    const TrackData& track, const ClipData& clip)
{
    const TimelineLayout& L = *in.layout;
    const QRectF r = L.clipRect(row, clip);
    const double viewRight = L.viewSize().width();
    const double visLeft = std::max(r.left(), double(kHeaderWidth));
    const double visRight = std::min(r.right(), viewRight);
    if (visRight <= visLeft && r.width() >= 1)
        return;
    const bool selected = in.selected->contains(clip.id);
    const QColor base = clipColor(clip, track.type);

    // Sub-pixel / tiny clips (zoomed far out): one rectangle, nothing else.
    if (r.width() < 4) {
        p.fillRect(QRectF(r.left(), r.top(), std::max(1.0, r.width()), r.height()),
            selected ? kSelection : (clip.enabled ? base.lighter(115) : base.darker(200)));
        return;
    }

    const double bandH = r.height() >= kMinBandRow ? kBandHeight : r.height();
    const QRectF visible(visLeft, r.top(), visRight - visLeft, r.height());
    const QRectF band(visLeft, r.top(), visible.width(), bandH);
    const QRectF content(visLeft, r.top() + bandH, visible.width(), r.height() - bandH);

    p.fillRect(band, base);
    const QColor contentColor = base.darker(165);
    p.fillRect(content, contentColor);

    if (content.height() >= 6) {
        switch (clip.kind) {
        case ClipKind::Media:
        case ClipKind::Nested:
            if (track.type == TrackType::Video)
                paintThumbnails(p, in, clip,
                    QRectF(r.left(), content.top(), r.width(), content.height()), content);
            else {
                const QRectF full(r.left(), content.top(), r.width(), content.height());
                paintWaveform(p, in, clip, full, content, base.lighter(165));
                paintVolumeLine(p, in, clip, full, content);
            }
            break;
        case ClipKind::Solid:
            p.fillRect(content.adjusted(0, 2, 0, -2),
                clip.solidColor.isValid() ? clip.solidColor : kSolidClip);
            break;
        case ClipKind::Title:
            if (content.height() >= 14 && visible.width() > 30) {
                p.setFont(m_smallFont);
                p.setPen(base.lighter(170));
                // Rect-based drawText clips to the content; titles are rare, so the layout
                // cost is acceptable here.
                p.drawText(content.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    clip.title.staticValue(keys::Text).toString());
            }
            break;
        }
    }

    // Edges: highlight on top, dark outline around.
    p.fillRect(QRectF(visLeft, r.top(), visible.width(), 1), base.lighter(140));
    p.setPen(base.darker(230));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(r.left(), r.top(), r.width() - 1, r.height() - 1));

    // Name (sticky: stays readable when the clip start is scrolled off), duration, fx badge.
    const bool hasFx = !clip.effects.isEmpty();
    const double badgeW = hasFx ? 18 : 0;
    const int nameRoom = int(visible.width() - 10 - badgeW);
    if (nameRoom > 12 && bandH >= 12) {
        p.setFont(m_smallFont);
        p.setPen(clip.enabled ? QColor(0xf2, 0xf2, 0xf4) : m_textDim);
        const QString& name = elidedName(clip, nameRoom);
        const double baseline = r.top() + (bandH + m_fontAscent) / 2.0 - 1;
        p.drawText(QPointF(visLeft + 5, baseline), name);
        // Duration only when the full name fits with room to spare (duration-aware text).
        if (name == clip.name && nameRoom > 150) {
            const QFontMetrics fm(m_smallFont);
            const int nameW = fm.horizontalAdvance(name);
            const QString dur = timecode::format(clip.duration, in.data->fps);
            const int durW = fm.horizontalAdvance(dur);
            if (nameW + durW + 24 < nameRoom) {
                p.setPen(base.lighter(175));
                p.drawText(QPointF(visRight - 6 - badgeW - durW, baseline), dur);
            }
        }
    }
    if (const QRectF badge = hasFx ? fxBadgeRect(r, viewRight) : QRectF(); !badge.isEmpty()) {
        p.fillRect(badge, base.darker(190));
        p.setPen(QColor(0xf0, 0xc8, 0x6a));
        p.setFont(m_smallFont);
        p.drawText(badge, Qt::AlignCenter, QStringLiteral("fx"));
    }

    if (r.height() >= 24)
        paintKeyframes(p, in, clip, r);

    // Clip markers (spec 18.4): small coloured diamonds in the name band. Markers are
    // clip-local frames; only those inside the visible part of the clip are drawn.
    if (!clip.markers.isEmpty() && bandH >= 8) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const double cy = r.top() + bandH / 2.0;
        for (const Marker& m : clip.markers) {
            if (m.frame < 0 || m.frame >= clip.duration)
                continue;
            const double x = L.frameToX(double(clip.start + m.frame));
            if (x < visLeft - 4 || x > visRight + 4)
                continue;
            const QPointF pts[4] = {{x, cy - 4}, {x + 4, cy}, {x, cy + 4}, {x - 4, cy}};
            p.setPen(m.color.darker(170));
            p.setBrush(m.color);
            p.drawConvexPolygon(pts, 4);
        }
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    if (!clip.enabled)
        p.fillRect(visible, QColor(12, 12, 14, 150));
    if (selected) {
        p.setPen(QPen(kSelection, 2));
        p.drawRect(QRectF(r.left() + 1, r.top() + 1, r.width() - 2, r.height() - 2));
    }
}

void TimelinePainter::paintThumbnails(QPainter& p, const TimelinePaintInput& in,
    const ClipData& clip, const QRectF& full, const QRectF& visible)
{
    const int tileH = int(full.height()) - 2;
    if (tileH < 10 || visible.width() < 6)
        return;
    const MediaItem* item = in.mediaPool ? in.mediaPool->item(clip.mediaId) : nullptr;
    double aspect = 16.0 / 9.0;
    if (item && item->info.width > 0 && item->info.height > 0) {
        aspect = double(item->info.width) / item->info.height;
        if (item->info.rotation == 90 || item->info.rotation == 270)
            aspect = 1.0 / aspect;
    }
    const double tileW = std::max(8.0, std::round(tileH * aspect));
    const double top = full.top() + 1;
    const TimelineLayout& L = *in.layout;
    const QColor placeholder = clipColor(clip, TrackType::Video).darker(210);
    const QColor separator = clipColor(clip, TrackType::Video).darker(260);

    p.save();
    p.setClipRect(visible.adjusted(1, 0, -1, 0), Qt::IntersectClip);
    // Tiles are anchored at the clip start so they do not swim while scrolling.
    const int firstTile = int(std::floor((visible.left() - full.left()) / tileW));
    for (int i = std::max(0, firstTile);; ++i) {
        const double x = full.left() + i * tileW;
        if (x >= visible.right() || x >= full.right())
            break;
        const QRectF tile(x, top, tileW, tileH);
        QImage image;
        if (m_thumbnails && (item || clip.kind == ClipKind::Nested)) {
            const int64_t frame = clip.start + int64_t((x - full.left()) / L.pixelsPerFrame());
            image = m_thumbnails(
                clipPreviewPath(clip, item), clip.sourceTimeUsAt(frame, in.data->fps), tileH);
        }
        if (image.isNull())
            p.fillRect(tile.adjusted(0, 0, -1, 0), placeholder);
        else
            p.drawImage(tile, image);
        p.fillRect(QRectF(x + tileW - 1, top, 1, tileH), separator);
    }
    p.restore();
}

void TimelinePainter::paintWaveform(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
    const QRectF& full, const QRectF& visible, const QColor& color)
{
    const double mid = full.center().y();
    const double half = full.height() / 2.0 - 2;
    const int columns = int(visible.width());
    if (columns <= 0 || half < 1)
        return;
    const TimelineLayout& L = *in.layout;
    const Rational fps = in.data->fps;
    bool ok = false;
    if (m_waveforms && in.mediaPool) {
        const MediaItem* item = in.mediaPool->item(clip.mediaId);
        if (item || clip.kind == ClipKind::Nested) {
            const double usPerFrame = fps.isValid() ? 1e6 * fps.den / fps.num * clip.speed : 0;
            const double startFrame = L.xToFrame(visible.left()) - double(clip.start);
            const double startUs = double(clip.sourceInUs) + startFrame * usPerFrame;
            m_peaks.resize(size_t(columns)); // capacity is kept between paints
            ok = m_waveforms(clipPreviewPath(clip, item), startUs, usPerFrame / L.pixelsPerFrame(),
                columns, m_peaks.data());
        }
    }
    p.setPen(color);
    if (!ok) {
        p.drawLine(QPointF(visible.left(), mid), QPointF(visible.right(), mid));
        return;
    }
    m_lines.clear();
    const double x0 = visible.left() + 0.5;
    for (int c = 0; c < columns; ++c) {
        const double a = std::clamp(m_peaks[size_t(c)], 0.0f, 1.0f) * half;
        if (a >= 0.5)
            m_lines.emplace_back(x0 + c, mid - a, x0 + c, mid + a);
    }
    p.drawLines(m_lines.data(), int(m_lines.size()));
    p.fillRect(QRectF(visible.left(), mid, visible.width(), 1), color.darker(130));
}

void TimelinePainter::paintVolumeLine(QPainter& p, const TimelinePaintInput& in,
    const ClipData& clip, const QRectF& full, const QRectF& visible)
{
    if (full.height() < 14)
        return;
    // Gain mapped linearly in dB: +12 dB at the top, -60 dB (and below) at the bottom.
    constexpr double kTopDb = 12.0;
    constexpr double kBottomDb = -60.0;
    const auto yFor = [&](double db) {
        const double t = (kTopDb - std::clamp(db, kBottomDb, kTopDb)) / (kTopDb - kBottomDb);
        return full.top() + 2 + t * (full.height() - 4);
    };
    const TimelineLayout& L = *in.layout;
    const auto it = clip.audio.values.constFind(keys::VolumeDb);
    m_lines.clear();
    if (it == clip.audio.values.cend() || it->keys.isEmpty()) {
        const double db = it == clip.audio.values.cend() ? 0.0 : it->base;
        const double y = yFor(db);
        m_lines.emplace_back(visible.left(), y, visible.right(), y);
    } else {
        // Straight segments between keys (linear approximation of the eases is enough for an
        // overview), flat before the first and after the last key.
        const auto& keys = it->keys;
        double prevX = full.left();
        double prevY = yFor(keys.front().value);
        for (const Keyframe& k : keys) {
            const double x = L.frameToX(double(clip.start + k.frame));
            const double y = yFor(k.value);
            if (x >= visible.left() - 1 || prevX <= visible.right() + 1)
                m_lines.emplace_back(prevX, prevY, x, y);
            prevX = x;
            prevY = y;
        }
        m_lines.emplace_back(prevX, prevY, full.right(), prevY);
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(0xff, 0xe0, 0x80, 210), 1.2));
    p.drawLines(m_lines.data(), int(m_lines.size()));
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelinePainter::paintKeyframes(
    QPainter& p, const TimelinePaintInput& in, const ClipData& clip, const QRectF& r)
{
    const TimelineLayout& L = *in.layout;
    const double y = r.bottom() - 5;
    const double left = std::max(r.left(), double(kHeaderWidth)) - 4;
    const double right = std::min(r.right(), double(L.viewSize().width())) + 4;
    bool penSet = false;
    auto drawSet = [&](const ParamSet& set) {
        for (auto it = set.values.cbegin(); it != set.values.cend(); ++it) {
            for (const Keyframe& k : it->keys) {
                const double x = L.frameToX(double(clip.start + k.frame));
                if (x < left || x > right)
                    continue;
                if (!penSet) {
                    p.setPen(QColor(20, 20, 20));
                    p.setBrush(QColor(0xf4, 0xe2, 0x9a));
                    p.setRenderHint(QPainter::Antialiasing, true);
                    penSet = true;
                }
                drawDiamond(p, QPointF(x, y), 3.5);
            }
        }
    };
    drawSet(clip.transform);
    drawSet(clip.audio);
    drawSet(clip.title);
    for (const EffectInstance& fx : clip.effects)
        drawSet(fx.params);
    if (penSet) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
    }
}

QRectF TimelinePainter::fxBadgeRect(const QRectF& r, double viewRight)
{
    constexpr double badgeW = 18;
    const double visLeft = std::max(r.left(), double(kHeaderWidth));
    const double visRight = std::min(r.right(), viewRight);
    const double bandH = r.height() >= kMinBandRow ? kBandHeight : r.height();
    if (r.width() < 4 || visRight - visLeft <= 30 || bandH < 12)
        return {};
    return QRectF(visRight - badgeW - 3, r.top() + 2, badgeW, bandH - 4);
}

void TimelinePainter::paintTransitionBlock(QPainter& p, const QRectF& r, bool in, bool selected)
{
    // A bevelled block: light face, bright top/left edges, dark bottom/right edges and a
    // diagonal showing the direction of the blend.
    p.fillRect(r, QColor(0xd8, 0xd8, 0xe2, 150));
    p.fillRect(QRectF(r.left(), r.top(), r.width(), 1), QColor(255, 255, 255, 200));
    p.fillRect(QRectF(r.left(), r.top(), 1, r.height()), QColor(255, 255, 255, 160));
    p.fillRect(QRectF(r.left(), r.bottom() - 1, r.width(), 1), QColor(0, 0, 0, 160));
    p.fillRect(QRectF(r.right() - 1, r.top(), 1, r.height()), QColor(0, 0, 0, 160));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(40, 40, 50, 200), 1));
    if (in)
        p.drawLine(r.bottomLeft() + QPointF(1, -1), r.topRight() + QPointF(-1, 1));
    else
        p.drawLine(r.topLeft() + QPointF(1, 1), r.bottomRight() + QPointF(-1, -1));
    p.setRenderHint(QPainter::Antialiasing, false);
    if (selected) {
        // Same colour as a selected clip's outline.
        p.setPen(QPen(kSelection, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -1, -1));
    }
}

void TimelinePainter::paintTransitions(QPainter& p, const TimelinePaintInput& in, int row,
    const TrackData& track, size_t first, size_t last)
{
    const TimelineLayout& L = *in.layout;
    // Transitions at a cut reach into the neighbour: look one clip further.
    const size_t from = first > 0 ? first - 1 : 0;
    const size_t to = std::min(track.clips.size(), last + 1);
    const QRect rr = L.rowRect(row);
    const double top = rr.top() + 3;
    const double h = rr.height() - 6;
    for (size_t i = from; i < to; ++i) {
        const ClipData& c = track.clips[i];
        if (c.transitionIn) {
            const bool touching = i > 0 && track.clips[i - 1].end() == c.start;
            const double d = double(c.transitionIn->duration);
            const double f0
                = touching ? double(c.start - c.transitionIn->framesBeforeCut()) : double(c.start);
            const double x0 = L.frameToX(f0);
            const double x1 = L.frameToX(f0 + d);
            if (x1 - x0 >= 3)
                paintTransitionBlock(p, QRectF(x0, top, x1 - x0, h), true,
                    in.transition == TransitionRef {c.id, true});
        }
        if (c.transitionOut) {
            const bool touching = i + 1 < track.clips.size() && track.clips[i + 1].start == c.end();
            const double d = double(c.transitionOut->duration);
            const double f1 = touching ? double(c.end() - c.transitionOut->framesBeforeCut()) + d
                                       : double(c.end());
            const double x0 = L.frameToX(f1 - d);
            const double x1 = L.frameToX(f1);
            if (x1 - x0 >= 3)
                paintTransitionBlock(p, QRectF(x0, top, x1 - x0, h), false,
                    in.transition == TransitionRef {c.id, false});
        }
    }
}

// ---- Overlays (drawn above clips, below headers/ruler) ----

void TimelinePainter::paintOverlays(QPainter& p, const TimelinePaintInput& in)
{
    const TimelineLayout& L = *in.layout;
    const TimelineOverlay& o = *in.overlay;
    const QSize view = L.viewSize();
    p.setClipRect(QRect(
        kHeaderWidth, kRulerHeight, view.width() - kHeaderWidth, view.height() - kRulerHeight));

    for (const TimelineGhost& g : o.ghosts) {
        const QRect rr = g.row >= 0 ? L.rowRect(g.row) : g.band;
        if (rr.isEmpty())
            continue;
        const double x0 = L.frameToX(double(g.start));
        const double x1 = L.frameToX(double(g.start + g.duration));
        const QRectF r(x0, rr.top() + 1, std::max(2.0, x1 - x0), rr.height() - 2);
        p.fillRect(r, withAlpha(m_text, 46));
        p.setPen(QPen(withAlpha(m_text, 190), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
        if (r.width() > 30) {
            p.setFont(m_smallFont);
            p.setPen(m_text);
            p.drawText(QPointF(std::max(x0, double(kHeaderWidth)) + 5, r.top() + m_fontAscent + 2),
                g.name);
        }
    }
    if (o.dropClip) {
        if (const auto loc = editops::findClip(*in.data, o.dropClip)) {
            const int row = L.rowOf(loc->track);
            const ClipData& c = in.data->track(loc->track)->clips[size_t(loc->clipIndex)];
            p.setPen(QPen(m_accent, 2));
            p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 40));
            p.drawRect(L.clipRect(row, c).adjusted(1, 1, -1, -1));
            p.setBrush(Qt::NoBrush);
        }
    }
    if (o.dropTransitionRow >= 0) {
        const QRect rr = L.rowRect(o.dropTransitionRow);
        const double x0 = L.frameToX(double(o.dropTransitionFrom));
        const double x1 = L.frameToX(double(o.dropTransitionTo));
        paintTransitionBlock(
            p, QRectF(x0, rr.top() + 3, std::max(4.0, x1 - x0), rr.height() - 6), true);
        p.setPen(QPen(m_accent, 2));
        p.drawRect(QRectF(x0, rr.top() + 3, std::max(4.0, x1 - x0), rr.height() - 6));
    }
    if (in.gapRow >= 0 && in.gapEnd > in.gapStart) {
        const QRect rr = L.rowRect(in.gapRow);
        const double x0 = L.frameToX(double(in.gapStart));
        const double x1 = L.frameToX(double(in.gapEnd));
        const QRectF r(x0, rr.top() + 2, std::max(2.0, x1 - x0), rr.height() - 4);
        p.fillRect(r, QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 55));
        p.setPen(QPen(m_accent, 1));
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    // Selected edit point: a thick bracket on the selected side(s) of the cut.
    for (const quint64 id : {in.editTailClip, in.editHeadClip}) {
        if (!id)
            continue;
        const auto loc = editops::findClip(*in.data, id);
        if (!loc)
            continue;
        const int row = L.rowOf(loc->track);
        const ClipData& c = in.data->track(loc->track)->clips[size_t(loc->clipIndex)];
        const QRectF cr = L.clipRect(row, c);
        const bool tail = id == in.editTailClip;
        const double x = tail ? cr.right() - 3 : cr.left();
        p.fillRect(QRectF(x, cr.top(), 3, cr.height()), kSnapLine);
    }
    if (o.bladeFrame && o.bladeRow >= 0) {
        const QRect rr = L.rowRect(o.bladeRow);
        const double x = std::round(L.frameToX(double(*o.bladeFrame))) + 0.5;
        p.setPen(QPen(kPlayhead, 1, Qt::DashLine));
        p.drawLine(QPointF(x, rr.top()), QPointF(x, rr.bottom()));
    }
    if (o.snapFrame) {
        const double x = std::round(L.frameToX(double(*o.snapFrame))) + 0.5;
        p.setPen(QPen(kSnapLine, 1));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, view.height()));
    }
    if (!o.rubberBand.isEmpty()) {
        p.fillRect(o.rubberBand, QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 40));
        p.setPen(m_accent);
        p.drawRect(o.rubberBand.adjusted(0, 0, -1, -1));
    }
    p.setClipping(false);
}

// ---- Headers ----

void TimelinePainter::paintHeaders(QPainter& p, const TimelinePaintInput& in)
{
    const TimelineLayout& L = *in.layout;
    const QSize view = L.viewSize();
    p.setClipRect(QRect(0, kRulerHeight, kHeaderWidth, view.height() - kRulerHeight));
    p.fillRect(QRect(0, kRulerHeight, kHeaderWidth, view.height()), m_window.darker(108));
    p.fillRect(QRect(0, L.dividerY(), kHeaderWidth, kSectionGap), m_line);

    const QFontMetrics smallMetrics(m_smallFont);
    for (int row = 0; row < int(L.rows().size()); ++row) {
        const QRect r = L.rowRect(row);
        if (r.bottom() < kRulerHeight || r.top() > view.height())
            continue;
        const TrackRef ref = L.rows()[size_t(row)].ref;
        const TrackData& track = *in.data->track(ref);
        const bool video = ref.type == TrackType::Video;
        p.fillRect(QRect(0, r.top(), kHeaderWidth, r.height()), m_window);
        p.fillRect(QRect(0, r.top(), 3, r.height()), video ? kVideoStrip : kAudioStrip);
        p.fillRect(QRect(0, r.bottom(), kHeaderWidth, 1), m_line);

        // Destination patch with the short name ("V1"); filled when targeted.
        const QRect patch = L.headerButtonRect(row, HeaderButton::Target);
        const bool targeted = (video ? in.targetVideo : in.targetAudio) == ref.index;
        const QColor patchColor = video ? kVideoStrip : kAudioStrip;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(targeted ? patchColor.lighter(130) : m_line.lighter(220));
        p.setBrush(targeted ? patchColor : m_window.darker(125));
        p.drawRoundedRect(QRectF(patch).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setFont(m_boldFont);
        p.setPen(targeted ? QColor(Qt::white) : m_text);
        const QString shortName
            = (video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(ref.index + 1);
        p.drawText(patch, Qt::AlignCenter, shortName);

        // Buttons from the right.
        int buttonsLeft = kHeaderWidth;
        for (HeaderButton b : TimelineLayout::headerButtons(ref.type)) {
            const QRect br = L.headerButtonRect(row, b);
            buttonsLeft = std::min(buttonsLeft, br.left());
            bool on = false;
            QColor onColor;
            const QPixmap* icon = nullptr;
            switch (b) {
            case HeaderButton::Lock:
                on = track.locked;
                onColor = kLockedOn;
                icon = on ? &m_iconLock : &m_iconUnlock;
                break;
            case HeaderButton::Visible:
                on = !track.enabled;
                onColor = m_window.darker(150);
                icon = track.enabled ? &m_iconVisible : &m_iconHidden;
                break;
            case HeaderButton::Mute:
                on = track.muted;
                onColor = kMuteOn;
                icon = &m_iconMute;
                break;
            case HeaderButton::Solo:
                on = track.solo;
                onColor = kSoloOn;
                icon = &m_iconSolo;
                break;
            case HeaderButton::AutoSelect:
                on = track.autoSelect;
                onColor = video ? kVideoStrip.darker(140) : kAudioStrip.darker(140);
                break;
            default:
                break;
            }
            p.fillRect(br, on ? onColor : m_window.darker(118));
            if (icon && !icon->isNull()) {
                const double dpr = icon->devicePixelRatio();
                const QSizeF s = QSizeF(icon->size()) / dpr;
                p.drawPixmap(QPointF(br.center().x() - s.width() / 2 + 0.5,
                                 br.center().y() - s.height() / 2 + 0.5),
                    *icon);
            }
            if (b == HeaderButton::AutoSelect) {
                // Painted glyph (no icon file): a track bar with a selection tick; bright when
                // the auto track selector is on.
                p.setPen(Qt::NoPen);
                p.setBrush(on ? QColor(Qt::white) : m_text.darker(160));
                p.drawRect(QRect(br.left() + 4, br.top() + 5, br.width() - 8, 3));
                p.drawRect(QRect(br.left() + 4, br.bottom() - 7, br.width() - 8, 3));
                p.setBrush(Qt::NoBrush);
            }
        }

        // Track name, then the clip count on the second line if there is room.
        const int nameLeft = patch.right() + 8;
        const int nameRoom = buttonsLeft - 6 - nameLeft;
        if (nameRoom > 10) {
            p.setFont(m_smallFont);
            p.setPen(m_text);
            const QString name = track.name.isEmpty()
                ? (video ? qtr("Video %1") : qtr("Audio %1")).arg(ref.index + 1)
                : track.name;
            p.drawText(QPointF(nameLeft, patch.center().y() + m_fontAscent / 2.0 - 1),
                smallMetrics.elidedText(name, Qt::ElideRight, nameRoom));
        }
        if (r.height() >= 44) {
            p.setFont(m_smallFont);
            p.setPen(m_textDim);
            p.drawText(QPointF(12, patch.bottom() + 6 + m_fontAscent),
                track.clips.size() == 1 ? qtr("1 Clip") : qtr("%1 Clips").arg(track.clips.size()));
        }
    }
    p.setClipping(false);
    p.fillRect(QRect(kHeaderWidth - 1, kRulerHeight, 1, view.height()), m_line);
}

// ---- Ruler ----

void TimelinePainter::paintRuler(QPainter& p, const TimelinePaintInput& in)
{
    const TimelineLayout& L = *in.layout;
    const TimelineData& d = *in.data;
    const int width = L.viewSize().width();
    const QColor bg = m_window.darker(112);
    p.fillRect(QRect(0, 0, width, kRulerHeight), bg);

    // Top-left corner: playhead timecode.
    p.fillRect(QRect(0, 0, kHeaderWidth, kRulerHeight), m_window.darker(125));
    QFont tcFont = m_boldFont;
    tcFont.setPointSizeF(m_font.pointSizeF() + 2);
    p.setFont(tcFont);
    p.setPen(m_timecode);
    p.drawText(QRect(10, 0, kHeaderWidth - 14, kRulerHeight), Qt::AlignVCenter | Qt::AlignLeft,
        timecode::format(in.playhead, d.fps));

    p.setClipRect(QRect(kHeaderWidth, 0, width - kHeaderWidth, kRulerHeight));
    // In/out shading.
    if (d.markIn >= 0 || d.markOut >= 0) {
        const double x0 = d.markIn >= 0 ? L.frameToX(double(d.markIn)) : kHeaderWidth;
        const double x1 = d.markOut >= 0 ? L.frameToX(double(d.markOut)) : width;
        QColor shade = m_accent;
        shade.setAlpha(70);
        p.fillRect(QRectF(x0, kRulerHeight - 9, x1 - x0, 9), shade);
        p.setPen(m_accent.lighter(130));
        // Bracket-like edges in the lower half, clear of the labels.
        const QColor edge = m_accent.lighter(120);
        if (d.markIn >= 0) {
            p.fillRect(QRectF(x0, kRulerHeight - 14, 2, 14), edge);
            p.fillRect(QRectF(x0, kRulerHeight - 14, 6, 2), edge);
        }
        if (d.markOut >= 0) {
            p.fillRect(QRectF(x1 - 2, kRulerHeight - 14, 2, 14), edge);
            p.fillRect(QRectF(x1 - 6, kRulerHeight - 14, 6, 2), edge);
        }
    }

    const int fpsInt = std::max(1, timecode::framesPerSecond(d.fps));
    const double ppf = L.pixelsPerFrame();
    const int64_t major = majorTickFrames(ppf, fpsInt);
    const int64_t minor = minorTickFrames(major, ppf);
    const auto [f0, f1] = L.visibleFrames();
    m_lines.clear();
    if (minor > 0) {
        for (int64_t f = (f0 / minor) * minor; f <= f1; f += minor) {
            const double x = std::round(L.frameToX(double(f))) + 0.5;
            m_lines.emplace_back(x, kRulerHeight - (f % major == 0 ? 12 : 5), x, kRulerHeight);
        }
    } else {
        for (int64_t f = (f0 / major) * major; f <= f1; f += major) {
            const double x = std::round(L.frameToX(double(f))) + 0.5;
            m_lines.emplace_back(x, kRulerHeight - 12, x, kRulerHeight);
        }
    }
    p.setPen(m_textDim);
    p.drawLines(m_lines.data(), int(m_lines.size()));
    p.setFont(m_smallFont);
    p.setPen(m_text);
    // Start one step early so a label whose tick is just off-screen still shows its text.
    for (int64_t f = std::max<int64_t>(0, (f0 / major - 1) * major); f <= f1; f += major) {
        const double x = std::round(L.frameToX(double(f)));
        p.drawText(QPointF(x + 4, 11), timecode::format(f, d.fps));
    }

    // Markers: small coloured flags on the ruler bottom.
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Marker& m : d.markers) {
        const double x = L.frameToX(double(m.frame));
        if (x < kHeaderWidth - 8 || x > width + 8)
            continue;
        const QPointF pts[5] = {{x - 5, kRulerHeight - 14.0}, {x + 5, kRulerHeight - 14.0},
            {x + 5, kRulerHeight - 6.0}, {x, kRulerHeight - 1.0}, {x - 5, kRulerHeight - 6.0}};
        p.setPen(m.color.darker(160));
        p.setBrush(m.color);
        p.drawConvexPolygon(pts, 5);
    }
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setClipping(false);
    p.fillRect(QRect(0, kRulerHeight - 1, width, 1), m_line);
    p.fillRect(QRect(kHeaderWidth - 1, 0, 1, kRulerHeight), m_line);
}

void TimelinePainter::paintPlayhead(QPainter& p, const TimelinePaintInput& in)
{
    const TimelineLayout& L = *in.layout;
    const double x = std::round(L.frameToX(double(in.playhead))) + 0.5;
    const QSize view = L.viewSize();
    if (x < kHeaderWidth || x > view.width())
        return;
    p.setPen(QPen(kPlayhead, 1));
    p.drawLine(QPointF(x, kRulerHeight - 10), QPointF(x, view.height()));
    // Head: a downward-pointing pentagon in the ruler.
    const QPointF pts[5] = {{x - 6, 2}, {x + 6, 2}, {x + 6, kRulerHeight - 12.0},
        {x, kRulerHeight - 5.0}, {x - 6, kRulerHeight - 12.0}};
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(kPlayhead.darker(140));
    p.setBrush(kPlayhead);
    p.drawConvexPolygon(pts, 5);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, false);
}

} // namespace kk
