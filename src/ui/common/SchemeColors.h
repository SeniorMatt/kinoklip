// SPDX-License-Identifier: GPL-3.0-or-later
// SchemeColors — every colour we paint ourselves, derived from the active colour scheme.
#pragma once

#include <QColor>
#include <QObject>

namespace kk {

/// The KDE colour roles of the active scheme that QPalette does not carry (View set unless
/// noted). Filled by kk::theme (KColorScheme with KF6, the .colors file without); every colour
/// left invalid is derived from the application palette instead.
struct SchemeRoles
{
    QColor link; ///< ForegroundLink
    QColor visited; ///< ForegroundVisited
    QColor active; ///< ForegroundActive
    QColor inactive; ///< ForegroundInactive
    QColor positive; ///< ForegroundPositive
    QColor neutral; ///< ForegroundNeutral
    QColor negative; ///< ForegroundNegative
    QColor focus; ///< DecorationFocus
    QColor hover; ///< DecorationHover
    QColor header; ///< Header set BackgroundNormal
    QColor complementary; ///< Complementary set BackgroundNormal (dark in light schemes too)
    QColor complementaryText; ///< Complementary set ForegroundNormal
};

/// Semantic colours for custom-painted widgets (timeline, keyframe editors, rulers, viewer
/// overlays, media pool tiles, render range bar). Nothing in src/ui hard-codes a colour any
/// more: it asks schemeColors() for a role. Derived shades use mix()/darker()/lighter() of
/// these roles, never new literals.
///
/// Mapping (scheme role -> editor role):
///  - video clips, video track badges, cache "done": link
///  - audio clips, waveforms, audio badges: positive
///  - titles, generators, subtitles: visited (mixed with neutral for the latter two)
///  - selection outlines, selected keyframes, warnings: neutral
///  - transitions, focus: focus decoration
///  - playhead, in/out marks, locks, offline: negative
///  - snap line, solo, level lines, keyframes: neutral mixed with positive (a warm yellow)
///  - lanes, rulers, grids, panel backgrounds: window / view / alternate backgrounds
///  - text: the foreground roles; textOn() picks the readable one for a fill
///  - plates drawn over video (multicam, media pool badges): the complementary set
///
/// Exceptions that keep fixed colours (documented where they are used): the user's clip
/// colour labels and flags (named colours stored in the project), rendered video content
/// (black letterbox behind a frame, thumbnails), and the white-on-black-halo mask outlines
/// drawn over video for visibility.
struct SchemeColors
{
    // ---- scheme roles ------------------------------------------------------------------
    QColor window, windowAlt, view, viewAlt, button, header;
    QColor text, textDim, textDisabled, highlight, highlightText;
    QColor line; ///< separators, frames (QPalette::Dark)
    QColor shadow; ///< darkest shade of the scheme (key outlines, dimming)
    QColor bright; ///< the lighter of text and view background (light marks on clip bodies)
    QColor link, visited, active, positive, neutral, negative, focus, hover;
    QColor complementary, complementaryText;

    // ---- editor roles ------------------------------------------------------------------
    QColor videoClip, audioClip, titleClip, solidClip, nestedClip, nestedAudioClip;
    QColor adjustmentClip, generatorClip, subtitleClip;
    QColor videoTrack, audioTrack, subtitleTrack; ///< track badges / header strips
    QColor selection; ///< selected clip / keyframe outline
    QColor transition;
    QColor playhead; ///< also in/out marks and the render range edges
    QColor timecode; ///< playhead timecode in the ruler
    QColor snap, locked, muted, soloed;
    QColor keyframe, keyframeSelected, levelLine;
    QColor lane, laneAlt, ruler, grid;
    QColor cached, uncached; ///< render cache bar
    QColor overlay, overlayText; ///< plates and text drawn over video
    QColor viewerSurround, viewerText; ///< around the picture (dark in every scheme)
    QColor speedNormal, speedSlow, speedFast, speedFreeze, speedReverse;

    bool dark = true;
    quint64 generation = 0; ///< increases whenever the colours change

    /// Text colour readable on @p fill: text, view background or the bright/shadow end of
    /// the scheme, whichever contrasts most.
    [[nodiscard]] QColor textOn(const QColor& fill) const;
    /// Distinct colour @p index (wraps) for curves and category tints, from the roles above.
    [[nodiscard]] QColor series(int index) const;
    /// Number of distinct series() colours.
    [[nodiscard]] static int seriesCount();
};

/// The colours of the active scheme. Recomputed lazily when the application palette changes
/// (and by setSchemeRoles()). GUI thread only.
[[nodiscard]] const SchemeColors& schemeColors();

/// Stores the roles of the newly applied scheme and recomputes. Called by kk::theme after
/// every scheme or style switch; emits SchemeColorsNotifier::changed().
void setSchemeRoles(const SchemeRoles& roles);

/// Blend of @p a towards @p b (0 = a, 1 = b), alpha of @p a.
[[nodiscard]] QColor mixColors(const QColor& a, const QColor& b, double amount);
/// @p c with alpha @p alpha (0..255).
[[nodiscard]] QColor withAlpha(QColor c, int alpha);

/// Emits changed() after a colour scheme switch, once the new colours are in place. Widgets
/// that cache colours or pre-rendered pixmaps connect to it (QEvent::PaletteChange arrives
/// as well, but may come before kk::theme has stored the new scheme roles).
class SchemeColorsNotifier : public QObject
{
    Q_OBJECT
public:
    static SchemeColorsNotifier* instance();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void changed();
};

} // namespace kk
