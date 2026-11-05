// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoderSeek — the decode/seek state machine of VideoDecoder: decoding one frame,
// seeking with retries, deciding between decode-forward and seek, positioning on a target.
#include "media/MediaLog.h"
#include "media/VideoDecoderImpl.h"

#include <algorithm>

namespace kk::media {

using namespace detail;

namespace {

/// Without a usable keyframe index, a target less than this far ahead of the decoder position
/// is reached by decoding forward; anything further triggers a seek.
constexpr int64_t kForwardDecodeLimitUs = 2'000'000;

/// When a seek lands after the target (B-frame reordering, imprecise index) or nothing is
/// decodable after it, we seek again this much earlier, doubling each time; the last retry
/// rewinds to the start of the stream.
constexpr int64_t kSeekBackoffUs = 1'000'000;
constexpr int kMaxSeekRetries = 4;

/// After a seek, decoders may emit damaged frames until the next keyframe; skip at most
/// this many of them (a genuinely damaged file still returns *something*).
constexpr int kMaxSkippedCorruptFrames = 60;

/// Consecutive packet errors from a hardware decoder before we give up on it.
constexpr int kMaxHwPacketErrors = 3;

} // namespace

// ---- Frame bookkeeping ----------------------------------------------------------------------

FramePtr VideoDecoder::Impl::takeFrame()
{
    if (spareFrames.empty()) {
        return FramePtr(av_frame_alloc());
    }
    FramePtr frame = std::move(spareFrames.back());
    spareFrames.pop_back();
    return frame;
}

void VideoDecoder::Impl::release(DecodedFrame& frame)
{
    if (frame.av) {
        av_frame_unref(frame.av.get()); // returns the buffer / hw surface to its pool
        spareFrames.push_back(std::move(frame.av));
    }
    frame.av = nullptr;
    frame.video.reset();
    frame.pts = AV_NOPTS_VALUE;
}

void VideoDecoder::Impl::releaseAll()
{
    release(cur);
    release(next);
    curImageValid = false;
}

// ---- Decoding and seeking -------------------------------------------------------------------

bool VideoDecoder::Impl::decodeOne(DecodedFrame& out)
{
    int skippedCorrupt = 0;
    for (;;) {
        int ret = 0;
        {
            const auto lock = lockHw(); // see HwDeviceManager::callLock()
            ret = avcodec_receive_frame(codec.get(), scratch.get());
        }
        if (ret == 0) {
            if (reinterpret_cast<intptr_t>(scratch->opaque) != generation) {
                av_frame_unref(scratch.get()); // decoded before a soft seek: stale
                continue;
            }
            if (activeHw != HwAccelMode::Off && hwState.rejected) {
                // FFmpeg could not use the GPU for this stream and decodes in software with
                // our single hardware thread: better to reopen a proper software decoder.
                av_frame_unref(scratch.get());
                hwFailed = true;
                return false;
            }
            if (afterSeek && (scratch->flags & AV_FRAME_FLAG_CORRUPT)
                && skippedCorrupt < kMaxSkippedCorruptFrames) {
                ++skippedCorrupt; // references missing after the seek; wait for a clean frame
                av_frame_unref(scratch.get());
                continue;
            }
            int64_t pts = scratch->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                pts = lastDecodedTs != AV_NOPTS_VALUE ? lastDecodedTs + frameStepTs : firstTs;
            }
            if (afterSeek) {
                qCDebug(lcMedia) << "first frame after seek at" << tsToUs(pts) << "us";
                afterSeek = false;
            }
            lastDecodedTs = pts;
            ++decodedFrames;
            release(out);
            out.av = takeFrame();
            if (!out.av) {
                av_frame_unref(scratch.get());
                return false;
            }
            av_frame_move_ref(out.av.get(), scratch.get());
            out.pts = pts;
            return true;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            qCDebug(lcMedia) << "video decoder error:" << avErrorString(ret);
            hwFailed = activeHw != HwAccelMode::Off;
            hwFailReason = QStringLiteral("decoder error: ") + avErrorString(ret);
            return false;
        }
        if (draining) {
            return false;
        }
        // The decoder wants more input.
        ret = av_read_frame(fmt.get(), packet.get());
        if (ret < 0) {
            if (ret != AVERROR_EOF) {
                qCDebug(lcMedia) << "read error, treating as end of stream:" << avErrorString(ret);
            }
            {
                const auto lock = lockHw();
                avcodec_send_packet(codec.get(), nullptr); // flush: returns the delayed frames
            }
            draining = true;
            continue;
        }
        if (packet->stream_index == streamIndex) {
            if (softSeekPending) {
                // First packet at the new position: an IDR resets all decoder references, so
                // the flush can be skipped; anything else needs the real flush.
                softSeekPending = false;
                if (!isH264IdrPacket(packet.get(), stream->codecpar)) {
                    const auto lock = lockHw();
                    avcodec_flush_buffers(codec.get());
                }
            }
            packet->opaque = reinterpret_cast<void*>(generation);
            {
                const auto lock = lockHw();
                ret = avcodec_send_packet(codec.get(), packet.get());
            }
            if (ret < 0) {
                qCDebug(lcMedia) << "skipping undecodable packet:" << avErrorString(ret);
                if (activeHw != HwAccelMode::Off && ++hwPacketErrors >= kMaxHwPacketErrors) {
                    av_packet_unref(packet.get());
                    hwFailed = true;
                    hwFailReason = QStringLiteral("%1 packets in a row rejected: %2")
                                       .arg(hwPacketErrors)
                                       .arg(avErrorString(ret));
                    return false;
                }
            } else {
                hwPacketErrors = 0;
            }
        }
        av_packet_unref(packet.get());
    }
}

