// SPDX-License-Identifier: GPL-3.0-or-later
// MemoryGuard: /proc/meminfo parsing, the pause/resume hysteresis, the waiting loop and the
// marker of a render that never finished.
#include "core/MemoryGuard.h"

#include <QtTest>

using namespace kk::memguard;

class TestMemoryGuard : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void parsesMeminfo()
    {
        const QByteArray text
            = "MemTotal:       16000000 kB\nMemFree: 1 kB\nMemAvailable:    2048 kB\n";
        QCOMPARE(availableBytesFromMeminfo(text), int64_t(2048) * 1024);
        QCOMPARE(availableBytesFromMeminfo("MemTotal: 1 kB\n"), int64_t(-1));
    }

    void hysteresis()
    {
        QVERIFY(!shouldPause(kPauseBytes + 1, false));
        QVERIFY(shouldPause(kPauseBytes - 1, false));
        QVERIFY(shouldPause(kResumeBytes - 1, true)); // still paused between the two limits
        QVERIFY(!shouldPause(kResumeBytes, true));
        QVERIFY(!shouldPause(-1, false)); // unknown: never pause
    }

    void waitsUntilMemoryReturns()
    {
        int calls = 0;
        QList<bool> changes;
        const bool ok
            = waitForMemory([] { return false; }, [&](bool paused) { changes.push_back(paused); },
                [&]() -> int64_t { return ++calls < 3 ? (100LL << 20) : (4LL << 30); });
        QVERIFY(ok);
        QCOMPARE(changes, (QList<bool> {true, false}));
    }

    void cancelEndsTheWait()
    {
        QVERIFY(!waitForMemory([] { return true; }, {}, [] { return int64_t(1 << 20); }));
        QVERIFY(waitForMemory([] { return true; }, {}, [] { return int64_t(8) << 30; }));
    }

    void unfinishedRenderIsReported()
    {
        QTemporaryDir dir;
        const QString marker = dir.filePath(QStringLiteral("m.txt"));
        QCOMPARE(takeInterruptedRender(marker), QString());
        QFile f(marker);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("999999999\n/tmp/out.mp4"); // a process id that does not exist
        f.close();
        QCOMPARE(takeInterruptedRender(marker), QStringLiteral("/tmp/out.mp4"));
        QVERIFY(!QFile::exists(marker)); // reported once
    }
};

QTEST_APPLESS_MAIN(TestMemoryGuard)
#include "test_memoryguard.moc"
