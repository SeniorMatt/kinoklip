// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePainter implementation. Hot loop: paintRow()/paintClip() for every visible clip.
// Rules for the hot loop: no QString formatting (names come from an elide cache), no
// QPainterPath, no per-clip heap allocations (scratch buffers are members), cheap fillRect for
// everything that can be a rectangle.
#include "ui/timeline/TimelinePainter.h"

#include "core/ClipColors.h"
#include "core/EditOps.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "ui/common/Icons.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/LevelLines.h"
#include "ui/timeline/NestedPreview.h"
#include "ui/timeline/RetimeBar.h"

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
// Every colour comes from the active colour scheme (SchemeColors: clip and track colours from
// the scheme's link / positive / visited roles, states from neutral / negative, lanes and
// ruler from the backgrounds). Fixed exceptions: the user's clip colour labels, flag and
// marker colours (project data) and solid clips' own colour.
const SchemeColors& sc()
{
    return schemeColors();
}

constexpr double kBandHeight = levellines::kNameBand; ///< clip title band (name, fx badge)
constexpr int kMinBandRow = levellines::kMinBandRow; ///< lower rows show only the band
constexpr int kMajorTickSpacingPx = 128; ///< aim for labelled ruler ticks this far apart
constexpr double kFlagMark = 6; ///< px, flag squares in a clip's name band
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
    // Focus colour, not Highlight: in/out ranges, drop targets and rubber bands must stand
    // out, and some schemes (Kinoklip) have a neutral grey selection background.
    m_accent = sc().focus;
    m_lane = sc().lane;
    m_laneAlt = sc().laneAlt;
    m_line = pal.color(QPalette::Dark);
    m_timecode = sc().timecode;
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
        return sc().titleClip;
    case ClipKind::Solid:
        return sc().solidClip;
    case ClipKind::Nested:
        return type == TrackType::Video ? sc().nestedClip : sc().nestedAudioClip;
    case ClipKind::Adjustment:
        return sc().adjustmentClip;
    case ClipKind::Generator:
        return sc().generatorClip;
    case ClipKind::Subtitle:
        return sc().subtitleClip;
    case ClipKind::Media:
        break;
    }
    return type == TrackType::Video ? sc().videoClip : sc().audioClip;
}

QColor TimelinePainter::clipColor(const ClipData& clip, TrackType type, const MediaItem* item)
{
    if (!clip.label.isValid() && item) {
        if (const QColor c = clipColorValue(item->clipColor); c.isValid())
            return c;
    }
    return clipColor(clip, type);
}

const QString& TimelinePainter::clipLabel(const TimelinePaintInput& in, const ClipData& clip)
{
    // A multicam segment shows its active angle (number and name) before the clip name.
    if (clip.kind != ClipKind::Nested || clip.multicamAngle < 0 || !in.timelines)
        return clip.name;
    const TimelineData* mc = (*in.timelines)(clip.nestedUid);
    if (!mc || !mc->multicam)
        return clip.name;
    NameCache& entry = m_names[clip.id];
    if (entry.angle != clip.multicamAngle || entry.multicam != mc || entry.name != clip.name
        || entry.label.isEmpty()) {
        entry.angle = clip.multicamAngle;
        entry.multicam = mc;
        entry.label = QStringLiteral("%1  %2 - %3")
                          .arg(clip.multicamAngle + 1)
                          .arg(multicam::angleName(*mc, clip.multicamAngle), clip.name);
        entry.width = -1; // re-elide
    }
    return entry.label;
}

