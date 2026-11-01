// SPDX-License-Identifier: GPL-3.0-or-later
// MediaInfo — what we know about a media file (streams, size, duration) + media::probe().
// Shared contract between the media engine and the rest of the app (plan.md §5.3).
#pragma once
#include "core/Rational.h"

#include <QString>

#include <cstdint>

namespace kk::media {

/// Technical description of one media file, as returned by probe() and VideoDecoder::info().
///
/// Time convention: "media time" is measured in microseconds from the start of the file
/// (time 0 = first frame / first sample; see `startTimeUs`). All media APIs use it.
/// Plain value type; cheap to copy.
struct MediaInfo
{
    bool valid = false; ///< false → file unusable; `error` says why
    QString error; ///< human-readable reason when !valid (or a non-fatal note)
    QString path; ///< the path that was probed (as given)
    bool hasVideo = false;
    bool hasAudio = false;
    bool isStillImage = false; ///< png/jpg/...: hasVideo=true, durationUs=0 (infinite)
    int width = 0; ///< coded frame width in pixels (before sample aspect ratio and rotation)
    int height = 0; ///< coded frame height in pixels (before rotation)
    int rotation = 0; ///< display rotation, degrees CLOCKWISE (0/90/180/270); decoders apply it
    Rational frameRate {0, 1}; ///< average/nominal frame rate; invalid ({0,1}) for stills/audio
    int64_t durationUs = 0; ///< length in microseconds (video stream if any, else audio)
    int audioChannels = 0; ///< channels of the audio stream as stored in the file
    int audioSampleRate = 0; ///< Hz, as stored in the file (decoders always output 48 kHz)
    QString videoCodec; ///< FFmpeg short codec name ("h264", "prores", "png", ...)
    QString audioCodec; ///< FFmpeg short codec name ("aac", "mp3", "pcm_s16le", ...)
    QString container; ///< container/format name ("mp4", "mov", "matroska", "png", ...)

    // ---- Added by the media engine (not in the original PLAN §5.3 contract) ----
    /// Size of the frames VideoDecoder::frameAt() returns without a size limit, i.e. after
    /// applying the sample aspect ratio and `rotation`. Use this for "fit to frame" maths.
    int displayWidth = 0;
    int displayHeight = 0; ///< see displayWidth
    Rational sampleAspectRatio {1, 1}; ///< pixel aspect ratio (1:1 = square pixels)
    bool hasAlpha = false; ///< video/still carries an alpha channel (ProRes 4444, PNG, qtrle...)
    QString pixelFormat; ///< FFmpeg pixel format name ("yuv420p", ...); Qt format for stills
    int64_t bitRate = 0; ///< overall bit rate in bits/s, 0 if unknown
    /// Container timestamp (µs) that corresponds to media time 0. Informational only — every
    /// media API already measures time relative to it.
    int64_t startTimeUs = 0;
};

/// Probes `path` and describes its streams. Never throws; on failure returns `valid=false`
/// with `error` set (missing file, garbage data, no audio/video stream...).
/// Still images (png/jpg/webp/bmp/tiff/tga) report hasVideo=true, isStillImage=true,
/// durationUs=0; EXIF orientation is already applied to width/height (rotation stays 0).
/// Synchronous (may take tens of ms for some files); safe to call from any thread.
MediaInfo probe(const QString& path);

} // namespace kk::media