bool VideoDecoder::Impl::seek(int64_t targetTs, int attempt)
{
    int64_t seekTs = targetTs;
    if (attempt >= kMaxSeekRetries) {
        seekTs = firstTs;
    } else if (attempt > 0) {
        seekTs = targetTs
            - av_rescale_q(kSeekBackoffUs << (attempt - 1), kMicrosecondsTb, stream->time_base);
    }
    seekTs = std::max(seekTs, firstTs);
    const int ret = seekDemuxer(fmt.get(), streamIndex, seekTs, firstTs);
    if (ret < 0) {
        qCWarning(lcMedia) << "seek failed in" << info.path << avErrorString(ret);
        return false;
    }
    qCDebug(lcMedia) << "video seek" << info.path << "target" << tsToUs(targetTs) << "us, attempt"
                     << attempt << "seek point" << tsToUs(seekTs) << "us";
    ++generation;
    if (softSeekAllowed && !draining) {
        softSeekPending = true; // decided at the first packet (see decodeOne)
    } else {
        const auto lock = lockHw();
        avcodec_flush_buffers(codec.get()); // also required to leave the drained (EOF) state
        softSeekPending = false;
    }
    releaseAll();
    eof = draining = false;
    afterSeek = true;
    lastSeekTs = seekTs;
    lastDecodedTs = AV_NOPTS_VALUE;
    return true;
}

int64_t VideoDecoder::Impl::keyframeAtOrBefore(int64_t ts) const
{
    const int count = avformat_index_get_entries_count(stream);
    if (count <= 0) {
        return AV_NOPTS_VALUE;
    }
    // Some demuxers (MPEG-TS, ...) build their index while reading; such an index says nothing
    // about regions not read yet, so only trust it when it reaches the target.
    const AVIndexEntry* last = avformat_index_get_entry(stream, count - 1);
    if (!last || last->timestamp < ts) {
        return AV_NOPTS_VALUE;
    }
    const AVIndexEntry* entry
        = avformat_index_get_entry_from_timestamp(stream, ts, AVSEEK_FLAG_BACKWARD);
    return entry ? entry->timestamp : AV_NOPTS_VALUE;
}

