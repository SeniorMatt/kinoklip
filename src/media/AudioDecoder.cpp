// SPDX-License-Identifier: GPL-3.0-or-later
// AudioDecoder — FFmpeg + libswresample decoding to 48 kHz stereo float with exact positioning.
#include "media/AudioDecoder.h"

#include "media/FFmpegUtil.h"
#include "media/MediaLog.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <numeric>
#include <vector>

namespace kk::media {

using namespace detail;

namespace {

/// After a seek we start decoding this long before the target and throw the extra samples
/// away. Covers codec warm-up (AAC/Opus overlap, MP3 bit reservoir) and resampler history,
/// so a random read is bit-identical (or very nearly) to a contiguous one.
constexpr int64_t kSeekPrerollUs = 200'000;

/// A target less than this many samples ahead of the decoded data is reached by decoding
/// forward (cheaper than a seek + pre-roll).
constexpr int64_t kForwardDecodeLimitSamples = AudioDecoder::kSampleRate;

/// Samples before the last read position kept in memory, so slightly overlapping reads
/// (e.g. a mixer re-reading a block) do not cost a seek.
constexpr int64_t kKeptHistorySamples = AudioDecoder::kSampleRate / 10;

/// Until the real end is known, reads starting this far past the estimated length return
/// silence without touching the decoder.
constexpr int64_t kEndSlackSamples = AudioDecoder::kSampleRate;

/// A seek that lands after the target is retried with a doubled pre-roll this many times;
/// the last retry seeks to the start of the stream.
constexpr int kMaxSeekRetries = 4;

/// Upper bound for the input samples dropped to align the resampling phase after a seek
/// (see appendFrame); well inside kSeekPrerollUs for every common sample rate.
constexpr int64_t kMaxPhaseAlignSamples = 4096;

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

} // namespace

// ---- Implementation state ---------------------------------------------------------------
//
// Decoded, resampled audio is kept in `buffer` (interleaved stereo) and covers the sample
// range [bufferStart, bufferEnd()). Its position is anchored once after each seek from the
// first decoded frame's timestamp; from then on samples are counted, which keeps contiguous
// decoding exact even for codecs with imprecise per-frame timestamps.

struct AudioDecoder::Impl
{
    MediaInfo info;
    bool isOpen = false;

    FormatContextPtr fmt;
    CodecContextPtr codec;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    PacketPtr packet;
    FramePtr frame;
    SwrContextPtr swr;
    AVChannelLayout swrInLayout {}; ///< input configuration `swr` was built for
    int swrInRate = 0;
    int swrInFormat = -1;

    int64_t originTs = 0; ///< stream timestamp of media time 0
    int64_t firstTs = 0; ///< earliest timestamp of the stream (seek lower bound)
    int64_t seekPrerollUs = kSeekPrerollUs;
    int64_t estimatedLength = 0; ///< from the container duration
    int64_t exactLength = -1; ///< set once EOF was reached with a known position

    std::vector<float> buffer;
    size_t bufferOffset = 0; ///< floats at the front of `buffer` that were already discarded
    int64_t bufferStart = 0; ///< sample index of buffer[bufferOffset]
    bool positionValid = false;
    bool eof = false;
    bool draining = false;
    int64_t lastSeekTs = 0;
    int64_t inputSkip = 0; ///< input samples still to drop for phase alignment after a seek
    std::vector<const uint8_t*> inputPlanes; ///< scratch: offset plane pointers

    ~Impl() { av_channel_layout_uninit(&swrInLayout); }

    int64_t bufferEnd() const
    {
        return bufferStart + int64_t((buffer.size() - bufferOffset) / kChannels);
    }

