// SPDX-License-Identifier: GPL-3.0-or-later
// SchemeColors implementation: scheme roles + application palette -> editor colours.
#include "ui/common/SchemeColors.h"

#include <QApplication>
#include <QEvent>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

// How far clip fills move from their role colour towards the view background: enough to
// keep white or black clip names readable, little enough that the hue stays obvious.
constexpr double kClipMixDark = 0.40;
constexpr double kClipMixLight = 0.12;
constexpr double kBadgeMix = 0.12; ///< track badges: closer to the role colour than clips
/// Hues used only when a palette has no scheme roles at all (tests, foreign palettes):
/// positive = green, neutral = orange, in degrees.
constexpr int kPositiveHue = 145;
constexpr int kNeutralHue = 28;

struct Store
{
    SchemeRoles roles;
    SchemeColors colors;
    bool dirty = true;
    bool watching = false;
    quint64 generation = 0;
};

Store& store()
{
    static Store s;
    return s;
}

double luminance(const QColor& c)
{
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor or_(const QColor& c, const QColor& fallback)
{
    return c.isValid() ? c : fallback;
}

/// A colour with the saturation/lightness of @p like and hue @p hue (fallback roles only).
QColor hueLike(const QColor& like, int hue)
{
    QColor hsl = like.toHsl();
    return QColor::fromHsl(hue, std::max(hsl.hslSaturation(), 120), hsl.lightness());
}

SchemeColors compute(const SchemeRoles& r, const QPalette& p, quint64 generation)
{
    SchemeColors c;
    c.generation = generation;
    c.window = p.color(QPalette::Window);
    c.view = p.color(QPalette::Base);
    c.viewAlt = p.color(QPalette::AlternateBase);
    c.button = p.color(QPalette::Button);
    c.text = p.color(QPalette::WindowText);
    c.textDim = or_(r.inactive, p.color(QPalette::PlaceholderText));
    c.textDisabled = p.color(QPalette::Disabled, QPalette::WindowText);
    c.highlight = p.color(QPalette::Highlight);
    c.highlightText = p.color(QPalette::HighlightedText);
    c.line = p.color(QPalette::Dark);
    c.shadow = p.color(QPalette::Shadow);
    c.dark = c.window.lightness() < 128;
    c.bright = c.dark ? c.text : c.view;
    c.windowAlt = mixColors(c.window, c.text, 0.05);
    c.header = or_(r.header, c.dark ? c.window.lighter(115) : c.window.darker(103));
    // A header colour from another scheme than the palette is never right.
    if ((c.header.lightness() < 128) != c.dark)
        c.header = c.dark ? c.window.lighter(115) : c.window.darker(103);

    c.link = or_(r.link, p.color(QPalette::Link));
    c.visited = or_(r.visited, p.color(QPalette::LinkVisited));
    c.focus = or_(r.focus, c.highlight);
    c.hover = or_(r.hover, c.focus);
    c.active = or_(r.active, c.focus);
    c.positive = or_(r.positive, hueLike(c.link, kPositiveHue));
    c.neutral = or_(r.neutral, hueLike(c.link, kNeutralHue));
    c.negative = or_(r.negative, p.color(QPalette::BrightText));
    c.complementary = or_(r.complementary, c.dark ? c.shadow : mixColors(c.text, c.shadow, 0.3));
    c.complementaryText = or_(r.complementaryText, c.dark ? c.text : c.view);

    // Clips: role colour pulled towards the view background.
    const double clipMix = c.dark ? kClipMixDark : kClipMixLight;
    const auto fill = [&](const QColor& role) { return mixColors(role, c.view, clipMix); };
    const QColor yellow = mixColors(c.neutral, c.positive, 0.4);
    c.videoClip = fill(c.link);
    c.audioClip = fill(c.positive);
    c.titleClip = fill(c.visited);
    c.solidClip = fill(c.textDim);
    c.nestedClip = fill(mixColors(c.link, c.positive, 0.5));
    c.nestedAudioClip = fill(mixColors(c.positive, c.link, 0.25));
    c.adjustmentClip = fill(mixColors(c.link, c.textDim, 0.6));
    c.generatorClip = fill(mixColors(c.visited, c.neutral, 0.55));
    c.subtitleClip = fill(mixColors(c.neutral, c.textDim, 0.5));

    c.videoTrack = mixColors(c.link, c.view, kBadgeMix);
    c.audioTrack = mixColors(c.positive, c.view, kBadgeMix);
    c.subtitleTrack = mixColors(mixColors(c.neutral, c.textDim, 0.35), c.view, kBadgeMix);

    c.selection = c.neutral;
    c.transition = c.focus;
    c.playhead = c.negative;
    c.timecode = mixColors(c.negative, c.neutral, 0.45);
    c.snap = yellow;
    c.locked = c.negative;
    c.muted = c.neutral;
    c.soloed = yellow;
    c.keyframe = mixColors(yellow, c.bright, 0.55);
    c.keyframeSelected = c.selection;
    c.levelLine = withAlpha(mixColors(yellow, c.bright, 0.35), 220);

    // Lanes: the view background nudged towards the text colour, so alternate rows stay
    // distinguishable in light schemes too (lighter() does nothing on white).
    c.lane = mixColors(c.view, c.text, 0.05);
    c.laneAlt = mixColors(c.view, c.text, 0.08);
    c.ruler = c.dark ? c.window.darker(112) : c.window.darker(104);
    c.grid = mixColors(c.view, c.text, 0.18);

    c.cached = c.link;
    c.uncached = c.negative;
    c.overlay = withAlpha(c.complementary, 160);
    c.overlayText = c.complementaryText;
    // Viewer surround: the complementary set (KDE's "dark area" colours, dark in light schemes
    // too), darkened so the picture edge stays obvious.
    c.viewerSurround = c.complementary.darker(200);
    c.viewerText = mixColors(c.complementaryText, c.viewerSurround, 0.35);

    const QColor speedBase = mixColors(c.view, c.text, 0.18);
    const auto speed = [&](const QColor& role) { return mixColors(role, speedBase, 0.55); };
    c.speedNormal = speedBase;
    c.speedSlow = speed(c.neutral);
    c.speedFast = speed(mixColors(c.positive, c.link, 0.5));
    c.speedFreeze = speed(c.negative);
    c.speedReverse = speed(c.visited);
    return c;
}

} // namespace

