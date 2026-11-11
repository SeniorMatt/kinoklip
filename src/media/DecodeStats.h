// SPDX-License-Identifier: GPL-3.0-or-later
// DecodeStats — process-wide counters of how decoded frames reached the compositor
// (hardware surface exported as DMA-BUF, hardware surface downloaded, software frame), for the
// per-second playback statistics in the session log (Help > Export Log File).
#pragma once

#include <QString>

#include <cstdint>

namespace kk::media {

/// Monotonic counters; take two snapshots and subtract them to get a window.
struct DecodeCounters
{
    int64_t zeroCopyFrames = 0; ///< hardware frames handed to the GPU as DMA-BUF (no copy)
    int64_t zeroCopyUs = 0; ///< time spent exporting them (vaSyncSurface + export)
    int64_t downloadedFrames = 0; ///< hardware frames copied to system memory
    int64_t downloadUs = 0; ///< time spent copying them (incl. waiting for the GPU)
    int64_t softwareFrames = 0; ///< frames of software decoders returned to the compositor
    int64_t importFailures = 0; ///< DMA-BUF imports that failed on the GPU side (fell back)
};

/// Called by the decoder / renderer. Thread-safe and wait-free.
void countZeroCopyFrame(int64_t exportUs);
void countDownloadedFrame(int64_t downloadUs);
void countSoftwareFrame();
void countImportFailure();

/// Current counters. Thread-safe.
DecodeCounters decodeCounters();

/// "hw 59 zero-copy (0.1 ms each), 0 downloaded, 0 software" for the frames between two
/// snapshots; empty when no frame was counted.
QString describeDecodeWindow(const DecodeCounters& from, const DecodeCounters& to);

} // namespace kk::media
