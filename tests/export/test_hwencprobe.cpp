// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware encoder detection runs in a process of its own (spec 160, issue 18): a probe that
// crashes, hangs or exits badly makes the encoder unavailable with a reason and is remembered on
// disk for this GPU and driver; KINOKLIP_HWENC=off and KINOKLIP_HWACCEL=off probe nothing.
#include "export/EncoderCatalog.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

#ifndef KK_HWENC_PROBE_PATH
#define KK_HWENC_PROBE_PATH ""
#endif

namespace {

HwEncoderStatus find(const QList<HwEncoderStatus>& all, const QString& name)
{
    for (const HwEncoderStatus& s : all) {
        if (s.encoderName == name)
            return s;
    }
    return {};
}

} // namespace

class TestHwEncProbe : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

    void fresh()
    {
        qunsetenv("KINOKLIP_HWENC");
        qunsetenv("KINOKLIP_HWACCEL");
        qunsetenv("KK_HWENC_PROBE_TEST");
        EncoderCatalog::setProbeProgram(QString::fromUtf8(KK_HWENC_PROBE_PATH));
        EncoderCatalog::setProbeTimeoutMs(10000);
        EncoderCatalog::setCacheFile(m_dir.filePath(QStringLiteral("hwenc.json")));
        EncoderCatalog::retest();
    }

