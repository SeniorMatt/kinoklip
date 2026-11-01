// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoderImpl — internal: the state of a VideoDecoder, shared by VideoDecoder.cpp (opening,
// hardware selection, output) and VideoDecoderSeek.cpp (decode/seek state machine).
#pragma once

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/HwDevices.h"
#include "media/ImageFrameList.h"
#include "media/VideoDecoder.h"
#include "media/VideoFrameFactory.h"

#include <QImage>

#include <optional>
#include <vector>

namespace kk::media {

namespace detail {

/// A decoder output frame (hardware surface or software frame) plus its timestamp, and the
/// VideoFrame made from it — created lazily, only for frames that are actually returned.
struct DecodedFrame
{
    FramePtr av;
    int64_t pts = AV_NOPTS_VALUE;
    std::shared_ptr<const VideoFrame> video;
    explicit operator bool() const { return av != nullptr; }
};

} // namespace detail

// ---- Implementation state ---------------------------------------------------------------
//
// The decoder keeps two frames: `cur` (the last frame handed out, or about to be) and `next`
// (the frame after it, decoded as look-ahead). `cur` is the right answer for every time in
// [cur.pts, next.pts) — or [cur.pts, ∞) once the stream hit EOF. Knowing `next` is what makes
// the answer exact even for variable-frame-rate files with unreliable frame durations.
//
// Allocation-free steady state: AVPacket/AVFrame structs are recycled (spareFrames), decoder
// buffers come from FFmpeg's pools, GPU downloads from VideoFrameFactory's pool.

struct VideoDecoder::Impl
{
    MediaInfo info;
    int threadCount = 0;
    std::optional<HwAccelMode> hwOverride;
    bool isOpen = false;
    int64_t lastReturnedUs = -1;

    // Still images: decoded once, scaled copies on demand.
    bool isStill = false;
    QImage still;
    QImage stillScaled;
    QSize stillScaledFor;
    std::shared_ptr<const VideoFrame> stillFrame;

    // Image sequences and Qt animations (animated WebP): one image per frame.
    std::unique_ptr<detail::ImageFrameList> frameList;
    int frameListIndex = -1; ///< index of the frame `frameListImage` holds
    QImage frameListImage; ///< full size
    QImage frameListScaled; ///< frameListImage scaled for frameListScaledFor
    QSize frameListScaledFor;
    std::shared_ptr<const VideoFrame> frameListVideo; ///< frameListImage wrapped

    // FFmpeg state (video files).
    detail::FormatContextPtr fmt;
    detail::CodecContextPtr codec;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    detail::PacketPtr packet;
    detail::FramePtr scratch;
    std::vector<detail::FramePtr> spareFrames; ///< recycled AVFrame structs (no per-frame malloc)
    detail::FrameConverter converter;
    detail::VideoFrameFactory factory;
    int64_t originTs = 0; ///< stream timestamp of media time 0
    int64_t firstTs = 0; ///< earliest timestamp of the stream (seek lower bound)
    int64_t firstFrameTs = 0; ///< pts of the first decodable frame (clamp lower bound)
    int64_t frameStepTs = 1; ///< one nominal frame period in stream time base
    int64_t lastDecodedTs = AV_NOPTS_VALUE;
    int64_t lastSeekTs = 0;
    bool eof = false; ///< decoder fully drained; `cur` is the last frame of the stream
    bool draining = false; ///< demuxer hit EOF, flush packet sent to the decoder
    bool afterSeek = false; ///< no frame delivered since the last seek
    detail::DecodedFrame cur;
    detail::DecodedFrame next;
    QImage curImage; ///< `cur` converted for curImageMaxSize
    QSize curImageMaxSize;
    bool curImageValid = false;

    // Hardware decoding.
    HwAccelMode activeHw = HwAccelMode::Off;
    detail::HwDecodeState hwState; ///< get_format callback state (codec->opaque points here)
    bool hwFailed = false; ///< hw decoding broke → switch to software at the next chance
    int hwPacketErrors = 0;

    // Soft seeks (see seek()): skip avcodec_flush_buffers() when re-initialising the decoder
    // is expensive and the new position starts with an IDR picture.
    bool softSeekAllowed = false;
    bool softSeekPending = false; ///< demuxer moved, decoder not flushed yet
    intptr_t generation = 0; ///< tags packets/frames; frames of older generations are stale

    bool openStill(const QString& path, const ImageHeader& header, QString* error);
    bool openFrameList(
        const QString& path, detail::MediaRoute route, const ImageHeader& header, QString* error);
    bool frameListPosition(int64_t timeUs);
    static QImage scaledCopy(const QImage& image, QSize maxSize, QImage& scaled, QSize& scaledFor);
    bool openVideo(const QString& path, QString* error);
    bool openCodec(HwAccelMode requested, QString* error);
    bool switchToSoftware();
    QImage stillAt(QSize maxSize);

    int64_t usToTs(int64_t us) const
    {
        return originTs
            + av_rescale_q_rnd(us, detail::kMicrosecondsTb, stream->time_base,
                AVRounding(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX));
    }
    int64_t tsToUs(int64_t ts) const
    {
        return av_rescale_q(ts - originTs, stream->time_base, detail::kMicrosecondsTb);
    }

    detail::FramePtr takeFrame();
    void release(detail::DecodedFrame& frame);
    void releaseAll();
    bool isCurrent(int64_t ts) const
    {
        return cur && cur.pts <= ts && (next ? ts < next.pts : eof);
    }
    bool decodeOne(detail::DecodedFrame& out);
    bool seek(int64_t targetTs, int attempt);
    int64_t keyframeAtOrBefore(int64_t ts) const;
    bool canDecodeForwardTo(int64_t ts) const;
    void advanceTo(int64_t ts);
    bool positionExact(int64_t ts);
    bool positionNearKeyframe(int64_t ts);
    bool position(int64_t ts, Accuracy accuracy);
    std::shared_ptr<const VideoFrame> currentVideoFrame();
    QImage currentImage(QSize maxSize);
};

} // namespace kk::media
