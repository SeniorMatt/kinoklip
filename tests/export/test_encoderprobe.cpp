// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware encoder detection is quiet: probing every hardware encoder (NVENC, VAAPI, QSV) logs no
// FFmpeg error and no warning at all - whatever this machine has - and exactly one summary line.
#include "export/EncoderCatalog.h"
#include "media/HwAccel.h"

#include <QTest>

#include <mutex>

using namespace kk;

namespace {

std::mutex g_mutex;
QStringList g_warnings; ///< kk.* warnings or worse
QStringList g_infos; ///< kk.export info lines
QtMessageHandler g_previous = nullptr;

void handler(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    const QByteArray category = context.category ? QByteArray(context.category) : QByteArray();
    {
        const std::lock_guard lock(g_mutex);
        if (category.startsWith("kk.") && type != QtDebugMsg && type != QtInfoMsg) {
            g_warnings << QStringLiteral("%1: %2").arg(QString::fromLatin1(category), text);
        }
        if (category == "kk.export" && type == QtInfoMsg) {
            g_infos << text;
        }
    }
    if (g_previous) {
        g_previous(type, context, text);
    }
}

} // namespace

class TestEncoderProbe : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void probingIsQuiet()
    {
        qunsetenv("KINOKLIP_HWENC");
        g_previous = qInstallMessageHandler(handler);
        const QList<HwEncoderStatus> all = EncoderCatalog::hardwareEncoders();
        (void)EncoderCatalog::hardwareEncoders(); // cached: no second summary
        (void)EncoderCatalog::candidates(QStringLiteral("h264"), EncoderBackend::Auto);
        qInstallMessageHandler(g_previous);

        QVERIFY(!all.isEmpty());
        for (const HwEncoderStatus& status : all) {
            qInfo("%s: %s", qPrintable(status.encoderName),
                qPrintable(status.available ? status.device : status.error));
            if (status.backend == EncoderBackend::Nvenc && !media::nvidiaDriverPresent()) {
                QCOMPARE(status.error, QStringLiteral("no NVIDIA driver"));
            }
            if (status.backend == EncoderBackend::Qsv && !media::qsvRuntimePresent()) {
                QVERIFY(status.error.startsWith(QLatin1String("no Intel GPU")));
            }
        }
        const std::lock_guard lock(g_mutex);
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(u'\n')));
        int summaries = 0;
        for (const QString& line : g_infos) {
            summaries += line.startsWith(QLatin1String("hardware encoders:")) ? 1 : 0;
        }
        QCOMPARE(summaries, 1);
    }
};

QTEST_GUILESS_MAIN(TestEncoderProbe)
#include "test_encoderprobe.moc"
