// SPDX-License-Identifier: GPL-3.0-or-later
// Subtitle export from Deliver: captions burnt into the picture, written as SRT / WebVTT files
// next to the render, or embedded as subtitle streams (MP4 mov_text, MKV subrip / ass, WebM
// webvtt) - checked by decoding the frames and probing / extracting the streams with the
// ffprobe / ffmpeg command-line tools.
#include "ExportTestUtil.h"
#include "core/Subtitles.h"
#include "export/ExportJob.h"
#include "export/ExportSettings.h"
#include "render/Compositor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

using namespace kk;
using namespace kk::test;

namespace {

/// Runs @p program with @p args; stdout, or a null array when it fails.
QByteArray runTool(const QString& program, const QStringList& args)
{
    const QString exe = QStandardPaths::findExecutable(program);
    if (exe.isEmpty())
        return {};
    QProcess p;
    p.start(exe, args);
    if (!p.waitForFinished(60000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return {};
    QByteArray out = p.readAllStandardOutput();
    return out.isNull() ? QByteArray("") : out;
}

/// Subtitle streams of @p path as reported by ffprobe: [{codec_name, tags: {language}}].
QJsonArray probeSubtitleStreams(const QString& path)
{
    const QByteArray json = runTool(QStringLiteral("ffprobe"),
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
            QStringLiteral("s"), QStringLiteral("-show_streams"), QStringLiteral("-of"),
            QStringLiteral("json"), path});
    return QJsonDocument::fromJson(json).object().value(QStringLiteral("streams")).toArray();
}

/// Subtitle stream @p index of @p path converted to SRT by ffmpeg.
QByteArray extractSrt(const QString& path, int index)
{
    return runTool(QStringLiteral("ffmpeg"),
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
            QStringLiteral("-map"), QStringLiteral("0:s:%1").arg(index), QStringLiteral("-f"),
            QStringLiteral("srt"), QStringLiteral("-")});
}

/// Bright pixels in the lower third of @p image (white caption text on a dark picture).
int brightLowerPixels(const QImage& image)
{
    int count = 0;
    for (int y = image.height() * 2 / 3; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            count += qGray(image.pixel(x, y)) > 200 ? 1 : 0;
    return count;
}

} // namespace

class TestSubtitleExport : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Compositor> m_compositor;
    std::shared_ptr<const TimelineData> m_timeline;

    ExportResult render(const ExportSettings& settings)
    {
        ExportJob job(settings, m_timeline, std::make_shared<MediaPathMap>());
        job.setCompositor(m_compositor.get());
        return job.run();
    }

