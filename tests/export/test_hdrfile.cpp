// SPDX-License-Identifier: GPL-3.0-or-later
// HDR export end to end (1.13): renders a short H.265 file through ExportJob and checks the
// written file with ffprobe: Rec.2020 primaries, PQ / HLG transfer, 10-bit pixel format and,
// for HDR10, the mastering display and content light level side data.
#include "ExportTestUtil.h"
#include "export/ExportJob.h"
#include "export/ExportSettings.h"
#include "render/Compositor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace kk;
using namespace kk::test;

namespace {

/// The first video stream of @p path (with frame side data) as reported by ffprobe.
QJsonObject probeVideo(const QString& path)
{
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (exe.isEmpty())
        return {};
    QProcess p;
    p.start(exe,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
            QStringLiteral("v:0"), QStringLiteral("-show_streams"), QStringLiteral("-show_frames"),
            QStringLiteral("-read_intervals"), QStringLiteral("%+#1"), QStringLiteral("-of"),
            QStringLiteral("json"), path});
    if (!p.waitForFinished(60000) || p.exitCode() != 0)
        return {};
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    QJsonObject stream = o.value(QStringLiteral("streams")).toArray().at(0).toObject();
    stream[QStringLiteral("frame_side_data")]
        = o.value(QStringLiteral("frames")).toArray().at(0).toObject().value(
            QStringLiteral("side_data_list"));
    return stream;
}

bool hasSideData(const QJsonObject& stream, const QString& type)
{
    for (const QString& key : {QStringLiteral("side_data_list"), QStringLiteral("frame_side_data")}) {
        for (const QJsonValue& v : stream.value(key).toArray()) {
            if (v.toObject().value(QStringLiteral("side_data_type")).toString() == type)
                return true;
        }
    }
    return false;
}

/// True when this FFmpeg build has the encoder; the file tests are skipped only for that reason.
bool hasEncoder(const char* name)
{
    return avcodec_find_encoder_by_name(name) != nullptr;
}

} // namespace