QColor mixColors(const QColor& a, const QColor& b, double amount)
{
    const auto lerp = [amount](int x, int y) { return int(x + (y - x) * amount + 0.5); };
    return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()),
        a.alpha());
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

QColor SchemeColors::textOn(const QColor& fill) const
{
    QColor best = text;
    for (const QColor& candidate : {view, bright, shadow, highlightText}) {
        if (contrast(candidate, fill) > contrast(best, fill) + 0.01)
            best = candidate;
    }
    return best;
}

int SchemeColors::seriesCount()
{
    return 8;
}

QColor SchemeColors::series(int index) const
{
    const int i = ((index % seriesCount()) + seriesCount()) % seriesCount();
    switch (i) {
    case 0:
        return negative;
    case 1:
        return positive;
    case 2:
        return link;
    case 3:
        return mixColors(neutral, positive, 0.4);
    case 4:
        return visited;
    case 5:
        return mixColors(link, positive, 0.5);
    case 6:
        return neutral;
    default:
        return textDim;
    }
}

const SchemeColors& schemeColors()
{
    Store& s = store();
    if (!s.watching && qApp) {
        // Any application palette change (scheme switch, style switch, tests) marks the
        // colours stale; painting code then pays one bool test per call.
        qApp->installEventFilter(SchemeColorsNotifier::instance());
        s.watching = true;
    }
    if (s.dirty) {
        s.dirty = false;
        s.colors = compute(s.roles, QApplication::palette(), ++s.generation);
    }
    return s.colors;
}

void setSchemeRoles(const SchemeRoles& roles)
{
    Store& s = store();
    s.roles = roles;
    s.dirty = true;
    (void)schemeColors();
    emit SchemeColorsNotifier::instance()->changed();
}

SchemeColorsNotifier* SchemeColorsNotifier::instance()
{
    static SchemeColorsNotifier* notifier = new SchemeColorsNotifier;
    return notifier;
}

bool SchemeColorsNotifier::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ApplicationPaletteChange && watched == qApp)
        store().dirty = true;
    return QObject::eventFilter(watched, event);
}

} // namespace kk
