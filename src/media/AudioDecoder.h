// SPDX-License-Identifier: GPL-3.0-or-later
// AudioDecoder — sample-accurate random access to the audio of one file as 48 kHz stereo float.
#pragma once

#include "core/ClipAttributes.h"
#include "media/MediaInfo.h"

#include <QString>

#include <cstdint>
#include <memory>

namespace kk::media {

/// Decodes the audio stream of one media file into the engine's fixed audio format:
/// **48 000 Hz, 2 channels, interleaved `float`** (docs/archive/plan.md §5.1), whatever the file contains.
/// Mono is copied to both channels at unity gain; more than two channels are downmixed
/// (libswresample's standard matrix, normalised so it cannot clip).
///
/// Positions are sample-frame indices at 48 kHz from media time 0 (see MediaInfo), so
/// `sample = llround(timeUs * 48000 / 1e6)`. read() is sample-accurate: a random read returns
/// exactly the samples a contiguous read from the start would have produced (seeks pre-roll
/// and discard to the exact sample). Contiguous reads (playback) never seek.
///
/// Thread-safety: NOT thread-safe — one instance per consumer/thread. Separate instances can
/// be used concurrently.
class AudioDecoder
{
public:
    /// Output sample rate of read(), in Hz.
    static constexpr int kSampleRate = 48000;
    /// Output channel count of read() (interleaved L, R).
    static constexpr int kChannels = 2;

    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// Opens the primary audio stream of `path`.
    /// @return false (with a message in `*error`) if the file cannot be read or has no audio.
    [[nodiscard]] bool open(const QString& path, QString* error = nullptr);

    /// Releases the file. Called automatically by the destructor.
    void close();

    /// True between a successful open() and close().
    bool isOpen() const;

    /// Description of the open file (valid=false when nothing is open).
    const MediaInfo& info() const;

    /// Length of the audio in 48 kHz sample frames. Estimated from the container duration
    /// until the decoder has reached the end of the stream once; exact afterwards.
    int64_t lengthSamples() const;

    /// Routes the source channels as Clip Attributes @p attributes say (channelFormat,
    /// left, right; the other fields are ignored): Auto = the default layout above. Can be
    /// called at any time; a change makes the next read() decode again from the exact sample.
    void setChannelMapping(const ClipAttributes& attributes);
    /// The routing set by setChannelMapping() (only its audio fields are meaningful).
    [[nodiscard]] const ClipAttributes& channelMapping() const;
    /// Most source channels an explicit mapping can address (more = default downmix).
    static constexpr int kMaxMappedChannels = 64;

    /// Decodes `frames` sample frames starting at `startSample48k` into `dst`
    /// (`frames * 2` floats, interleaved stereo, nominally in [-1, 1]).
    /// Positions before 0, past the end of the audio or on decoding errors are zero-filled.
    /// @return the number of frames written — always `frames` (or 0 if frames <= 0).
    int read(int64_t startSample48k, float* dst, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace kk::media