    bool openFile(const QString& path, QString* error);
    void seekTo(int64_t targetSample);
    bool decodeChunk();
    void appendFrame(const AVFrame* decoded);
    bool ensureResampler(const AVFrame* decoded);
    void appendResampled(const uint8_t* const* input, int inputSamples);
    void discardBefore(int64_t sample);
};

bool AudioDecoder::Impl::openFile(const QString& path, QString* error)
{
    fmt = openInput(path, error);
    if (!fmt) {
        return false;
    }
    info = infoFromFormatContext(fmt.get(), path);
    streamIndex = findAudioStream(fmt.get());
    if (streamIndex < 0) {
        setError(error, QStringLiteral("No audio stream"));
        return false;
    }
    stream = fmt->streams[streamIndex];
    codec = openDecoder(stream, 1, error); // audio decoders gain nothing from threads
    if (!codec) {
        return false;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (int(i) != streamIndex) {
            fmt->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    packet.reset(av_packet_alloc());
    frame.reset(av_frame_alloc());
    if (!packet || !frame) {
        setError(error, QStringLiteral("Out of memory"));
        return false;
    }
    originTs = av_rescale_q(info.startTimeUs, kMicrosecondsTb, stream->time_base);
    firstTs = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : originTs;
    const int codecPrerollSamples = stream->codecpar->seek_preroll;
    if (codecPrerollSamples > 0 && stream->codecpar->sample_rate > 0) {
        seekPrerollUs = std::max(seekPrerollUs,
            av_rescale(codecPrerollSamples, 1'000'000, stream->codecpar->sample_rate));
    }
    // The audio length of an A/V file can differ from the video length in info.durationUs.
    int64_t audioEndUs = info.durationUs;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        audioEndUs = av_rescale_q(
            firstTs + stream->duration - originTs, stream->time_base, kMicrosecondsTb);
    } else if (fmt->duration != AV_NOPTS_VALUE) {
        audioEndUs = std::max(audioEndUs, fmt->duration);
    }
    estimatedLength = av_rescale(audioEndUs, kSampleRate, 1'000'000);

    // Validate by decoding the first chunk (also positions us at the start).
    seekTo(0);
    if (!positionValid) {
        setError(error, QStringLiteral("No decodable audio"));
        return false;
    }
    return true;
}

void AudioDecoder::Impl::seekTo(int64_t targetSample)
{
    for (int attempt = 0; attempt <= kMaxSeekRetries; ++attempt) {
        int64_t seekTs = firstTs;
        if (attempt < kMaxSeekRetries) {
            const int64_t seekUs
                = av_rescale(targetSample, 1'000'000, kSampleRate) - (seekPrerollUs << attempt);
            seekTs = originTs + av_rescale_q(seekUs, kMicrosecondsTb, stream->time_base);
        }
        seekTs = std::max(seekTs, firstTs);
        const int ret = seekDemuxer(fmt.get(), streamIndex, seekTs, firstTs);
        avcodec_flush_buffers(codec.get());
        swr.reset(); // drops resampler history; rebuilt by the first frame
        buffer.clear();
        bufferOffset = 0;
        positionValid = false;
        inputSkip = 0;
        eof = draining = false;
        lastSeekTs = seekTs;
        if (ret < 0) {
            qCWarning(lcMedia) << "audio seek failed in" << info.path << avErrorString(ret);
            eof = true;
            return;
        }
        if (!decodeChunk() && !positionValid) {
            return; // nothing decodable after the seek point → silence
        }
        if (bufferStart <= targetSample || seekTs <= firstTs) {
            return;
        }
        // Landed after the target (imprecise index): retry with a longer pre-roll.
    }
}

bool AudioDecoder::Impl::decodeChunk()
{
    for (;;) {
        int ret = avcodec_receive_frame(codec.get(), frame.get());
        if (ret == 0) {
            appendFrame(frame.get());
            av_frame_unref(frame.get());
            return true;
        }
        if (ret == AVERROR_EOF || (ret == AVERROR(EAGAIN) && draining)) {
            if (swr) {
                appendResampled(nullptr, 0); // flush the resampler's delayed samples
            }
            eof = true;
            if (positionValid) {
                exactLength = bufferEnd();
            }
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            qCDebug(lcMedia) << "audio decoder error:" << avErrorString(ret);
            eof = true;
            return false;
        }
        ret = av_read_frame(fmt.get(), packet.get());
        if (ret < 0) {
            // End of file (or a read error, treated alike): ask the decoder for its delayed
            // frames; receive_frame returns them one by one, then AVERROR_EOF.
            avcodec_send_packet(codec.get(), nullptr);
            draining = true;
            continue;
        }
        if (packet->stream_index == streamIndex) {
            ret = avcodec_send_packet(codec.get(), packet.get());
            if (ret < 0) {
                qCDebug(lcMedia) << "skipping undecodable audio packet:" << avErrorString(ret);
            }
        }
        av_packet_unref(packet.get());
    }
}

void AudioDecoder::Impl::appendFrame(const AVFrame* decoded)
{
    const int inRate = decoded->sample_rate;
    if (!positionValid && inRate > 0) {
        // Anchor the buffer: first decoded sample after a seek ↔ its timestamp.
        int64_t pts = decoded->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
            pts = lastSeekTs; // best guess for timestamp-less streams
        }
        const int64_t firstInput = av_rescale_q(pts - originTs, stream->time_base, {1, inRate});
        // Resampling phase: the input and 48 kHz sample grids only coincide every
        // `periodIn` input samples (147 for 44.1 kHz). Starting the resampler between grid
        // points would shift its output by a fraction of a sample compared to a contiguous
        // decode, so we drop input samples up to the next common grid point. That makes
        // random reads match contiguous ones (and anchors bufferStart exactly).
        const int64_t common = std::gcd(int64_t(inRate), int64_t(kSampleRate));
        const int64_t periodIn = inRate / common;
        int64_t skip = ((-firstInput) % periodIn + periodIn) % periodIn;
        if (periodIn > kMaxPhaseAlignSamples) {
            skip = 0; // exotic rate: accept a sub-sample phase difference instead
        }
        inputSkip = skip;
        bufferStart = av_rescale(firstInput + skip, kSampleRate, inRate);
        buffer.clear();
        bufferOffset = 0;
        positionValid = true;
    }
    if (!ensureResampler(decoded)) {
        return;
    }
    const int drop = int(std::min<int64_t>(inputSkip, decoded->nb_samples));
    inputSkip -= drop;
    if (drop == 0) {
        appendResampled(decoded->extended_data, decoded->nb_samples);
        return;
    }
    if (drop == decoded->nb_samples) {
        return;
    }
    // Feed the frame minus its first `drop` samples (pointer offset per plane).
    const auto format = AVSampleFormat(decoded->format);
    const int bytesPerSample = av_get_bytes_per_sample(format);
    const int channels = decoded->ch_layout.nb_channels;
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    inputPlanes.assign(size_t(planar ? channels : 1), nullptr);
    for (size_t plane = 0; plane < inputPlanes.size(); ++plane) {
        inputPlanes[plane] = decoded->extended_data[plane]
            + size_t(drop) * size_t(bytesPerSample) * (planar ? 1 : channels);
    }
    appendResampled(inputPlanes.data(), decoded->nb_samples - drop);
}

bool AudioDecoder::Impl::ensureResampler(const AVFrame* decoded)
{
    AVChannelLayout inLayout {};
    if (decoded->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&inLayout, decoded->ch_layout.nb_channels);
    } else if (av_channel_layout_copy(&inLayout, &decoded->ch_layout) < 0) {
        return false;
    }
    if (swr && av_channel_layout_compare(&inLayout, &swrInLayout) == 0
        && decoded->sample_rate == swrInRate && decoded->format == swrInFormat) {
        av_channel_layout_uninit(&inLayout);
        return true;
    }