const QString& TimelinePainter::elidedName(const ClipData& clip, const QString& text, int width)
{
    NameCache& entry = m_names[clip.id];
    if (entry.width != width || entry.name != clip.name) {
        entry.name = clip.name;
        entry.width = width;
        entry.elided = QFontMetrics(m_smallFont).elidedText(text, Qt::ElideRight, width);
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
    // A multicam clip previews its active angle (the angle view's key).
    if (clip.kind == ClipKind::Nested)
        return nestedPreviewPath(clip.multicamAngle >= 0
                ? multicamViewKey(clip.nestedUid, clip.multicamAngle)
                : clip.nestedUid);
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
    const bool off = info.ref.type == TrackType::Audio ? track->muted : !track->enabled;
    if (off || track->locked) {
        p.fillRect(QRect(kHeaderWidth, r.top(), width - kHeaderWidth, r.height()),
            withAlpha(sc().shadow, track->locked ? 70 : 110));
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
    // Clip colour: the clip's own, else the colour of its media pool item (O(1) lookup).
    const MediaItem* poolItem
        = in.mediaPool && !clip.mediaId.isEmpty() ? in.mediaPool->item(clip.mediaId) : nullptr;
    const QColor base = clipColor(clip, track.type, poolItem);

    // Sub-pixel / tiny clips (zoomed far out): one rectangle, nothing else.
    if (r.width() < 4) {
        p.fillRect(QRectF(r.left(), r.top(), std::max(1.0, r.width()), r.height()),
            selected ? sc().selection : (clip.enabled ? base.lighter(115) : base.darker(200)));
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
                paintVolumeLine(p, in, clip, r, content);
            }
            break;
        case ClipKind::Solid:
            p.fillRect(content.adjusted(0, 2, 0, -2),
                clip.solidColor.isValid() ? clip.solidColor : sc().solidClip);
            break;
        case ClipKind::Adjustment: {
            // Diagonal hatching: the clip shows no picture, it processes what is below.
            p.save();
            p.setClipRect(content, Qt::IntersectClip);
            p.setPen(QPen(base.lighter(135), 1));
            m_lines.clear();
            const double h = content.height();
            const double first = std::floor((content.left() - h - r.left()) / 9.0) * 9.0 + r.left();
            for (double x = first; x < content.right(); x += 9)
                m_lines.emplace_back(QPointF(x, content.bottom()), QPointF(x + h, content.top()));
            p.setRenderHint(QPainter::Antialiasing, true);
            for (const QLineF& line : m_lines)
                p.drawLine(line);
            p.restore();
            break;
        }
        case ClipKind::Generator:
            // A muted swatch strip reads as "generated picture" next to media thumbnails.
            p.fillRect(content.adjusted(0, 2, 0, -2), base.darker(120));
            break;
        case ClipKind::Subtitle:
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

    // Flags of the media pool item: small squares at the right end of the name band.
    const quint32 flags = poolItem ? poolItem->flags : 0;
    const int flagMarks = flags != 0 && bandH >= 10 ? flagCount(flags) : 0;
    const double flagsW = flagMarks > 0 ? flagMarks * (kFlagMark + 2) + 2 : 0;
    // Name (sticky: stays readable when the clip start is scrolled off), duration, fx badge.
    const bool hasFx = !clip.effects.isEmpty();
    const double badgeW = (hasFx ? 18 : 0) + flagsW;
    const int nameRoom = int(visible.width() - 10 - badgeW);
    if (nameRoom > 12 && bandH >= 12) {
        p.setFont(m_smallFont);
        p.setPen(clip.enabled ? sc().textOn(base) : m_textDim);
        const QString& label = clipLabel(in, clip);
        const QString& name = elidedName(clip, label, nameRoom);
        const double baseline = r.top() + (bandH + m_fontAscent) / 2.0 - 1;
        p.drawText(QPointF(visLeft + 5, baseline), name);
        // Duration only when the full name fits with room to spare (duration-aware text).
        if (name == label && nameRoom > 150) {
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
    if (flagMarks > 0 && visible.width() > flagsW + 24) {
        const double right = std::min(r.right(), viewRight) - (hasFx ? 20 : 0) - 3;
        double x = right - kFlagMark;
        const double y = r.top() + (bandH - kFlagMark) / 2.0;
        for (int i = int(flagPalette().size()) - 1; i >= 0; --i) {
            if (!(flags & (1u << i)))
                continue;
            p.fillRect(QRectF(x - 0.5, y - 0.5, kFlagMark + 1, kFlagMark + 1), base.darker(240));
            p.fillRect(QRectF(x, y, kFlagMark, kFlagMark), flagColor(i));
            x -= kFlagMark + 2;
        }
    }
    if (const QRectF badge = hasFx ? fxBadgeRect(r, viewRight) : QRectF(); !badge.isEmpty()) {
        p.fillRect(badge, base.darker(190));
        p.setPen(sc().keyframe);
        p.setFont(m_smallFont);
        p.drawText(badge, Qt::AlignCenter, QStringLiteral("fx"));
    }

    paintFades(p, in, clip, r, visible);
    if (r.height() >= 24)
        paintKeyframes(p, in, clip, r);
    if (in.retimeShown && in.retimeShown->contains(clip.id))
        paintRetime(p, in, clip, r, visible);

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
        p.fillRect(visible, withAlpha(sc().shadow, 150));
    if (selected) {
        p.setPen(QPen(sc().selection, 2));
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

namespace {
// Speed bar field colours by kind of segment (SchemeColors speed* roles).
QColor speedColor(const retime::Segment& s)
{
    if (s.isFreeze())
        return sc().speedFreeze;
    if (s.isReverse())
        return sc().speedReverse;
    if (std::abs(s.speed - 1.0) < 1e-6)
        return sc().speedNormal;
    return s.speed < 1.0 ? sc().speedSlow : sc().speedFast;
}
} // namespace

void TimelinePainter::paintRetime(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
    const QRectF& rect, const QRectF& visible)
{
    const retimebar::Geometry g = retimebar::geometry(clip, rect, *in.layout);
    if (g.isEmpty())
        return;
    p.save();
    p.setClipRect(visible, Qt::IntersectClip);
    p.setFont(m_smallFont);
    const QFontMetrics fm(m_smallFont);
    for (const retimebar::SegmentField& f : g.fields) {
        p.fillRect(f.rect, speedColor(f.segment));
        p.fillRect(
            QRectF(f.rect.left(), f.rect.bottom() - 1, f.rect.width(), 1), withAlpha(sc().shadow, 120));
        // Speed text (sticky inside the visible part of the field) and the menu arrow.
        const QRectF shown = f.rect.intersected(visible);
        const QString text
            = f.segment.isFreeze() ? qtr("Freeze 0%") : retime::formatPercent(f.segment.speed);
        if (shown.width() > fm.horizontalAdvance(text) + retimebar::kArrowWidth + 12) {
            p.setPen(sc().textOn(speedColor(f.segment)));
            // Clear of a speed point's handle at the field start.
            const double inset = f.segment.startKey >= 0 && f.segment.startLocal > 0
                ? retimebar::kHandleHalfWidth + 3
                : 4;
            p.drawText(shown.adjusted(inset, 0, -retimebar::kArrowWidth - 2, 0),
                Qt::AlignVCenter | Qt::AlignLeft, text);
        }
        if (f.arrow.width() >= 6) {
            const QPointF c = f.arrow.center();
            const QPointF tri[3]
                = {{c.x() - 3, c.y() - 1.5}, {c.x() + 3, c.y() - 1.5}, {c.x(), c.y() + 2.5}};
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(sc().textOn(speedColor(f.segment)));
            p.drawConvexPolygon(tri, 3);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }
    // Speed points: a line through the clip, the slide handle in the bar, the retime handle
    // at the bottom; ramped points get a round slide handle.
    const AnimatedValue* curve = retime::curve(clip);
    for (const retimebar::PointMarks& m : g.points) {
        p.fillRect(QRectF(m.x - 0.5, g.bar.bottom(), 1, rect.bottom() - g.bar.bottom()),
            withAlpha(sc().snap, 170));
        bool ramp = false;
        if (curve) {
            const qsizetype i = curve->indexOf(m.localFrame);
            ramp = i >= 0 && keyInterp(curve->keys[i]) != Interp::Linear;
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(withAlpha(sc().shadow, 180), 1));
        p.setBrush(sc().snap);
        const QRectF top = m.top.adjusted(1, 2, -1, -2);
        if (ramp)
            p.drawEllipse(top);
        else
            p.drawRoundedRect(top, 1.5, 1.5);
        const QRectF b = m.bottom;
        const QPointF tri[3]
            = {{b.left() + 1, b.bottom()}, {b.right() - 1, b.bottom()}, {b.center().x(), b.top()}};
        p.drawConvexPolygon(tri, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    // Retime handle of the clip end.
    p.fillRect(g.endHandle.adjusted(1, 2, -1, -2), sc().snap);
    p.restore();
}

namespace {
/// Columns per waveform request on retimed clips (the curve is treated as straight inside).
constexpr int kRetimeChunkPx = 16;
/// Below this speed retimed audio is muted (AudioMixer's kMinAudibleSpeed).
constexpr double kMinAudibleSpeed = 0.02;
} // namespace

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
        if ((item || clip.kind == ClipKind::Nested) && !retime::isRetimed(clip)) {
            const double usPerFrame = fps.isValid() ? 1e6 * fps.den / fps.num : 0;
            const double startFrame = L.xToFrame(visible.left()) - double(clip.start);
            const double startUs = double(clip.sourceInUs) + startFrame * usPerFrame;
            m_peaks.resize(size_t(columns)); // capacity is kept between paints
            ok = m_waveforms(clipPreviewPath(clip, item), startUs, usPerFrame / L.pixelsPerFrame(),
                columns, m_peaks.data());
        } else if (item || clip.kind == ClipKind::Nested) {
            // Retimed: the waveform follows the retime curve in chunks of kRetimeChunkPx
            // columns (each a straight piece of the curve); freeze and reverse pieces are drawn
            // silent, as the mixer plays them.
            m_peaks.assign(size_t(columns), 0.0f);
            const QString path = clipPreviewPath(clip, item);
            ok = true;
            for (int c0 = 0; c0 < columns && ok; c0 += kRetimeChunkPx) {
                const int n = std::min(kRetimeChunkPx, columns - c0);
                const double f0 = L.xToFrame(visible.left() + c0) - double(clip.start);
                const double f1 = L.xToFrame(visible.left() + c0 + n) - double(clip.start);
                const double us0 = double(retime::sourceTimeUs(clip, f0, fps));
                const double us1 = double(retime::sourceTimeUs(clip, f1, fps));
                const double perColumn = (us1 - us0) / n;
                const double speed = perColumn * L.pixelsPerFrame() / retime::frameUs(fps);
                if (speed < kMinAudibleSpeed)
                    continue; // muted
                ok = m_waveforms(path, us0, perColumn, n, m_peaks.data() + c0);
            }
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
    const ClipData& clip, const QRectF& clipRect, const QRectF& visible)
{
    namespace ll = levellines;
    const QRectF area = ll::levelArea(clipRect);
    if (area.isEmpty())
        return;
    const TimelineLayout& L = *in.layout;
    const auto it = clip.audio.values.constFind(keys::VolumeDb);
    const bool animated = it != clip.audio.values.cend() && !it->keys.isEmpty();
    const double clipStart = double(clip.start);
    m_lines.clear();
    if (!animated) {
        const double db = it == clip.audio.values.cend() ? 0.0 : it->base;
        const double y = ll::yForDb(area, db);
        m_lines.emplace_back(visible.left(), y, visible.right(), y);
    } else {
        // The exact curve (eases included), sampled every kStepPx over the visible part, so
        // what is drawn is what the hit test and the mixer use.
        constexpr double kStepPx = 2.0;
        double prevX = visible.left();
        double prevY = ll::yForDb(area, it->valueAtTime(L.xToFrame(prevX) - clipStart));
        for (double x = prevX + kStepPx; prevX < visible.right(); x += kStepPx) {
            x = std::min(x, visible.right());
            const double y = ll::yForDb(area, it->valueAtTime(L.xToFrame(x) - clipStart));
            m_lines.emplace_back(prevX, prevY, x, y);
            prevX = x;
            prevY = y;
        }
    }
    p.save();
    p.setClipRect(visible, Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(sc().levelLine, 1.3));
    p.drawLines(m_lines.data(), int(m_lines.size()));
    if (animated) {
        // Keys on the line: diamonds, selected ones filled with the selection colour. The
        // path string is built once (no allocation per key and paint).
        static const QString kVolumePath = levelops::volumePath();
        const bool anySelected = in.selectedKeys && !in.selectedKeys->isEmpty();
        for (const Keyframe& k : it->keys) {
            if (k.frame < 0 || k.frame >= clip.duration)
                continue;
            const double x = L.frameToX(clipStart + double(k.frame));
            if (x < visible.left() - ll::kKeyRadius || x > visible.right() + ll::kKeyRadius)
                continue;
            const bool sel = anySelected
                && in.selectedKeys->contains(KeyframeRef {clip.id, kVolumePath, k.frame});
            p.setPen(sc().shadow);
            p.setBrush(sel ? sc().selection : sc().levelLine);
            drawDiamond(p, QPointF(x, ll::yForDb(area, k.value)), ll::kKeyRadius + 0.5);
        }
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
}

void TimelinePainter::paintFades(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
    const QRectF& r, const QRectF& visible)
{
    namespace ll = levellines;
    const TimelineLayout& L = *in.layout;
    const double ppf = L.pixelsPerFrame();
    const bool hovered = in.hoverClip == clip.id;
    if (clip.fadeInFrames <= 0 && clip.fadeOutFrames <= 0 && !hovered)
        return;
    p.save();
    p.setClipRect(visible, Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, true);
    // The faded part: a shaded wedge from the corner to where the fade reaches full level,
    // its edge the fade curve (straight: video fades linearly; close enough for audio).
    const double top = r.top() + 1;
    const double bottom = r.bottom() - 1;
    const auto wedge = [&](double cornerX, double endX) {
        const QPointF pts[3] = {{cornerX, top}, {endX, top}, {cornerX, bottom}};
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(sc().shadow, 90));
        p.drawConvexPolygon(pts, 3);
        p.setPen(QPen(withAlpha(sc().bright, 200), 1.2));
        p.drawLine(QPointF(cornerX, bottom), QPointF(endX, top));
    };
    if (clip.fadeInFrames > 0)
        wedge(r.left(), r.left() + double(clip.fadeInFrames) * ppf);
    if (clip.fadeOutFrames > 0)
        wedge(r.right(), r.right() - double(clip.fadeOutFrames) * ppf);
    if (hovered || clip.fadeInFrames > 0 || clip.fadeOutFrames > 0) {
        for (const bool head : {true, false}) {
            const QRectF h = ll::fadeHandleRect(clip, r, L, head);
            if (h.isEmpty() || (!hovered && (head ? clip.fadeInFrames : clip.fadeOutFrames) == 0))
                continue;
            p.setPen(sc().shadow);
            p.setBrush(sc().bright);
            p.drawRect(h.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    p.restore();
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
                    p.setPen(sc().shadow);
                    p.setBrush(sc().keyframe);
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
    p.fillRect(r, withAlpha(mixColors(sc().transition, sc().bright, 0.6), 150));
    p.fillRect(QRectF(r.left(), r.top(), r.width(), 1), withAlpha(sc().bright, 200));
    p.fillRect(QRectF(r.left(), r.top(), 1, r.height()), withAlpha(sc().bright, 160));
    p.fillRect(QRectF(r.left(), r.bottom() - 1, r.width(), 1), withAlpha(sc().shadow, 160));
    p.fillRect(QRectF(r.right() - 1, r.top(), 1, r.height()), withAlpha(sc().shadow, 160));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(withAlpha(sc().shadow, 200), 1));
    if (in)
        p.drawLine(r.bottomLeft() + QPointF(1, -1), r.topRight() + QPointF(-1, 1));
    else
        p.drawLine(r.topLeft() + QPointF(1, 1), r.bottomRight() + QPointF(-1, -1));
    p.setRenderHint(QPainter::Antialiasing, false);
    if (selected) {
        // Same colour as a selected clip's outline.
        p.setPen(QPen(sc().selection, 2));
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
            p.setBrush(withAlpha(m_accent, 40));
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
        p.fillRect(r, withAlpha(m_accent, 55));
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
        p.fillRect(QRectF(x, cr.top(), 3, cr.height()), sc().snap);
    }
    if (o.bladeFrame && o.bladeRow >= 0) {
        const QRect rr = L.rowRect(o.bladeRow);
        const double x = std::round(L.frameToX(double(*o.bladeFrame))) + 0.5;
        p.setPen(QPen(sc().playhead, 1, Qt::DashLine));
        p.drawLine(QPointF(x, rr.top()), QPointF(x, rr.bottom()));
    }
    if (o.snapFrame) {
        const double x = std::round(L.frameToX(double(*o.snapFrame))) + 0.5;
        p.setPen(QPen(sc().snap, 1));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, view.height()));
    }
    if (!o.rubberBand.isEmpty()) {
        p.fillRect(o.rubberBand, withAlpha(m_accent, 40));
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
        const bool subtitle = ref.type == TrackType::Subtitle;
        const QColor strip = video ? sc().videoTrack : subtitle ? sc().subtitleTrack : sc().audioTrack;
        p.fillRect(QRect(0, r.top(), kHeaderWidth, r.height()), m_window);
        p.fillRect(QRect(0, r.top(), 3, r.height()), strip);
        p.fillRect(QRect(0, r.bottom(), kHeaderWidth, 1), m_line);

        // Destination patch with the short name ("V1"); filled when targeted.
        const QRect patch = L.headerButtonRect(row, HeaderButton::Target);
        // Subtitle tracks have no source patch: their label is never "targeted".
        const bool targeted = !subtitle && (video ? in.targetVideo : in.targetAudio) == ref.index;
        const QColor patchColor = strip;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(targeted ? patchColor.lighter(130) : m_line.lighter(220));
        p.setBrush(targeted ? patchColor : m_window.darker(125));
        p.drawRoundedRect(QRectF(patch).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setFont(m_boldFont);
        p.setPen(targeted ? sc().textOn(patchColor) : m_text);
        const QString shortName = (video             ? QStringLiteral("V")
                                          : subtitle ? QStringLiteral("ST")
                                                     : QStringLiteral("A"))
            + QString::number(ref.index + 1);
        p.drawText(patch, Qt::AlignCenter, shortName);

        // Buttons from the right.
        int buttonsLeft = kHeaderWidth;
        for (HeaderButton b : L.buttonsFor(ref.type)) {
            const QRect br = L.headerButtonRect(row, b);
            buttonsLeft = std::min(buttonsLeft, br.left());
            bool on = false;
            QColor onColor;
            const QPixmap* icon = nullptr;
            switch (b) {
            case HeaderButton::Lock:
                on = track.locked;
                onColor = sc().locked;
                icon = on ? &m_iconLock : &m_iconUnlock;
                break;
            case HeaderButton::Visible:
                on = !track.enabled;
                onColor = m_window.darker(150);
                icon = track.enabled ? &m_iconVisible : &m_iconHidden;
                break;
            case HeaderButton::Mute:
                on = track.muted;
                onColor = sc().muted;
                icon = &m_iconMute;
                break;
            case HeaderButton::Solo:
                on = track.solo;
                onColor = sc().soloed;
                icon = &m_iconSolo;
                break;
            case HeaderButton::AutoSelect:
                on = track.autoSelect;
                onColor = strip.darker(140);
                break;
            case HeaderButton::Arm:
                break; // drawn disabled below (recording is deferred)
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
            if (b == HeaderButton::Arm) {
                p.setFont(m_boldFont);
                p.setPen(sc().textDisabled);
                p.drawText(br, Qt::AlignCenter, qtr("R"));
            }
            if (b == HeaderButton::AutoSelect) {
                // Painted glyph (no icon file): a track bar with a selection tick; bright when
                // the auto track selector is on.
                p.setPen(Qt::NoPen);
                p.setBrush(on ? sc().textOn(onColor) : m_text.darker(160));
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
            const QString name = track.name.isEmpty() ? (video             ? qtr("Video %1")
                                                                : subtitle ? qtr("Subtitle %1")
                                                                           : qtr("Audio %1"))
                                                            .arg(ref.index + 1)
                                                      : track.name;
            p.drawText(QPointF(nameLeft, patch.center().y() + m_fontAscent / 2.0 - 1),
                smallMetrics.elidedText(name, Qt::ElideRight, nameRoom));
        }
        const bool audioPage = L.audioPageHeaders() && ref.type == TrackType::Audio;
        if (r.height() >= 44) {
            p.setFont(m_smallFont);
            p.setPen(m_textDim);
            // Audio page: the channel format instead of the clip count.
            const QString second = audioPage ? (track.mix.mono ? qtr("Mono") : qtr("Stereo"))
                : track.clips.size() == 1    ? qtr("1 Clip")
                                             : qtr("%1 Clips").arg(track.clips.size());
            p.drawText(QPointF(12, patch.bottom() + 6 + m_fontAscent), second);
        }
        if (audioPage && in.headerMeter) {
            // Level meter at the header's right edge (scheme zone colours, like the mixer).
            const QRect m = L.headerMeterRect(row);
            const auto [leftDb, rightDb] = (*in.headerMeter)(track.id);
            const int bars = track.mix.mono ? 1 : 2;
            const int w = bars == 1 ? m.width() : (m.width() - 1) / 2;
            const float levels[2] = {leftDb, rightDb};
            for (int b = 0; b < bars; ++b) {
                const QRect bar(m.left() + b * (w + 1), m.top(), w, m.height());
                p.fillRect(bar, m_window.darker(150));
                const double t = std::clamp((double(levels[b]) + 60.0) / 60.0, 0.0, 1.0);
                const int h = int(t * bar.height());
                if (h > 0) {
                    const QColor c = levels[b] > -6.0f ? sc().negative
                        : levels[b] > -18.0f           ? sc().neutral
                                                       : sc().positive;
                    p.fillRect(QRect(bar.left(), bar.bottom() - h + 1, bar.width(), h), c);
                }
            }
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
    const QColor bg = sc().ruler;
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

    // Render cache status: a thin bar along the top of the ruler, red where a section needs
    // caching, blue where it is cached (Resolve's render cache indicator).
    if (in.cache && !in.cache->isEmpty()) {
        constexpr int kCacheBarHeight = 3;
        for (const CacheBarRange& r : *in.cache) {
            if (r.end <= f0 || r.start > f1)
                continue;
            const double x0 = L.frameToX(double(r.start));
            const double x1 = L.frameToX(double(r.end));
            p.fillRect(QRectF(x0, 0, std::max(1.0, x1 - x0), kCacheBarHeight),
                r.cached ? sc().cached : sc().uncached);
        }
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
    p.setPen(QPen(sc().playhead, 1));
    p.drawLine(QPointF(x, kRulerHeight - 10), QPointF(x, view.height()));
    // Head: a downward-pointing pentagon in the ruler.
    const QPointF pts[5] = {{x - 6, 2}, {x + 6, 2}, {x + 6, kRulerHeight - 12.0},
        {x, kRulerHeight - 5.0}, {x - 6, kRulerHeight - 12.0}};
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(sc().playhead.darker(140));
    p.setBrush(sc().playhead);
    p.drawConvexPolygon(pts, 5);
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, false);
}

} // namespace kk
