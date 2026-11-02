// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware decoding robustness: failed downloads (fault injection) retry once, then fall back to
// software without losing or corrupting a frame; several decoders share one device from several
// threads (CUDA on the dev box as the analogue of the iHD VAAPI case; VAAPI too when a render
// node is accessible); FFmpeg log capture and the quiet/once-only log lines.
#include "MediaTestUtil.h"
#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"
#include "media/HwDevices.h"
#include "media/MediaLog.h"
#include "media/VideoDecoder.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace kk;
using namespace kk::media;

namespace {

constexpr int64_t kFrameUs = 40'000; // h264_25fps.mp4
constexpr int kFrames = 50;

int64_t frameTimeUs(int k)
{
    return k * kFrameUs + kFrameUs / 2;
}

/// Hash of the luma plane: identical for bit-exact decoders (H.264 is), whatever the layout
/// (software yuv420p or downloaded NV12).
QByteArray lumaHash(const std::shared_ptr<const VideoFrame>& frame)
{
    if (!frame) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const int rowBytes = frame->planeWidth(0) * frame->bytesPerPixel(0);
    for (int y = 0; y < frame->planeHeight(0); ++y) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char*>(frame->data[0]) + qsizetype(y) * frame->linesize[0],
            rowBytes));
    }
    return hash.result();
}

/// Captured Qt messages of the kk.* categories.
struct Messages
{
    std::mutex mutex;
    QStringList warnings; ///< warning or worse
    QStringList infos;
    QStringList all;
};
Messages* g_messages = nullptr;
QtMessageHandler g_previous = nullptr;

void handler(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    if (g_messages && context.category && QByteArray(context.category).startsWith("kk.")) {
        const std::lock_guard lock(g_messages->mutex);
        g_messages->all << text;
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
            g_messages->warnings << text;
        } else if (type == QtInfoMsg) {
            g_messages->infos << text;
        }
    }
    if (g_previous) {
        g_previous(type, context, text);
    }
}

/// Installs the handler for its lifetime.
struct CaptureMessages
{
    Messages messages;
    CaptureMessages()
    {
        g_messages = &messages;
        g_previous = qInstallMessageHandler(handler);
    }
    ~CaptureMessages()
    {
        qInstallMessageHandler(g_previous);
        g_messages = nullptr;
    }
    int count(const QStringList& list, const QString& needle)
    {
        const std::lock_guard lock(messages.mutex);
        int n = 0;
        for (const QString& line : list) {
            n += line.contains(needle) ? 1 : 0;
        }
        return n;
    }
};

/// Hardware backends usable here. VAAPI is only listed when a render node is readable
/// (the dev box's nodes belong to the seat user: then CUDA is the analogue).
QList<HwAccelMode> usableBackends()
{
    QList<HwAccelMode> modes;
    for (const HwDeviceInfo& info : hwCapabilities()) {
        if (info.available && info.mode != HwAccelMode::Vulkan) {
            modes << info.mode;
        }
    }
    return modes;
}

} // namespace

class TestHwDecode : public QObject
{
    Q_OBJECT

