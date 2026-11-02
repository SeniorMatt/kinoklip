// SPDX-License-Identifier: GPL-3.0-or-later
// ClipAttributes — how a media file is interpreted (Clip Attributes dialog): pixel aspect,
// data levels, field order / deinterlacing, extra rotation, audio channel mapping and a start
// timecode override. Plus the clip fade helpers shared by the renderers and the mixer.
#pragma once

#include "core/Rational.h"

#include <QString>

#include <cstdint>

namespace kk {

/// Value range of the decoded YUV samples (Resolve's "Data Levels").
enum class DataLevels : quint8 {
    Auto, ///< what the file says (limited unless it is flagged full range)
    Video, ///< limited range: 16 - 235 (8-bit) is black - white
    Full ///< full range: 0 - 255 is black - white
};

/// Field order of interlaced material. Auto = upper field first (the common HD case; the
/// decoder does not report the field order of every format reliably).
enum class FieldOrder : quint8 {
    Auto,
    Progressive, ///< not interlaced: deinterlacing does nothing
    UpperFirst, ///< top field first (TFF)
    LowerFirst ///< bottom field first (BFF, DV / SD)
};

/// How the source channels become the clip's stereo pair (the mixer's internal format).
enum class ChannelFormat : quint8 {
    Auto, ///< the decoder's standard layout (mono to both sides, stereo as is, downmix above)
    Stereo, ///< left output = source channel `left`, right output = source channel `right`
    Mono ///< one source channel (`left`, or kMixAllChannels) on both outputs, centred
};

/// Source channel index meaning "no channel": the output is silent.
inline constexpr int kMutedChannel = -1;
/// Source channel index meaning "all channels mixed" (Mono format only).
inline constexpr int kMixAllChannels = -2;

/// Interpretation of one media pool item. Stored on MediaItem::attributes (the single source
/// of truth, edited by Clip > Clip Attributes for pool items and timeline clips alike) and
/// mirrored into ClipData::attributes of every clip of that item by Project::syncDerived(),
/// so render snapshots carry it without a media pool lookup. Plain value type.
struct ClipAttributes
{
    // ---- Video ----
    /// Pixel aspect ratio (width / height of one pixel). {0, 1} (invalid) = the file's own
    /// sample aspect ratio.
    Rational pixelAspect {0, 1};
    DataLevels levels = DataLevels::Auto;
    FieldOrder fieldOrder = FieldOrder::Auto;
    /// Deinterlace (keep the first field, interpolate the other): Resolve's "Enable
    /// Deinterlacing". Ignored for progressive material.
    bool deinterlace = false;
    /// Extra clockwise rotation applied on top of the file's own (0, 90, 180, 270 degrees).
    int rotation = 0;

    // ---- Audio ----
    ChannelFormat channelFormat = ChannelFormat::Auto;
    int left = 0; ///< source channel of the left output (Mono: the channel), or kMuted...
    int right = 1; ///< source channel of the right output (Stereo), or kMutedChannel

    // ---- Timecode ----
    bool overrideTimecode = false;
    /// Start timecode of the media in frames at the item's own frame rate (timeline rate for
    /// audio-only media); used when overrideTimecode is on.
    int64_t startTimecodeFrames = 0;

    /// True when every attribute has its default (the file is used as is).
    [[nodiscard]] bool isDefault() const { return *this == ClipAttributes {}; }
    /// True when the video geometry or decoding differs from the file (PAR, rotation, levels,
    /// deinterlacing).
    [[nodiscard]] bool affectsVideo() const
    {
        return pixelAspect.isValid() || levels != DataLevels::Auto || rotation != 0
            || (deinterlace && fieldOrder != FieldOrder::Progressive);
    }
    /// True when the audio channel routing differs from the decoder's default.
    [[nodiscard]] bool affectsAudio() const { return channelFormat != ChannelFormat::Auto; }

    friend bool operator==(const ClipAttributes&, const ClipAttributes&) = default;
};

/// Normalises a rotation in degrees to 0, 90, 180 or 270 (nearest quarter turn).
[[nodiscard]] int normalizedRotation(int degrees);

/// Pixel aspect ratio of a picture: @p attributes' override, else @p fileSar (invalid / 0 →
/// square).
[[nodiscard]] Rational effectivePixelAspect(const ClipAttributes& attributes, Rational fileSar);

/// Linear gain matrix of the audio mapping for a source with @p channels channels: fills
/// @p matrix (2 rows of @p channels, row 0 = left output) and returns true, or returns false
/// when the decoder's default layout applies (Auto, or no channels). Mono with
/// kMixAllChannels sums all channels at 1 / channels each (cannot clip).
bool channelMatrix(const ClipAttributes& attributes, int channels, double* matrix);

/// Short identifiers used by project files and the dialog ("auto", "video", "full", ...).
[[nodiscard]] QString dataLevelsToString(DataLevels levels);
[[nodiscard]] DataLevels dataLevelsFromString(const QString& s);
[[nodiscard]] QString fieldOrderToString(FieldOrder order);
[[nodiscard]] FieldOrder fieldOrderFromString(const QString& s);
[[nodiscard]] QString channelFormatToString(ChannelFormat format);
[[nodiscard]] ChannelFormat channelFormatFromString(const QString& s);

// ---- Clip fades (fade handles at the top corners of a clip, spec section 31) ----

/// Video fade factor (0..1, multiplies the clip's opacity) at clip-local frame @p local of a
/// clip @p duration frames long with fade-in @p fadeIn and fade-out @p fadeOut frames:
/// linear, 0 on the clip's first frame and reaching 1 after `fadeIn` frames; mirrored at the
/// end (the last frame is 0). Fades longer than the clip are clamped to it.
[[nodiscard]] double videoFadeFactor(
    int64_t local, int64_t duration, int64_t fadeIn, int64_t fadeOut);

/// Audio fade gain (0..1) at the fractional clip-local frame @p local: the equal-power curve
/// sin(t * pi / 2) over the fade-in (t from 0 at the clip start to 1 after `fadeIn` frames) and
/// cos(...) over the fade-out (1 at its start, 0 at the clip end), like the mixer's
/// crossfades. Fades longer than the clip are clamped to it.
[[nodiscard]] double audioFadeGain(double local, int64_t duration, int64_t fadeIn, int64_t fadeOut);

} // namespace kk