private Q_SLOTS:
    void init() { fresh(); }
    void cleanup()
    {
        qunsetenv("KK_HWENC_PROBE_TEST");
        qunsetenv("KINOKLIP_HWENC");
        qunsetenv("KINOKLIP_HWACCEL");
    }

    /// Nothing is tested until somebody asks.
    void nothingIsTestedByItself()
    {
        QVERIFY(EncoderCatalog::cachedEncoders().isEmpty());
        QVERIFY(!EncoderCatalog::probeWasRun());
    }

    /// The probe program crashes: unavailable with the reason, no crash here.
    void aCrashingDriverMarksTheEncoderUnavailable()
    {
        if (!QFile::exists(QString::fromUtf8(KK_HWENC_PROBE_PATH)))
            QSKIP("kinoklip-hwenc-probe is not built");
        qputenv("KK_HWENC_PROBE_TEST", "crash");
        // An encoder that passes the quick checks; software FFmpeg builds without VAAPI skip.
        const QList<HwEncoderStatus> all = EncoderCatalog::hardwareEncoders();
        int failed = 0;
        for (const HwEncoderStatus& s : all) {
            QVERIFY(!s.available);
            if (s.probeFailed) {
                ++failed;
                QVERIFY2(s.error.contains(QLatin1String("crashed")), qPrintable(s.error));
            }
        }
        if (failed == 0)
            QSKIP("this machine has no hardware encoder candidate to test");
        QVERIFY(EncoderCatalog::probeWasRun());
        // The answer is on disk: another "run" (memory cleared) does not start the probe again.
        EncoderCatalog::resetMemory();
        qputenv("KK_HWENC_PROBE_TEST", "none"); // would succeed or fail differently if it ran
        const QList<HwEncoderStatus> again = EncoderCatalog::cachedEncoders();
        QCOMPARE(again.size(), all.size());
        for (const HwEncoderStatus& s : again) {
            QVERIFY(!s.available);
        }
    }

    /// The probe program hangs: killed at the time limit, unavailable, quick.
    void aHangingDriverIsKilledAtTheTimeLimit()
    {
        if (!QFile::exists(QString::fromUtf8(KK_HWENC_PROBE_PATH)))
            QSKIP("kinoklip-hwenc-probe is not built");
        qputenv("KK_HWENC_PROBE_TEST", "hang");
        EncoderCatalog::setProbeTimeoutMs(500);
        QElapsedTimer timer;
        timer.start();
        const QList<HwEncoderStatus> all = EncoderCatalog::hardwareEncoders();
        int hung = 0;
        for (const HwEncoderStatus& s : all) {
            if (s.probeFailed) {
                ++hung;
                QVERIFY2(s.error.contains(QLatin1String("did not answer")), qPrintable(s.error));
            }
        }
        if (hung == 0)
            QSKIP("this machine has no hardware encoder candidate to test");
        // Only one VAAPI probe may hang: the others are not even started.
        QVERIFY2(timer.elapsed() < 500 * 6 + 5000, "too many hanging probes were started");
        const HwEncoderStatus h264 = find(all, QStringLiteral("h264_vaapi"));
        const HwEncoderStatus hevc = find(all, QStringLiteral("hevc_vaapi"));
        if (h264.probeFailed && hevc.probeFailed) {
            QVERIFY2(hevc.error.contains(QLatin1String("not tested")), qPrintable(hevc.error));
        }
    }

    /// A bad exit code is a failed test too.
    void aBadExitCodeIsAFailedTest()
    {
        if (!QFile::exists(QString::fromUtf8(KK_HWENC_PROBE_PATH)))
            QSKIP("kinoklip-hwenc-probe is not built");
        qputenv("KK_HWENC_PROBE_TEST", "exit");
        bool any = false;
        for (const HwEncoderStatus& s : EncoderCatalog::hardwareEncoders()) {
            if (s.probeFailed) {
                any = true;
                QVERIFY2(s.error.contains(QLatin1String("exit code 7")), qPrintable(s.error));
            }
        }
        if (!any)
            QSKIP("this machine has no hardware encoder candidate to test");
    }

    /// A missing probe program: hardware is unavailable, the render uses software, nothing is
    /// written to the disk cache (the program may be installed later).
    void aMissingProbeProgramIsNotRemembered()
    {
        EncoderCatalog::setProbeProgram(m_dir.filePath(QStringLiteral("no-such-program")));
        const QList<EncoderChoice> choices
            = EncoderCatalog::candidates(QStringLiteral("h264"), EncoderBackend::Auto);
        for (const EncoderChoice& c : choices)
            QCOMPARE(c.backend, EncoderBackend::Software);
        QFile f(m_dir.filePath(QStringLiteral("hwenc.json")));
        if (f.open(QIODevice::ReadOnly))
            QVERIFY(!f.readAll().contains("probe program"));
    }

    /// The cache belongs to one GPU, driver and Kinoklip version: another key is ignored.
    void theCacheIsKeyedByTheDriver()
    {
        if (!QFile::exists(QString::fromUtf8(KK_HWENC_PROBE_PATH)))
            QSKIP("kinoklip-hwenc-probe is not built");
        QVERIFY(!EncoderCatalog::driverKey().isEmpty());
        QCOMPARE(EncoderCatalog::driverKey(), EncoderCatalog::driverKey());
        qputenv("KK_HWENC_PROBE_TEST", "crash");
        (void)EncoderCatalog::hardwareEncoders();
        const QString path = m_dir.filePath(QStringLiteral("hwenc.json"));
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly))
            QSKIP("nothing was cached on this machine");
        QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
        QCOMPARE(root["key"].toString(), EncoderCatalog::driverKey());
        // A driver update (another key): the old answers are not used.
        root["key"] = QStringLiteral("another driver");
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(QJsonDocument(root).toJson());
        out.close();
        EncoderCatalog::resetMemory();
        QVERIFY(EncoderCatalog::cachedEncoders().isEmpty());
    }

    /// "Test again" forgets the answers and the disk cache.
    void retestForgetsTheAnswers()
    {
        if (!QFile::exists(QString::fromUtf8(KK_HWENC_PROBE_PATH)))
            QSKIP("kinoklip-hwenc-probe is not built");
        qputenv("KK_HWENC_PROBE_TEST", "crash");
        (void)EncoderCatalog::hardwareEncoders();
        EncoderCatalog::retest();
        QVERIFY(EncoderCatalog::cachedEncoders().isEmpty());
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("hwenc.json"))));
    }

    /// KINOKLIP_HWENC=off and KINOKLIP_HWACCEL=off: no hardware encoder, no probe program.
    void theEnvironmentSwitchesHardwareOff()
    {
        for (const char* name : {"KINOKLIP_HWENC", "KINOKLIP_HWACCEL"}) {
            fresh();
            qputenv("KK_HWENC_PROBE_TEST", "crash"); // would be visible if it ran
            qputenv(name, "off");
            QVERIFY(EncoderCatalog::hardwareDisabledByEnvironment());
            const QList<HwEncoderStatus> all = EncoderCatalog::hardwareEncoders();
            QVERIFY(!all.isEmpty());
            for (const HwEncoderStatus& s : all) {
                QVERIFY(!s.available);
                QVERIFY(!s.probeFailed);
                QVERIFY2(s.error.contains(QLatin1String(name)), qPrintable(s.error));
            }
            for (const EncoderChoice& c :
                EncoderCatalog::candidates(QStringLiteral("h264"), EncoderBackend::Auto))
                QCOMPARE(c.backend, EncoderBackend::Software);
            QVERIFY(!EncoderCatalog::probeWasRun());
            qunsetenv(name);
        }
    }
};

QTEST_GUILESS_MAIN(TestHwEncProbe)
#include "test_hwencprobe.moc"