    QList<QByteArray> m_reference; ///< software luma hash per frame of h264_25fps.mp4

private Q_SLOTS:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        VideoDecoder decoder;
        decoder.setHwAccel(HwAccelMode::Off);
        QVERIFY(decoder.open(test::fixture("h264_25fps.mp4")));
        for (int k = 0; k < kFrames; ++k) {
            const auto frame = decoder.decodeFrame(frameTimeUs(k));
            QVERIFY(frame);
            m_reference << lumaHash(frame);
        }
        QVERIFY(m_reference.first() != m_reference.last());
        qInfo("usable hardware backends: %d", int(usableBackends().size()));
    }

    void init() { detail::HwDeviceManager::instance().resetFailures(); }

    void logCaptureKeepsFfmpegQuiet()
    {
        detail::initFfmpeg();
        CaptureMessages capture;
        {
            detail::FfmpegLogCapture ffmpeg;
            av_log(nullptr, AV_LOG_ERROR, "probe failed: CUDA_ERROR_NO_DEVICE\n");
            {
                detail::FfmpegLogCapture inner; // nested: both see it
                av_log(nullptr, AV_LOG_ERROR, "second error\n");
                QCOMPARE(inner.errorCount(), 1);
            }
            QCOMPARE(ffmpeg.errorCount(), 2);
            QVERIFY(ffmpeg.firstError().contains(QLatin1String("CUDA_ERROR_NO_DEVICE")));
        }
        QCOMPARE(capture.count(capture.messages.warnings, QStringLiteral("CUDA_ERROR")), 0);
        // A known harmless demuxer complaint stays out of the visible log, a real error not.
        av_log(nullptr, AV_LOG_ERROR, "UDTA parsing failed retrying raw\n");
        av_log(nullptr, AV_LOG_ERROR, "a real error\n");
        QCOMPARE(capture.count(capture.messages.warnings, QStringLiteral("UDTA")), 0);
        QCOMPARE(capture.count(capture.messages.warnings, QStringLiteral("a real error")), 1);
        // Info is the default threshold of kk.media: FFmpeg warnings are debug output.
        QVERIFY(!lcMedia().isDebugEnabled() || qEnvironmentVariableIsSet("QT_LOGGING_RULES"));
        QVERIFY(lcMedia().isInfoEnabled());
    }

    void openLogsOncePerFile()
    {
        CaptureMessages capture;
        for (int i = 0; i < 3; ++i) {
            VideoDecoder decoder;
            decoder.setHwAccel(HwAccelMode::Off);
            QVERIFY(decoder.open(test::fixture("h264_2997.mp4")));
        }
        QCOMPARE(
            capture.count(capture.messages.infos, QStringLiteral("decoding h264_2997.mp4 (h264")),
            1);
        QCOMPARE(capture.count(capture.messages.infos, QStringLiteral("with software (")), 1);
    }

    void downloadFailureFallsBackWithoutLosingFrames_data()
    {
        QTest::addColumn<int>("mode");
        for (HwAccelMode mode : usableBackends()) {
            QTest::newRow(qPrintable(hwAccelName(mode))) << int(mode);
        }
        if (usableBackends().isEmpty()) {
            QTest::newRow("none") << -1;
        }
    }

    void downloadFailureFallsBackWithoutLosingFrames()
    {
        QFETCH(int, mode);
        if (mode < 0) {
            QSKIP("no usable hardware decoder on this machine");
        }
        const auto hw = HwAccelMode(mode);
        auto& manager = detail::HwDeviceManager::instance();
        CaptureMessages capture;
        VideoDecoder decoder;
        decoder.setHwAccel(hw);
        QVERIFY(decoder.open(test::fixture("h264_25fps.mp4")));
        QCOMPARE(decoder.activeHwAccel(), hw);
        for (int k = 0; k < kFrames; ++k) {
            if (k == 5) {
                detail::injectDownloadFailures(1); // transient: the retry succeeds
            }
            if (k == 20) {
                detail::injectDownloadFailures(2); // persistent: retry fails too
            }
            const auto frame = decoder.decodeFrame(frameTimeUs(k));
            QVERIFY2(frame, qPrintable(QStringLiteral("frame %1 lost").arg(k)));
            QCOMPARE(frame->ptsUs, k * kFrameUs);
            QVERIFY2(lumaHash(frame) == m_reference[k],
                qPrintable(QStringLiteral("frame %1 differs from software").arg(k)));
            QCOMPARE(decoder.activeHwAccel(), k < 20 ? hw : HwAccelMode::Off);
        }
        QCOMPARE(manager.failureCount(hw), 1);
        QCOMPARE(
            capture.count(capture.messages.warnings, QStringLiteral("decoding failed for")), 1);
        QCOMPARE(capture.count(capture.messages.warnings, QStringLiteral("injected download")), 1);
        QCOMPARE(capture.count(capture.messages.warnings, QStringLiteral("ffmpeg")), 0);

        // A failure while opening: software from the start, said in the open line.
        detail::injectDownloadFailures(2);
        VideoDecoder second;
        second.setHwAccel(hw);
        QVERIFY(second.open(test::fixture("h264_25fps.mp4")));
        QCOMPARE(second.activeHwAccel(), HwAccelMode::Off);
        QVERIFY(lumaHash(second.decodeFrame(frameTimeUs(0))) == m_reference[0]);
        QCOMPARE(capture.count(capture.messages.warnings,
                     hwAccelName(hw) + QStringLiteral(" failed: frame download")),
            1);

        // The third failure disables the backend for the session (one warning).
        detail::injectDownloadFailures(2);
        VideoDecoder third;
        third.setHwAccel(hw);
        QVERIFY(third.open(test::fixture("h264_25fps.mp4")));
        QCOMPARE(manager.failureCount(hw), 3);
        QCOMPARE(
            capture.count(capture.messages.warnings, QStringLiteral("disabled for this session")),
            1);
        VideoDecoder fourth;
        fourth.setHwAccel(hw);
        QVERIFY(fourth.open(test::fixture("h264_25fps.mp4")));
        QCOMPARE(fourth.activeHwAccel(), HwAccelMode::Off);
        detail::injectDownloadFailures(0);
        manager.resetFailures();
    }

    void concurrentDecodersShareOneDevice_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("serialise");
        for (HwAccelMode mode : usableBackends()) {
            QTest::newRow(qPrintable(hwAccelName(mode) + QStringLiteral(" serialised")))
                << int(mode) << int(detail::HwSerialise::All);
            QTest::newRow(qPrintable(hwAccelName(mode) + QStringLiteral(" unlocked")))
                << int(mode) << int(detail::HwSerialise::Off);
        }
        if (usableBackends().isEmpty()) {
            QTest::newRow("none") << -1 << 0;
        }
    }

    /// The alpha.3 situation: decode-ahead, parallel decode and thumbnail decoders use one
    /// shared device at the same time. Every thread decodes forward with seeks in between;
    /// every frame must be the right, intact picture and no decoder may fall back.
    void concurrentDecodersShareOneDevice()
    {
        QFETCH(int, mode);
        QFETCH(int, serialise);
        if (mode < 0) {
            QSKIP("no usable hardware decoder on this machine");
        }
        const auto hw = HwAccelMode(mode);
        auto& manager = detail::HwDeviceManager::instance();
        manager.setSerialise(detail::HwSerialise(serialise));
        QCOMPARE(manager.callLock(hw) != nullptr, serialise == int(detail::HwSerialise::All));
        constexpr int kThreads = 6;
        std::atomic<int> wrong {0};
        std::atomic<int> fellBack {0};
        std::atomic<int> decoded {0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                VideoDecoder decoder;
                decoder.setHwAccel(hw);
                if (!decoder.open(test::fixture("h264_25fps.mp4"))) {
                    ++wrong;
                    return;
                }
                for (int round = 0; round < 3; ++round) {
                    const int start = (t * 7 + round * 13) % (kFrames - 12);
                    for (int k = start; k < start + 12; ++k) {
                        const auto frame = decoder.decodeFrame(frameTimeUs(k));
                        if (!frame || lumaHash(frame) != m_reference[k]) {
                            ++wrong;
                        }
                        ++decoded;
                    }
                }
                if (decoder.activeHwAccel() != hw) {
                    ++fellBack;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        manager.setSerialise(detail::HwSerialise::Vaapi);
        QCOMPARE(decoded.load(), kThreads * 3 * 12);
        QCOMPARE(wrong.load(), 0);
        QCOMPARE(fellBack.load(), 0);
        QCOMPARE(manager.failureCount(hw), 0);
    }

    void cudaIsNotTriedWithoutNvidiaDriver()
    {
        const HwDeviceInfo cuda = detail::HwDeviceManager::instance().info(HwAccelMode::Cuda);
        if (!nvidiaDriverPresent()) {
            QVERIFY(!cuda.available);
            QCOMPARE(cuda.error, QStringLiteral("no NVIDIA driver"));
        } else {
            QVERIFY(cuda.error != QStringLiteral("no NVIDIA driver"));
        }
        if (!QFile::exists(QStringLiteral("/dev/dri"))) {
            QVERIFY(!qsvRuntimePresent());
        }
    }
};

QTEST_GUILESS_MAIN(TestHwDecode)
#include "test_hwdecode.moc"