    ExportSettings settings(const QString& container, const QString& name)
    {
        ExportSettings s;
        s.container = container;
        s.normalize();
        s.video.encoder = EncoderBackend::Software;
        s.audio.enabled = false;
        s.outputPath = m_dir.filePath(name);
        s.subtitles.enabled = true;
        return s;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // 320x180, 25 fps, 2 s: a dark solid; ST1 with two captions (one multi-line, UTF-8),
        // ST2 ("de") with one; ST3 hidden (never exported).
        TimelineData tl = emptyTimeline(320, 180);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(20, 20, 30), 0, 50));
        for (int i = 0; i < 3; ++i) {
            TrackData t = subtitles::makeSubtitleTrack();
            t.id = tl.allocateId();
            tl.subtitleTracks.push_back(t);
        }
        auto caption = [&](int track, const QString& text, int64_t start, int64_t duration) {
            ClipData c = subtitles::makeCaption(text, start, duration);
            c.id = tl.allocateId();
            tl.subtitleTracks[size_t(track)].clips.push_back(c);
        };
        caption(0, QStringLiteral("Hello"), 5, 15);
        caption(0, QStringLiteral("Zweite Zeile\nÜmlaut äö"), 25, 20);
        caption(1, QStringLiteral("Hallo"), 10, 10);
        caption(2, QStringLiteral("hidden"), 0, 10);
        tl.subtitleTracks[1].language = QStringLiteral("deu");
        tl.subtitleTracks[2].enabled = false;
        tl.subtitleTracks[0].subtitleStyle.setBase(keys::Size, 30);
        m_timeline = std::make_shared<const TimelineData>(tl);
        m_compositor = std::make_unique<Compositor>();
    }

    void cleanupTestCase() { m_compositor.reset(); }

    void settingsRoundTrip()
    {
        ExportSettings s;
        s.subtitles.enabled = true;
        s.subtitles.mode = SubtitleExportMode::Embedded;
        s.subtitles.sidecarFormat = SubtitleFormat::WebVtt;
        s.subtitles.embeddedCodec = QStringLiteral("ass");
        QCOMPARE(ExportSettings::fromJson(s.toJson(), ExportSettings {}), s);
        // Older presets without "subtitles" keep the default (off).
        QJsonObject old = s.toJson();
        old.remove(QStringLiteral("subtitles"));
        QVERIFY(!ExportSettings::fromJson(old, ExportSettings {}).subtitles.enabled);
        s.container = QStringLiteral("mkv");
        QCOMPARE(s.embeddedSubtitleCodec(), QStringLiteral("ass"));
        s.container = QStringLiteral("mp4");
        QCOMPARE(s.embeddedSubtitleCodec(), QStringLiteral("mov_text")); // ass not in MP4
        s.container = QStringLiteral("png_seq");
        QVERIFY(s.embeddedSubtitleCodec().isEmpty());
    }

    void sidecarFiles_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("srt") << int(SubtitleFormat::Srt);
        QTest::newRow("vtt") << int(SubtitleFormat::WebVtt);
    }
    void sidecarFiles()
    {
        QFETCH(int, format);
        const auto f = SubtitleFormat(format);
        ExportSettings s = settings(QStringLiteral("mp4"), QStringLiteral("side.mp4"));
        s.subtitles.mode = SubtitleExportMode::Sidecar;
        s.subtitles.sidecarFormat = f;
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        const QString ext = subtitles::suffix(f);
        // Two enabled tracks with captions: numbered / language-named files.
        const QString first = m_dir.filePath(QStringLiteral("side.1.") + ext);
        const QString second = m_dir.filePath(QStringLiteral("side.deu.") + ext);
        QVERIFY(r.files.contains(first));
        QVERIFY(r.files.contains(second));
        QVector<SubtitleCue> cues;
        QString error;
        QVERIFY(subtitles::readFile(first, &cues, &error));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0], (SubtitleCue {200'000, 800'000, QStringLiteral("Hello")}));
        QCOMPARE(cues[1].text, QStringLiteral("Zweite Zeile\nÜmlaut äö"));
        QCOMPARE(cues[1].startUs, 1'000'000);
        QFile file(first);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.contains(f == SubtitleFormat::Srt ? "00:00:00,200 --> 00:00:00,800"
                                                        : "00:00:00.200 --> 00:00:00.800"));
        QVERIFY(!bytes.contains("hidden"));
        // The movie itself has no subtitle stream and no burnt-in text.
        if (!QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
            QCOMPARE(probeSubtitleStreams(s.outputPath).size(), 0);
        QCOMPARE(brightLowerPixels(decodeAt(s.outputPath, 400'000)), 0);
    }

    void sidecarRange()
    {
        // In/out range: cues are clipped and timed from the first rendered frame.
        ExportSettings s = settings(QStringLiteral("mp4"), QStringLiteral("range.mp4"));
        s.subtitles.mode = SubtitleExportMode::Sidecar;
        s.range = RangeMode::Custom;
        s.rangeIn = 10;
        s.rangeOut = 30;
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVector<SubtitleCue> cues;
        QVERIFY(subtitles::readFile(m_dir.filePath(QStringLiteral("range.1.srt")), &cues, nullptr));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0], (SubtitleCue {0, 400'000, QStringLiteral("Hello")}));
        QCOMPARE(cues[1].startUs, 600'000);
        QCOMPARE(cues[1].endUs, 800'000);
    }

    void burnIn()
    {
        ExportSettings s = settings(QStringLiteral("mp4"), QStringLiteral("burn.mp4"));
        s.subtitles.mode = SubtitleExportMode::BurnIn;
        ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.files.size(), 1); // no sidecar
        QVERIFY(brightLowerPixels(decodeAt(s.outputPath, 400'000)) > 20); // "Hello" (+ "Hallo")
        QCOMPARE(brightLowerPixels(decodeAt(s.outputPath, 0)), 0); // ST3 is hidden
        // Export subtitle off: nothing burnt in although the tracks are visible in the viewer.
        s.subtitles.enabled = false;
        s.outputPath = m_dir.filePath(QStringLiteral("plain.mp4"));
        r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(brightLowerPixels(decodeAt(s.outputPath, 400'000)), 0);
    }

    void embedded_data()
    {
        QTest::addColumn<QString>("container");
        QTest::addColumn<QString>("codec");
        QTest::addColumn<QString>("probed");
        QTest::newRow("mp4") << "mp4" << "" << "mov_text";
        QTest::newRow("mov") << "mov" << "" << "mov_text";
        QTest::newRow("mkv srt") << "mkv" << "subrip" << "subrip";
        QTest::newRow("mkv ass") << "mkv" << "ass" << "ass";
        QTest::newRow("webm") << "webm" << "" << "webvtt";
    }
    void embedded()
    {
        QFETCH(QString, container);
        QFETCH(QString, codec);
        QFETCH(QString, probed);
        if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffprobe / ffmpeg not installed");
        ExportSettings s = settings(container, QStringLiteral("embed_%1_%2").arg(container, codec));
        s.outputPath = withContainerExtension(s.outputPath, container);
        s.subtitles.mode = SubtitleExportMode::Embedded;
        s.subtitles.embeddedCodec = codec;
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        const QJsonArray streams = probeSubtitleStreams(s.outputPath);
        QCOMPARE(streams.size(), 2);
        for (const QJsonValue& v : streams)
            QCOMPARE(v.toObject().value(QStringLiteral("codec_name")).toString(), probed);
        QCOMPARE(streams[1]
                     .toObject()
                     .value(QStringLiteral("tags"))
                     .toObject()
                     .value(QStringLiteral("language"))
                     .toString(),
            container == QLatin1String("mov") ? QStringLiteral("ger") : QStringLiteral("deu"));
        // The captions come back with their timing and text (UTF-8, two lines).
        QVector<SubtitleCue> cues;
        QString error;
        QVERIFY2(subtitles::parse(extractSrt(s.outputPath, 0), &cues, &error), qPrintable(error));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].text, QStringLiteral("Hello"));
        QVERIFY(std::abs(cues[0].startUs - 200'000) <= 1000);
        QVERIFY(std::abs(cues[0].endUs - 800'000) <= 1000);
        QCOMPARE(cues[1].text, QStringLiteral("Zweite Zeile\nÜmlaut äö"));
        QVERIFY(subtitles::parse(extractSrt(s.outputPath, 1), &cues, &error));
        QCOMPARE(cues.size(), 1);
        QCOMPARE(cues[0].text, QStringLiteral("Hallo"));
        // No burn-in.
        QCOMPARE(brightLowerPixels(decodeAt(s.outputPath, 400'000)), 0);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TestSubtitleExport test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_subtitleexport.moc"
