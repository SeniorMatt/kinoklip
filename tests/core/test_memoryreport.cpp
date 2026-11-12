// SPDX-License-Identifier: GPL-3.0-or-later
// The memory numbers of the log file (spec 85.8): DRM fdinfo sums and the /proc lines.
#include "core/MemoryReport.h"

#include <QtTest>

using namespace kk;

class TestMemoryReport : public QObject
{
    Q_OBJECT

private slots:
    void drmRegionsAreSummedPerClient()
    {
        const QByteArray a = "pos:\t0\ndrm-driver:\ti915\ndrm-client-id:\t7\n"
                             "drm-total-system:\t2048 KiB\ndrm-resident-system:\t1 MiB\n";
        const QByteArray sameClient = "drm-driver:\ti915\ndrm-client-id:\t7\n"
                                      "drm-total-system:\t2048 KiB\n";
        const QByteArray b = "drm-driver:\ti915\ndrm-client-id:\t9\ndrm-total-system:\t3 MiB\n";
        const QByteArray notDrm = "pos:\t0\nflags:\t02\n";
        const QStringList lines = memreport::drmLines({a, sameClient, b, notDrm});
        QVERIFY(lines.contains(QStringLiteral("drm i915 total-system: 5 MiB")));
        QVERIFY(lines.contains(QStringLiteral("drm i915 resident-system: 1 MiB")));
        QCOMPARE(lines.size(), 2);
    }

    void procLinesAreThere()
    {
#ifdef Q_OS_LINUX
        const QString text = memreport::lines().join(QLatin1Char('\n'));
        QVERIFY(text.contains(QStringLiteral("RSS: ")));
        QVERIFY(text.contains(QStringLiteral("RssShmem: ")));
        QVERIFY(text.contains(QStringLiteral("MemAvailable: ")));
#endif
    }
};

QTEST_GUILESS_MAIN(TestMemoryReport)
#include "test_memoryreport.moc"
