// SPDX-License-Identifier: GPL-3.0-or-later
// AudioEncoder — internal: encodes the mixer's 48 kHz interleaved stereo float into one audio
// codec (resampling/format conversion with swresample, frame-size chunking with AVAudioFifo).
#pragma once

#include "export/ExportFFmpeg.h"
#include "export/ExportSettings.h"

#include <QString>

#include <cstdint>

namespace kk::exporter {

/// One open audio encoder. Timestamps: time base 1/sampleRate, the first sample is pts 0, so
/// audio starts exactly with the first video frame (codec priming is signalled by the encoder
/// and written as an edit list / pre-skip by the muxer).
///
/// Thread-safety: not thread-safe; use from one thread.
class AudioEncoder
{
public:
    AudioEncoder();
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    /// Opens the encoder for @p settings. @p globalHeader: the muxer wants extradata.
    [[nodiscard]] bool open(const AudioExportSettings& settings, bool globalHeader, QString* error);
    /// The opened codec context, or nullptr.
    [[nodiscard]] AVCodecContext* context() const { return m_ctx.get(); }

    /// Encodes @p frames stereo sample frames at 48 kHz (2·frames floats).
    [[nodiscard]] bool encode(
        const float* interleaved48k, int frames, const PacketSink& sink, QString* error);
    /// Flushes the resampler, pads the last frame and drains the encoder.
    [[nodiscard]] bool flush(const PacketSink& sink, QString* error);

    /// Output samples (at the codec rate) handed to the encoder so far.
    [[nodiscard]] int64_t samplesEncoded() const { return m_nextPts; }

private:
    bool pushConverted(int samples);
    bool encodeFromFifo(bool final, const PacketSink& sink, QString* error);
    bool send(AVFrame* frame, const PacketSink& sink, QString* error);

    CodecContextPtr m_ctx;
    SwrPtr m_swr;
    AudioFifoPtr m_fifo;
    FramePtr m_frame;
    PacketPtr m_packet;
    int m_frameSize = 1024; ///< samples per encoder frame
    int64_t m_nextPts = 0;
    uint8_t** m_convBuffer = nullptr; ///< swr output (codec format), grown on demand
    int m_convCapacity = 0;
};

} // namespace kk::exporter