bool VideoDecoder::Impl::canDecodeForwardTo(int64_t ts) const
{
    if (!cur || eof || ts < cur.pts) {
        return false;
    }
    const int64_t position = next ? next.pts : cur.pts;
    if (ts <= position) {
        return true;
    }
    // Seeking only pays off if it lands on a keyframe after our current position.
    const int64_t keyframe = keyframeAtOrBefore(ts);
    if (keyframe != AV_NOPTS_VALUE) {
        return keyframe <= position;
    }
    return ts - position < av_rescale_q(kForwardDecodeLimitUs, kMicrosecondsTb, stream->time_base);
}

void VideoDecoder::Impl::advanceTo(int64_t ts)
{
    for (;;) {
        if (!next) {
            if (eof) {
                return;
            }
            if (stopCheck && (*stopCheck)()) {
                stopped = true;
                return;
            }
            if (!decodeOne(next)) {
                eof = !hwFailed;
                return;
            }
        }
        if (next.pts > ts) {
            return;
        }
        // Skipped frames are recycled without ever being converted or downloaded.
        release(cur);
        cur = std::move(next);
        next = DecodedFrame {};
        curImageValid = false;
    }
}

bool VideoDecoder::Impl::positionExact(int64_t ts)
{
    if (isCurrent(ts)) {
        return true;
    }
    if (!canDecodeForwardTo(ts) && !seek(ts, 0)) {
        return false;
    }
    for (int attempt = 0;; ++attempt) {
        advanceTo(ts);
        if (stopped) {
            return false;
        }
        if (cur || hwFailed) {
            return cur && !hwFailed;
        }
        const bool atStart = lastSeekTs <= firstTs || attempt >= kMaxSeekRetries;
        if (next && atStart) {
            // Started at the beginning and the first frame is still after the target:
            // the target precedes the first frame → clamp to it.
            cur = std::move(next);
            next = DecodedFrame {};
            return true;
        }
        if (atStart) {
            return false; // nothing decodable in the whole stream
        }
        // Either the first frame after the seek is past the target (B-frame reordering,
        // imprecise index) or nothing was decodable after the seek point (seek landed mid-GOP
        // near the end, truncated file): seek further back and try again.
        if (!seek(ts, attempt + 1)) {
            return false;
        }
    }
}

bool VideoDecoder::Impl::positionNearKeyframe(int64_t ts)
{
    if (isCurrent(ts)) {
        return true;
    }
    if (cur && ts >= cur.pts) {
        // If the keyframe a seek would land on is not after the current frame, the current
        // frame is at least as close to the target — keep it.
        const int64_t keyframe = keyframeAtOrBefore(ts);
        if (keyframe != AV_NOPTS_VALUE && keyframe <= cur.pts) {
            return true;
        }
    }
    for (int attempt = 0; attempt <= kMaxSeekRetries; ++attempt) {
        if (!seek(ts, attempt)) {
            return false;
        }
        const bool atStart = lastSeekTs <= firstTs || attempt == kMaxSeekRetries;
        DecodedFrame frame;
        if (!decodeOne(frame)) {
            eof = !hwFailed;
            if (atStart || hwFailed) {
                return false;
            }
            continue; // nothing decodable after this seek point: retry further back
        }
        if (frame.pts <= ts || atStart) {
            cur = std::move(frame);
            return true;
        }
        release(frame);
    }
    return false;
}

bool VideoDecoder::Impl::position(int64_t ts, Accuracy accuracy)
{
    auto run = [&] {
        return accuracy == Accuracy::Exact ? positionExact(ts) : positionNearKeyframe(ts);
    };
    bool ok = run();
    if (stopped) {
        return false; // not a failure: prepareFrame() was told to give up
    }
    if (activeHw != HwAccelMode::Off && (hwFailed || !ok)) {
        // Mid-stream hardware failure (or a stream the GPU cannot handle): redo in software.
        ok = switchToSoftware() && run();
    }
    return ok && cur;
}

} // namespace kk::media
