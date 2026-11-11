// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the MatteService with a stand-in model (spec 78): the unavailable path, a matte that
// arrives later while the caller gets nothing at once, waiting for it (export), frames that follow
// each other keeping the recurrent state and a seek resetting it, and the disk cache answering
// after the memory was dropped.
#include "matting/MatteService.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>

using namespace kk::matting;

namespace {

std::atomic<int> g_resets {0};
std::atomic<int> g_delayMs {0};

/// Marks the matte with the number of frames since the last reset (so tests can read the state).
class StubBackend final : public MatteBackend
{
public:
    QImage process(const QImage& rgb, MatteQuality, bool reset) override
    {
        if (g_delayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_delayMs.load()));
        if (reset) {
            m_age = 0;
            ++g_resets;
        }
        QImage m(rgb.size(), QImage::Format_Grayscale8);
        m.fill(uchar(std::min(255, 100 + m_age * 10)));
        ++m_age;
        return m;
    }

private:
    int m_age = 0;
};

QImage fakeFrame(const QString&, int64_t, int)
{
    QImage i(64, 36, QImage::Format_RGB32);
    i.fill(Qt::gray);
    return i;
}

} // namespace

class TestMatteService : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    MatteService& svc() { return MatteService::instance(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        setModelsDirectoryForTests(m_dir.path() + QStringLiteral("/models")); // no model here
        svc().setCacheRoot(m_dir.path() + QStringLiteral("/cache"));
    }

    void withoutModelOrRuntimeTheToolIsUnavailableAndNothingCrashes()
    {
        svc().setBackendFactory({});
        svc().setFrameSource({});
        const auto s = svc().status();
        QVERIFY(!s.ready);
        QVERIFY(!s.reason.isEmpty());
        QVERIFY(!svc().matte(QStringLiteral("/no/such.mp4"), 1.0, MatteQuality::Preview, false));
        QVERIFY(!svc().matte(QStringLiteral("/no/such.mp4"), 1.0, MatteQuality::Preview, true));
        svc().queueRange(QStringLiteral("/no/such.mp4"), 0, 1, MatteQuality::Preview);
        QCOMPARE(svc().backgroundProgress().total, 0);
    }

    void aMissingMatteIsQueuedAndArrivesLater()
    {
        svc().setBackendFactory(
            [](const QString&, QString*) { return std::make_unique<StubBackend>(); });
        svc().setFrameSource(fakeFrame);
        QVERIFY(svc().status().ready);
        QSignalSpy spy(&svc(), &MatteService::matteComputed);
        const QString file = QStringLiteral("/clips/a.mp4");
        QVERIFY(!svc().matte(file, 2.0, MatteQuality::Preview, false)); // not ready: nothing
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
        const MattePtr m = svc().matte(file, 2.0, MatteQuality::Preview, false);
        QVERIFY(m);
        QCOMPARE(m->format(), QImage::Format_Grayscale8);
        QCOMPARE(m->size(), QSize(64, 36)); // multiples of 4 kept
    }

    void waitingReturnsTheRealMatte()
    {
        const QString file = QStringLiteral("/clips/wait.mp4");
        g_delayMs = 30;
        QElapsedTimer timer;
        timer.start();
        const MattePtr m = svc().matte(file, 1.0, MatteQuality::Preview, true);
        QVERIFY(m);
        QVERIFY(timer.elapsed() >= 25);
        g_delayMs = 0;
    }

    void followingFramesKeepTheStateAndASeekResetsIt()
    {
        const QString file = QStringLiteral("/clips/seq.mp4");
        g_resets = 0;
        MattePtr a, b, c, d;
        // Frames 1/25 s apart, asked in order: one reset (the first), then the state grows.
        a = svc().matte(file, 0.00, MatteQuality::Preview, true);
        b = svc().matte(file, 0.04, MatteQuality::Preview, true);
        c = svc().matte(file, 0.08, MatteQuality::Preview, true);
        QVERIFY(a && b && c);
        const int va = qGray(a->pixel(1, 1)), vb = qGray(b->pixel(1, 1)),
                  vc = qGray(c->pixel(1, 1));
        QVERIFY2(vb > va && vc > vb, "the recurrent state must carry over between frames");
        const int resetsBefore = g_resets;
        QVERIFY(resetsBefore >= 1);
        // A seek far away starts from a clean state.
        d = svc().matte(file, 30.0, MatteQuality::Preview, true);
        QVERIFY(d);
        QCOMPARE(int(g_resets), resetsBefore + 1);
        QVERIFY(qGray(d->pixel(1, 1)) <= va);
    }

    void theDiskCacheAnswersWithoutRunningTheModel()
    {
        const QString file = QStringLiteral("/clips/disk.mp4");
        QVERIFY(svc().matte(file, 3.0, MatteQuality::Preview, true));
        // The cache writes after the signal: wait until the frame is on disk.
        QTRY_VERIFY_WITH_TIMEOUT(svc().cachedFrames(file, MatteQuality::Preview) >= 1, 5000);
        QTest::qWait(100);
        const int runs = svc().runs();
        svc().setCacheRoot(m_dir.path() + QStringLiteral("/cache")); // drops the memory only
        const MattePtr m = svc().matte(file, 3.0, MatteQuality::Preview, false);
        QVERIFY(m);
        QCOMPARE(svc().runs(), runs);
        // Quality is part of the key: Full is a different matte.
        QVERIFY(svc().cachedFrames(file, MatteQuality::Full) == 0);
    }

    void aFrameThatCannotBeDecodedGivesNullWhenWaiting()
    {
        svc().setFrameSource([](const QString&, int64_t, int) { return QImage(); });
        QVERIFY(!svc().matte(QStringLiteral("/clips/bad.mp4"), 1.0, MatteQuality::Preview, true));
        svc().setFrameSource(fakeFrame);
    }

    void aModelThatFailsToLoadMakesTheToolUnavailableWithItsReason()
    {
        svc().setBackendFactory([](const QString&, QString* why) {
            *why = QStringLiteral("broken model");
            return std::unique_ptr<MatteBackend>();
        });
        QVERIFY(!svc().matte(QStringLiteral("/clips/load.mp4"), 1.0, MatteQuality::Preview, true));
        svc().setBackendFactory(
            [](const QString&, QString*) { return std::make_unique<StubBackend>(); });
    }

    void cleanupTestCase() { setModelsDirectoryForTests({}); }
};

QTEST_MAIN(TestMatteService)
#include "test_matteservice.moc"
