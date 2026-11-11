// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/DecodeStats.h"

#include <atomic>

namespace kk::media {

namespace {

struct Atomics
{
    std::atomic<int64_t> zeroCopyFrames {0};
    std::atomic<int64_t> zeroCopyUs {0};
    std::atomic<int64_t> downloadedFrames {0};
    std::atomic<int64_t> downloadUs {0};
    std::atomic<int64_t> softwareFrames {0};
    std::atomic<int64_t> importFailures {0};
};

Atomics& counters()
{
    static Atomics s_counters;
    return s_counters;
}

} // namespace

void countZeroCopyFrame(int64_t exportUs)
{
    counters().zeroCopyFrames.fetch_add(1, std::memory_order_relaxed);
    counters().zeroCopyUs.fetch_add(exportUs, std::memory_order_relaxed);
}

void countDownloadedFrame(int64_t downloadUs)
{
    counters().downloadedFrames.fetch_add(1, std::memory_order_relaxed);
    counters().downloadUs.fetch_add(downloadUs, std::memory_order_relaxed);
}

void countSoftwareFrame()
{
    counters().softwareFrames.fetch_add(1, std::memory_order_relaxed);
}

void countImportFailure()
{
    counters().importFailures.fetch_add(1, std::memory_order_relaxed);
}

DecodeCounters decodeCounters()
{
    const Atomics& a = counters();
    DecodeCounters c;
    c.zeroCopyFrames = a.zeroCopyFrames.load(std::memory_order_relaxed);
    c.zeroCopyUs = a.zeroCopyUs.load(std::memory_order_relaxed);
    c.downloadedFrames = a.downloadedFrames.load(std::memory_order_relaxed);
    c.downloadUs = a.downloadUs.load(std::memory_order_relaxed);
    c.softwareFrames = a.softwareFrames.load(std::memory_order_relaxed);
    c.importFailures = a.importFailures.load(std::memory_order_relaxed);
    return c;
}

QString describeDecodeWindow(const DecodeCounters& from, const DecodeCounters& to)
{
    const int64_t zc = to.zeroCopyFrames - from.zeroCopyFrames;
    const int64_t dl = to.downloadedFrames - from.downloadedFrames;
    const int64_t sw = to.softwareFrames - from.softwareFrames;
    if (zc + dl + sw == 0) {
        return {};
    }
    auto each = [](int64_t us, int64_t n) { return n > 0 ? double(us) / double(n) / 1000.0 : 0.0; };
    QString text = QStringLiteral("frames: %1 zero-copy (%2 ms each), %3 downloaded (%4 ms each), "
                                  "%5 software")
                       .arg(zc)
                       .arg(each(to.zeroCopyUs - from.zeroCopyUs, zc), 0, 'f', 2)
                       .arg(dl)
                       .arg(each(to.downloadUs - from.downloadUs, dl), 0, 'f', 1)
                       .arg(sw);
    if (const int64_t failed = to.importFailures - from.importFailures; failed > 0) {
        text += QStringLiteral(", %1 GPU imports failed").arg(failed);
    }
    return text;
}

} // namespace kk::media