    SwrContext* raw = nullptr;
    const AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    int ret = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_FLT, kSampleRate, &inLayout,
        AVSampleFormat(decoded->format), decoded->sample_rate, 0, nullptr);
    SwrContextPtr context(raw);
    if (ret >= 0) {
        if (inLayout.nb_channels == 1) {
            // Mono → both channels at unity gain (swresample's default would be -3 dB).
            const double monoToStereo[2] = {1.0, 1.0};
            ret = swr_set_matrix(raw, monoToStereo, 1);
        } else {
            // Downmixes are normalised so the sum of channels cannot exceed full scale.
            ret = av_opt_set_double(raw, "rematrix_maxval", 1.0, 0);
        }
    }
    if (ret >= 0) {
        ret = swr_init(raw);
    }
    if (ret < 0) {
        qCWarning(lcMedia) << "cannot set up resampler for" << info.path << avErrorString(ret);
        av_channel_layout_uninit(&inLayout);
        return false;
    }
    swr = std::move(context);
    av_channel_layout_uninit(&swrInLayout);
    swrInLayout = inLayout; // takes ownership of inLayout's (possible) heap data
    swrInRate = decoded->sample_rate;
    swrInFormat = decoded->format;
    return true;
}

void AudioDecoder::Impl::appendResampled(const uint8_t* const* input, int inputSamples)
{
    const int maxOut = swr_get_out_samples(swr.get(), inputSamples);
    if (maxOut <= 0) {
        return;
    }
    const size_t oldSize = buffer.size();
    buffer.resize(oldSize + size_t(maxOut) * kChannels);
    auto* out = reinterpret_cast<uint8_t*>(buffer.data() + oldSize);
    const int written
        = swr_convert(swr.get(), &out, maxOut, const_cast<const uint8_t**>(input), inputSamples);
    buffer.resize(oldSize + size_t(std::max(0, written)) * kChannels);
}