class TestHdrFile : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Compositor> m_compositor;
    std::shared_ptr<const TimelineData> m_timeline;

    ExportResult render(const ExportSettings& s)
    {
        ExportJob job(s, m_timeline, std::make_shared<MediaPathMap>());
        job.setCompositor(m_compositor.get());
        return job.run();
    }

    ExportSettings settings(const QString& codec, GammaTag gamma, const QString& name)
    {
        ExportSettings s;
        s.container = name.endsWith(QLatin1String(".mov")) ? QStringLiteral("mov")
                                                           : QStringLiteral("mp4");
        s.video.codec = codec;
        s.video.colorSpace = ColorSpaceTag::Rec2020;
        s.video.gamma = gamma;
        s.normalize();
        s.audio.enabled = false;
        s.outputPath = m_dir.filePath(name);
        return s;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        TimelineData tl = emptyTimeline(320, 180);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(120, 120, 120), 0, 25));
        m_timeline = std::make_shared<const TimelineData>(tl);
        m_compositor = std::make_unique<Compositor>();
    }

    void cleanupTestCase() { m_compositor.reset(); }

    void hdr10File()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QSKIP("ffprobe not installed");
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        ExportSettings s = settings(QStringLiteral("hevc"), GammaTag::Pq, QStringLiteral("a.mp4"));
        s.video.encoder = EncoderBackend::Nvenc; // normalize() turns this into Software
        s.normalize();
        QCOMPARE(s.video.encoder, EncoderBackend::Software);
        s.video.hdr.maxCll = 900;
        s.video.hdr.maxFall = 300;
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        const QJsonObject v = probeVideo(s.outputPath);
        QVERIFY(!v.isEmpty());
        qInfo().noquote() << QJsonDocument(v).toJson(QJsonDocument::Indented);
        qInfo().noquote() << QStringLiteral("ffprobe: codec=%1 pix_fmt=%2 primaries=%3 transfer=%4 space=%5")
                                 .arg(v.value(QStringLiteral("codec_name")).toString(),
                                     v.value(QStringLiteral("pix_fmt")).toString(),
                                     v.value(QStringLiteral("color_primaries")).toString(),
                                     v.value(QStringLiteral("color_transfer")).toString(),
                                     v.value(QStringLiteral("color_space")).toString());
        QCOMPARE(v.value(QStringLiteral("codec_name")).toString(), QStringLiteral("hevc"));
        QCOMPARE(v.value(QStringLiteral("pix_fmt")).toString(), QStringLiteral("yuv420p10le"));
        QCOMPARE(v.value(QStringLiteral("color_primaries")).toString(), QStringLiteral("bt2020"));
        QCOMPARE(v.value(QStringLiteral("color_transfer")).toString(), QStringLiteral("smpte2084"));
        QCOMPARE(v.value(QStringLiteral("color_space")).toString(), QStringLiteral("bt2020nc"));
        QVERIFY(hasSideData(v, QStringLiteral("Mastering display metadata")));
        QVERIFY(hasSideData(v, QStringLiteral("Content light level metadata")));
    }

    void hlgFile()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QSKIP("ffprobe not installed");
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        const ExportSettings s
            = settings(QStringLiteral("hevc"), GammaTag::Hlg, QStringLiteral("b.mp4"));
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        const QJsonObject v = probeVideo(s.outputPath);
        QCOMPARE(v.value(QStringLiteral("pix_fmt")).toString(), QStringLiteral("yuv420p10le"));
        QCOMPARE(v.value(QStringLiteral("color_primaries")).toString(), QStringLiteral("bt2020"));
        QCOMPARE(v.value(QStringLiteral("color_transfer")).toString(),
            QStringLiteral("arib-std-b67"));
        QVERIFY(!hasSideData(v, QStringLiteral("Mastering display metadata")));
    }

    void proresHdrFile()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QSKIP("ffprobe not installed");
        if (!hasEncoder("prores_ks"))
            QSKIP("prores_ks is not in this FFmpeg build");
        for (const char* profile : {"hq", "4444"}) {
            ExportSettings s = settings(QStringLiteral("prores"), GammaTag::Pq,
                QStringLiteral("p_%1.mov").arg(QLatin1String(profile)));
            s.video.profile = QLatin1String(profile);
            s.video.hdr.primaries = MasterPrimaries::Rec2020;
            s.normalize();
            QVERIFY(s.video.isHdr());
            QCOMPARE(s.video.profile, QLatin1String(profile));
            const ExportResult r = render(s);
            QVERIFY2(r.ok, qPrintable(r.error));
            const QJsonObject v = probeVideo(s.outputPath);
            QCOMPARE(v.value(QStringLiteral("codec_name")).toString(), QStringLiteral("prores"));
            // 10-bit 4:2:2; ffprobe reports the decoder's format for 4444 (12-bit).
            const QString fmt = v.value(QStringLiteral("pix_fmt")).toString();
            QVERIFY2(fmt.contains(QStringLiteral("10le")) || fmt.contains(QStringLiteral("12le")),
                qPrintable(fmt));
            QCOMPARE(v.value(QStringLiteral("color_primaries")).toString(), QStringLiteral("bt2020"));
            QCOMPARE(v.value(QStringLiteral("color_transfer")).toString(),
                QStringLiteral("smpte2084"));
            QCOMPARE(v.value(QStringLiteral("color_space")).toString(), QStringLiteral("bt2020nc"));
            QVERIFY(hasSideData(v, QStringLiteral("Mastering display metadata")));
            QVERIFY(hasSideData(v, QStringLiteral("Content light level metadata")));
        }
        // HLG in ProRes carries the tags and no HDR10 metadata.
        ExportSettings hlg = settings(QStringLiteral("prores"), GammaTag::Hlg, QStringLiteral("h.mov"));
        hlg.normalize();
        QVERIFY2(render(hlg).ok, "prores hlg");
        const QJsonObject h = probeVideo(hlg.outputPath);
        QCOMPARE(h.value(QStringLiteral("color_transfer")).toString(), QStringLiteral("arib-std-b67"));
        QVERIFY(!hasSideData(h, QStringLiteral("Mastering display metadata")));
    }

    void masteringDisplayPrimariesInTheFile()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QSKIP("ffprobe not installed");
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        for (const MasterPrimaries mp : {MasterPrimaries::P3D65, MasterPrimaries::Rec2020}) {
            ExportSettings s = settings(QStringLiteral("hevc"), GammaTag::Pq,
                QStringLiteral("m_%1.mp4").arg(masterPrimariesId(mp)));
            s.video.hdr.primaries = mp;
            QVERIFY2(render(s).ok, "render");
            const QJsonObject v = probeVideo(s.outputPath);
            QString red;
            for (const QJsonValue& sd : v.value(QStringLiteral("frame_side_data")).toArray()) {
                if (sd.toObject().value(QStringLiteral("side_data_type")).toString()
                    == QLatin1String("Mastering display metadata"))
                    red = sd.toObject().value(QStringLiteral("red_x")).toString();
            }
            // 0.680 = 34000/50000 (P3), 0.708 = 35400/50000 (BT.2020)
            QVERIFY2(!red.isEmpty(), "no mastering display data");
            QCOMPARE(red, mp == MasterPrimaries::Rec2020 ? QStringLiteral("35400/50000")
                                                         : QStringLiteral("34000/50000"));
        }
    }

    void sdrFileStaysBt709()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QSKIP("ffprobe not installed");
        if (!hasEncoder("libx265"))
            QSKIP("libx265 is not in this FFmpeg build");
        ExportSettings s;
        s.container = QStringLiteral("mp4");
        s.video.codec = QStringLiteral("hevc");
        s.normalize();
        s.audio.enabled = false;
        s.outputPath = m_dir.filePath(QStringLiteral("c.mp4"));
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        const QJsonObject v = probeVideo(s.outputPath);
        QCOMPARE(v.value(QStringLiteral("pix_fmt")).toString(), QStringLiteral("yuv420p"));
        QVERIFY(v.value(QStringLiteral("color_transfer")).toString() != QLatin1String("smpte2084"));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TestHdrFile test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_hdrfile.moc"