void AudioDecoder::Impl::discardBefore(int64_t sample)
{
    const int64_t drop = std::min(sample - bufferStart, bufferEnd() - bufferStart);
    if (drop <= 0) {
        return;
    }
    bufferOffset += size_t(drop) * kChannels;
    bufferStart += drop;
    // Compact once the dead prefix dominates, so the vector does not grow forever.
    if (bufferOffset > buffer.size() / 2) {
        buffer.erase(buffer.begin(), buffer.begin() + std::ptrdiff_t(bufferOffset));
        bufferOffset = 0;
    }
}

// ---- Public API ---------------------------------------------------------------------------

AudioDecoder::AudioDecoder()
    : m_impl(std::make_unique<Impl>())
{ }

AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::open(const QString& path, QString* error)
{
    close();
    if (!m_impl->openFile(path, error)) {
        close();
        return false;
    }
    m_impl->isOpen = true;
    return true;
}

void AudioDecoder::close()
{
    m_impl = std::make_unique<Impl>();
}

bool AudioDecoder::isOpen() const
{
    return m_impl->isOpen;
}

const MediaInfo& AudioDecoder::info() const
{
    return m_impl->info;
}

int64_t AudioDecoder::lengthSamples() const
{
    return m_impl->exactLength >= 0 ? m_impl->exactLength : m_impl->estimatedLength;
}

int AudioDecoder::read(int64_t startSample48k, float* dst, int frames)
{
    if (frames <= 0) {
        return 0;
    }
    std::fill_n(dst, size_t(frames) * kChannels, 0.0f);
    Impl& d = *m_impl;
    if (!d.isOpen) {
        return frames;
    }
    const int64_t end = startSample48k + frames;
    const int64_t from = std::max<int64_t>(startSample48k, 0);
    const int64_t knownEnd
        = d.exactLength >= 0 ? d.exactLength : d.estimatedLength + kEndSlackSamples;
    if (from >= end || from >= knownEnd) {
        return frames;
    }

    const bool reachable = d.positionValid && from >= d.bufferStart
        && from <= d.bufferEnd() + kForwardDecodeLimitSamples;
    if (!reachable) {
        d.seekTo(from);
    }
    while (d.bufferEnd() < end && !d.eof) {
        if (!d.decodeChunk()) {
            break;
        }
        d.discardBefore(from - kKeptHistorySamples); // forward skips: drop as we go
    }

    const int64_t copyFrom = std::max(from, d.bufferStart);
    const int64_t copyTo = std::min(end, d.bufferEnd());
    if (copyTo > copyFrom) {
        const float* src
            = d.buffer.data() + d.bufferOffset + size_t(copyFrom - d.bufferStart) * kChannels;
        std::copy_n(src, size_t(copyTo - copyFrom) * kChannels,
            dst + size_t(copyFrom - startSample48k) * kChannels);
    }
    d.discardBefore(from - kKeptHistorySamples);
    return frames;
}

} // namespace kk::media
