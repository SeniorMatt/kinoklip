// SPDX-License-Identifier: GPL-3.0-or-later
// test_realworld — the files beginners throw at an editor (bug hunt round 11, spec section 46):
// phone footage (portrait rotation, VFR, HEVC 10-bit, 4K60, HDR HLG / PQ), camera formats,
// screen recordings, audio (44.1 kHz, mono, 5.1, 24-bit, cover art, two streams), pictures and
// broken or awkwardly named files. Each one goes through import, decoding, thumbnails, audio
// and waveform, export and proxy generation (fixtures: scripts/make_realworld_media.sh).
#include "../export/ExportTestUtil.h"
#include "core/Project.h"
#include "export/ExportJob.h"
#include "export/ProxyMedia.h"
#include "export/ProxyQueue.h"
#include "media/AudioDecoder.h"
#include "media/HdrToSdr.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"
#include "media/WaveformData.h"
#include "render/Compositor.h"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kJobTimeoutMs = 120'000;

QString realWorld(const QString& name)
{
    return fixtureDir() + QStringLiteral("/realworld/") + name;
}

/// The colours at the quadrant centres (TL, TR, BL, BR) of the quadrant picture.
struct Corners
{
    QRgb tl, tr, bl, br;
};

const QRgb kRed = qRgb(255, 0, 0), kLime = qRgb(0, 255, 0), kBlue = qRgb(0, 0, 255),
           kWhite = qRgb(255, 255, 255);

/// Empty when the quadrant centres of @p image show @p expected (lossy-codec tolerance).
QString checkCorners(const QImage& image, const Corners& expected)
{
    if (image.isNull())
        return QStringLiteral("null image");
    const int w = image.width(), h = image.height();
    const QPoint points[4]
        = {{w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4}};
    const QRgb want[4] = {expected.tl, expected.tr, expected.bl, expected.br};
    for (int i = 0; i < 4; ++i) {
        const QRgb got = image.pixel(points[i]);
        if (std::abs(qRed(got) - qRed(want[i])) > 60 || std::abs(qGreen(got) - qGreen(want[i])) > 60
            || std::abs(qBlue(got) - qBlue(want[i])) > 60)
            return QStringLiteral("corner %1 is #%2, expected #%3")
                .arg(i)
                .arg(got & 0xffffff, 6, 16, QLatin1Char('0'))
                .arg(want[i] & 0xffffff, 6, 16, QLatin1Char('0'));
    }
    return {};
}

/// Imports @p path; returns the item (empty id on failure; @p result filled either way).
MediaItem importOne(Project& project, const QString& path, ImportResult* result = nullptr)
{
    const ImportResult r = project.importMedia({path});
    if (result)
        *result = r;
    if (r.importedIds.size() != 1)
        return {};
    return *project.mediaPool()->item(r.importedIds.front());
}

/// A timeline of @p item alone (its video and audio), at its own size and frame rate.
std::shared_ptr<const TimelineData> clipTimeline(const MediaItem& item)
{
    const media::MediaInfo& i = item.info;
    const Rational fps = i.frameRate.isValid() ? i.frameRate : Rational {25, 1};
    TimelineData tl
        = emptyTimeline(i.hasVideo ? i.displayWidth : 320, i.hasVideo ? i.displayHeight : 180, fps);
    const int64_t frames = std::max<int64_t>(1, i.durationUs * fps.num / (fps.den * 1'000'000LL));
    if (i.hasVideo)
        addClip(tl, TrackType::Video, 0, mediaClip(tl, item.id, 0, frames, 0, i.durationUs));
    if (i.hasAudio)
        addClip(tl, TrackType::Audio, 0, mediaClip(tl, item.id, 0, frames, 0, i.durationUs));
    return snap(tl);
}

/// Renders @p item's one-clip timeline to an H.264 / AAC MP4 at @p outputPath.
ExportResult exportItem(
    Project& project, const MediaItem& item, const QString& outputPath, Compositor* compositor)
{
    ExportSettings s;
    s.outputPath = outputPath;
    ExportJob job(s, clipTimeline(item), makeMediaPathMap(project.mediaPool()->state()));
    job.setCompositor(compositor);
    return job.run();
}

/// Generates @p item's proxy into @p folder; returns the proxy's path (empty on failure).
QString generateProxy(
    Project& project, const MediaItem& item, const QString& folder, Compositor* compositor)
{
    ProxyJobSpec spec;
    QString error;
    if (!makeProxyJob(item, LinkKind::Proxy, project.cacheSettings(), folder, &spec, &error)) {
        qWarning() << error;
        return {};
    }
    ProxyQueue queue(project.mediaLinks());
    queue.setCompositor(compositor);
    QSignalSpy finished(&queue, &ProxyQueue::finished);
    if (!queue.enqueue(spec) || !queue.waitForIdle(kJobTimeoutMs) || finished.count() != 1)
        return {};
    if (!finished.front().at(2).toBool()) {
        qWarning() << finished.front().at(3).toString();
        return {};
    }
    return spec.outputPath;
}

/// Decoded 48 kHz stereo audio of @p path over [0, @p frames).
std::vector<float> decodeAudio(const QString& path, int frames)
{
    media::AudioDecoder decoder;
    std::vector<float> buffer(size_t(frames) * 2);
    if (!decoder.open(path) || decoder.read(0, buffer.data(), frames) <= 0)
        return {};
    return buffer;
}

/// Frequency (Hz) of the left channel of 48 kHz stereo @p samples, from its zero crossings.
double frequency(const std::vector<float>& samples)
{
    int crossings = 0;
    for (size_t i = 2; i < samples.size(); i += 2) {
        if ((samples[i - 2] < 0) != (samples[i] < 0))
            ++crossings;
    }
    return samples.empty() ? 0 : crossings / 2.0 / (double(samples.size() / 2) / 48000.0);
}

double rms(const std::vector<float>& samples)
{
    double sum = 0;
    for (float v : samples)
        sum += double(v) * v;
    return samples.empty() ? 0 : std::sqrt(sum / double(samples.size()));
}

} // namespace

Q_DECLARE_METATYPE(Corners)

class TestRealWorld : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_dir;
    QString m_why;
    bool m_ready = false;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_ready = ensureFixtures(&m_why);
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }

    // ---- 46.1 Phone portrait clips (display matrix 90 / 270 / 180) ------------------------
    void phonePortrait_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("rotation");
        QTest::addColumn<Corners>("corners");
        QTest::newRow("90") << QStringLiteral("phone_rot90.mp4") << 360 << 640 << 90
                            << Corners {kBlue, kRed, kWhite, kLime};
        QTest::newRow("270") << QStringLiteral("phone_rot270.mp4") << 360 << 640 << 270
                             << Corners {kLime, kWhite, kRed, kBlue};
        QTest::newRow("180") << QStringLiteral("phone_rot180.mp4") << 640 << 360 << 180
                             << Corners {kWhite, kBlue, kLime, kRed};
    }
    void phonePortrait()
    {
        QFETCH(QString, file);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(int, rotation);
        QFETCH(Corners, corners);
        if (!m_ready || !QFileInfo::exists(realWorld(file)))
            QSKIP("fixture missing");
        Project project;
        const MediaItem item = importOne(project, realWorld(file));
        QVERIFY(!item.id.isEmpty());
        QCOMPARE(item.info.rotation, rotation);
        QCOMPARE(QSize(item.info.displayWidth, item.info.displayHeight), QSize(width, height));
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(realWorld(file)));
        QCOMPARE(checkCorners(decoder.frameAt(1'000'000), corners), QString());
        const QImage thumb = decoder.frameAt(1'000'000, QSize(160, 90));
        QCOMPARE(thumb.width() < thumb.height(), width < height); // upright thumbnail
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            c->setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
            QCOMPARE(checkCorners(c->renderImage(clipTimeline(item), 10), corners), QString());
        }
        const QString out = m_dir.filePath(file + QStringLiteral(".out.mp4"));
        QVERIFY(exportItem(project, item, out, m_cpu.get()).ok);
        const media::MediaInfo rendered = media::probe(out);
        QCOMPARE(QSize(rendered.displayWidth, rendered.displayHeight), QSize(width, height));
        QCOMPARE(checkCorners(decodeAt(out, 1'000'000), corners), QString());
        const QString proxy = generateProxy(project, item, m_dir.filePath("Proxy"), m_cpu.get());
        QVERIFY(!proxy.isEmpty());
        const media::MediaInfo p = media::probe(proxy);
        QCOMPARE(p.displayWidth < p.displayHeight, width < height);
    }

    // ---- 46.2 HDR (HLG / PQ, BT.2020) is shown as SDR BT.709 ------------------------------
    void hdrReferenceMaths()
    {
        using media::HdrMapping;
        // HDR reference white (HLG 75 %, PQ 203 cd/m2) lands near SDR white, not grey.
        const auto hlgWhite = media::mapToSdr({0.75f, 0.75f, 0.75f}, HdrMapping::Hlg);
        const auto pqWhite = media::mapToSdr({0.5806f, 0.5806f, 0.5806f}, HdrMapping::Pq);
        for (float v : {hlgWhite[0], pqWhite[0]})
            QVERIFY2(v > 0.93f && v < 0.98f, qPrintable(QString::number(v)));
        // Black stays black, the HLG / PQ peak stays inside the range.
        QCOMPARE(media::mapToSdr({0, 0, 0}, HdrMapping::Hlg)[1], 0.0f);
        QVERIFY(media::mapToSdr({1, 1, 1}, HdrMapping::Pq)[0] <= 1.0f);
        // The BT.2020 HLG code value of BT.709 green becomes pure green (gamut conversion).
        const auto green = media::mapToSdr({0.5115f, 0.7335f, 0.2646f}, HdrMapping::Hlg);
        QVERIFY(green[0] < 0.1f && green[2] < 0.1f && green[1] > 0.9f);
        // SDR frames are untouched.
        QCOMPARE(media::mapToSdr({0.3f, 0.5f, 0.7f}, HdrMapping::None)[2], 0.7f);
    }
    void hdrIsMappedToSdr_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<bool>("hasGreen"); // right half: BT.709 green coded in BT.2020
        QTest::newRow("hlg") << QStringLiteral("phone_hlg.mp4") << true;
        QTest::newRow("pq") << QStringLiteral("phone_pq.mp4") << false;
    }
    void hdrIsMappedToSdr()
    {
        QFETCH(QString, file);
        QFETCH(bool, hasGreen);
        if (!m_ready || !QFileInfo::exists(realWorld(file)))
            QSKIP("fixture missing");
        // Shown as is, reference white would be grey (HLG 189, PQ 146) and the green dull
        // (130, 185, 65): "washed out".
        const auto check = [&](const QImage& image, const char* where) {
            if (image.isNull())
                return QStringLiteral("%1: null image").arg(QLatin1String(where));
            const QRgb white = image.pixel(image.width() / 4, image.height() / 2);
            if (qGray(white) < 225)
                return QStringLiteral("%1: reference white is %2")
                    .arg(QLatin1String(where))
                    .arg(qGray(white));
            const QRgb g = image.pixel(3 * image.width() / 4, image.height() / 2);
            if (hasGreen && (qGreen(g) < 200 || qRed(g) > 80 || qBlue(g) > 80))
                return QStringLiteral("%1: green is #%2")
                    .arg(QLatin1String(where))
                    .arg(g & 0xffffff, 6, 16, QLatin1Char('0'));
            return QString();
        };
        Project project;
        const MediaItem item = importOne(project, realWorld(file));
        QVERIFY(!item.id.isEmpty());
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(realWorld(file)));
        QCOMPARE(check(decoder.frameAt(500'000), "decoder"), QString());
        QCOMPARE(check(decoder.frameAt(500'000, QSize(160, 90)), "thumbnail"), QString());
        const auto frame = decoder.decodeFrame(500'000);
        QVERIFY(frame);
        QVERIFY(media::hdrMappingOf(*frame) != media::HdrMapping::None);
        if (m_gpu->isGpu()) {
            m_gpu->setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
            QCOMPARE(check(m_gpu->renderImage(clipTimeline(item), 10), "gpu"), QString());
        } else {
            qWarning("no OpenGL: the GPU shader path is not checked");
        }
        m_cpu->setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        QCOMPARE(check(m_cpu->renderImage(clipTimeline(item), 10), "cpu"), QString());
        const QString out = m_dir.filePath(file + QStringLiteral(".out.mp4"));
        QVERIFY(exportItem(project, item, out, m_gpu.get()).ok);
        QCOMPARE(check(decodeAt(out, 500'000), "export"), QString());
    }

    // ---- 46.3 / 46.4 Broken files: clear messages, incomplete files shortened ------------
    void brokenFileMessages_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("message");
        QTest::newRow("empty") << fixture("empty.mov") << QStringLiteral("The file is empty");
        QTest::newRow("no moov") << realWorld(QStringLiteral("cut_no_moov.mp4"))
                                 << QStringLiteral("The file is incomplete: its index");
        QTest::newRow("random bytes")
            << fixture("corrupt.mp4") << QStringLiteral("Not a video, audio or image file");
        QTest::newRow("text as mp4") << realWorld(QStringLiteral("text_named.mp4"))
                                     << QStringLiteral("Not a video, audio or image file");
        QTest::newRow("missing") << m_dir.filePath(QStringLiteral("gone.mp4"))
                                 << QStringLiteral("File not found");
    }
    void brokenFileMessages()
    {
        QFETCH(QString, path);
        QFETCH(QString, message);
        if (!m_ready)
            QSKIP(qPrintable(m_why));
        Project project;
        ImportResult r;
        QVERIFY(importOne(project, path, &r).id.isEmpty());
        QCOMPARE(r.warnings.size(), 1);
        QVERIFY2(r.warnings.front().contains(message), qPrintable(r.warnings.front()));
    }
    void unreadableFileMessage()
    {
        if (!m_ready)
            QSKIP(qPrintable(m_why));
        const QString path = m_dir.filePath(QStringLiteral("noread.mp4"));
        QVERIFY(QFile::copy(fixture("formats/mp4_h264.mp4"), path));
        QVERIFY(QFile::setPermissions(path, QFileDevice::Permissions()));
        if (QFile(path).open(QIODevice::ReadOnly))
            QSKIP("running as root: permissions are not enforced");
        Project project;
        ImportResult r;
        QVERIFY(importOne(project, path, &r).id.isEmpty());
        QVERIFY2(r.warnings.value(0).contains(QLatin1String("permission denied")),
            qPrintable(r.warnings.value(0)));
    }
    void incompleteFileIsShortened()
    {
        if (!m_ready)
            QSKIP(qPrintable(m_why));
        // h264_25fps.mp4 (10 s, index at the front) cut at 40 %: about 4 s of media data.
        Project project;
        ImportResult r;
        const MediaItem item = importOne(project, fixture("truncated.mp4"), &r);
        QVERIFY(!item.id.isEmpty());
        QVERIFY2(item.info.durationUs > 3'000'000 && item.info.durationUs < 4'200'000,
            qPrintable(QString::number(item.info.durationUs)));
        QCOMPARE(r.notes.size(), 1);
        QVERIFY2(r.notes.front().contains(QLatin1String("The file is incomplete: only the first")),
            qPrintable(r.notes.front()));
        // The last frame of the shortened clip still decodes (the clip does not end frozen).
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(fixture("truncated.mp4")));
        QCOMPARE(decoder.info().durationUs, item.info.durationUs);
        const int64_t lastUs = item.info.durationUs - 40'000;
        QCOMPARE(readFrameIndex(decoder.frameAt(lastUs)), int(expectedFrameIndex(lastUs, 25, 1)));
        // Complete files carry no note.
        Project other;
        ImportResult complete;
        QVERIFY(!importOne(other, fixture("h264_25fps.mp4"), &complete).id.isEmpty());
        QVERIFY(complete.notes.isEmpty());
    }

    // ---- 46.5 The rest of the real-world matrix -------------------------------------------
    void formats_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("width"); // display size; 0 = audio only
        QTest::addColumn<int>("height");
        QTest::addColumn<double>("fps"); // 0 = still / audio
        QTest::addColumn<int>("channels"); // audio channels in the file; 0 = no audio
        QTest::addColumn<int>("sampleRate");
        const auto row = [](const char* f, int w, int h, double fps, int ch, int rate) {
            QTest::newRow(f) << QString::fromLatin1(f) << w << h << fps << ch << rate;
        };
        row("phone_vfr.mp4", 640, 360, 30, 2, 48000);
        row("uhd60.mp4", 3840, 2160, 60, 1, 48000);
        row("cam_prores422hq.mov", 320, 240, 25, 1, 48000);
        row("cam_mjpeg.mov", 320, 240, 30, 1, 48000);
        row("cam_dnxhr_hq.mxf", 640, 360, 25, 1, 48000);
        row("scr_1366x768.mp4", 1366, 768, 30, 0, 0);
        row("scr_1440p.mp4", 2560, 1440, 30, 0, 0);
        row("scr_odd_1281x721.mp4", 1281, 721, 30, 0, 0);
        row("scr_5fps.mp4", 640, 360, 5, 1, 48000);
        row("scr_longgop.mp4", 320, 240, 30, 0, 0);
        row("aud_51.wav", 0, 0, 0, 6, 48000);
        row("aud_51.mp4", 320, 240, 25, 6, 48000);
        row("aud_24bit.wav", 0, 0, 0, 2, 48000);
        row("aud_44k_stereo.wav", 0, 0, 0, 2, 44100);
        row("aud_mono_44k.m4a", 0, 0, 0, 1, 44100);
        row("aud_cover.mp3", 0, 0, 0, 1, 44100); // the cover is not a video stream
        row("aud_cover.m4a", 0, 0, 0, 1, 44100);
        row("two_audio.mp4", 320, 240, 25, 2, 48000); // first stream only (spec 46.6)
        row("big_photo_exif6.jpg", 3024, 4032, 0, 0, 0); // EXIF orientation 6 applied
        row("mp4_named.mov", 320, 240, 25, 1, 48000); // wrong extensions: read by content
        row("wav_named.mp3", 0, 0, 0, 1, 48000);
        row("png_named.jpg", 64, 48, 0, 0, 0);
    }
    void formats()
    {
        QFETCH(QString, file);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(double, fps);
        QFETCH(int, channels);
        QFETCH(int, sampleRate);
        if (!m_ready || !QFileInfo::exists(realWorld(file)))
            QSKIP("fixture missing");
        Project project;
        ImportResult r;
        const MediaItem item = importOne(project, realWorld(file), &r);
        QVERIFY2(!item.id.isEmpty(), qPrintable(r.warnings.join(QLatin1Char(' '))));
        QVERIFY(r.notes.isEmpty());
        const media::MediaInfo& info = item.info;
        QCOMPARE(info.hasVideo, width > 0);
        QCOMPARE(info.hasAudio, channels > 0);
        QCOMPARE(info.isStillImage, width > 0 && fps == 0);
        if (width > 0) {
            QCOMPARE(QSize(info.displayWidth, info.displayHeight), QSize(width, height));
            media::VideoDecoder decoder;
            QVERIFY(decoder.open(realWorld(file)));
            for (int64_t t : {int64_t(0), info.durationUs / 2, info.durationUs - 50'000}) {
                const QImage image = decoder.frameAt(std::max<int64_t>(0, t));
                QCOMPARE(image.size(), QSize(width, height));
                QVERIFY(meanLuma(image) > 20);
            }
            const QImage thumb = decoder.frameAt(0, QSize(160, 90));
            QVERIFY(!thumb.isNull() && thumb.width() <= 160 && thumb.height() <= 90);
            if (info.isStillImage && width < height) { // EXIF orientation 6 applied
                QCOMPARE(checkCorners(decoder.frameAt(0), Corners {kBlue, kRed, kWhite, kLime}),
                    QString());
            }
        }
        if (fps > 0)
            QCOMPARE(info.frameRate.toDouble(), fps);
        if (channels > 0) {
            QCOMPARE(info.audioChannels, channels);
            QCOMPARE(info.audioSampleRate, sampleRate);
            const std::vector<float> audio = decodeAudio(realWorld(file), 48000);
            QVERIFY2(rms(audio) > 0.05, qPrintable(QString::number(rms(audio))));
            // 440 Hz whatever the file's rate: 44.1 kHz is resampled, not played fast.
            QVERIFY2(std::abs(frequency(audio) - 440) < 8,
                qPrintable(QString::number(frequency(audio))));
            QString error;
            const auto waveform = media::computeWaveform(realWorld(file), nullptr, &error);
            QVERIFY2(waveform && waveform->size() > 0, qPrintable(error));
        }
        if (info.isStillImage)
            return;
        // The render matches the source: size, length, sound at the right pitch.
        const QString out = m_dir.filePath(file + QStringLiteral(".out.mp4"));
        const ExportResult er = exportItem(project, item, out, m_cpu.get());
        QVERIFY2(er.ok, qPrintable(er.error));
        const media::MediaInfo rendered = media::probe(out);
        if (width > 0) {
            // H.264 4:2:0 needs even sizes: an odd size loses one pixel.
            QCOMPARE(rendered.displayWidth, width & ~1);
            QCOMPARE(rendered.displayHeight, height & ~1);
        }
        QVERIFY2(std::abs(rendered.durationUs - info.durationUs) < 120'000,
            qPrintable(QStringLiteral("%1 vs %2").arg(rendered.durationUs).arg(info.durationUs)));
        if (channels > 0)
            QVERIFY(std::abs(frequency(decodeAudio(out, 24000)) - 440) < 10);
        if (width > 0) {
            const QString proxy
                = generateProxy(project, item, m_dir.filePath("Proxy"), m_cpu.get());
            QVERIFY(!proxy.isEmpty());
            const media::MediaInfo p = media::probe(proxy);
            QVERIFY(p.hasVideo && p.displayWidth > 0);
            QVERIFY(std::abs(p.durationUs - info.durationUs) < 120'000);
        }
    }

    // ---- 46.7 Awkward names and folders ----------------------------------------------------
    void awkwardPaths_data()
    {
        QTest::addColumn<QString>("relative");
        QTest::newRow("long name") << QString(200, QLatin1Char('a')) + QStringLiteral(".mp4");
        QTest::newRow("cyrillic and emoji")
            << QString::fromUtf8("Видео с отпуска \xF0\x9F\x8E\xAC.mp4");
        QTest::newRow("spaces") << QStringLiteral("my videos/day one/clip 1.mp4");
        QTest::newRow("read-only folder") << QStringLiteral("readonly/clip.mp4");
    }
    void awkwardPaths()
    {
        QFETCH(QString, relative);
        if (!m_ready)
            QSKIP(qPrintable(m_why));
        QTemporaryDir dir;
        const QString path = dir.filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QVERIFY(QFile::copy(fixture("formats/mp4_h264.mp4"), path));
        const QString folder = QFileInfo(path).absolutePath();
        const bool readOnly = relative.startsWith(QLatin1String("readonly"));
        if (readOnly)
            QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner);
        Project project;
        const MediaItem item = importOne(project, path);
        QVERIFY(!item.id.isEmpty());
        QCOMPARE(item.info.durationUs, 2'000'000);
        QVERIFY(!decodeAt(path, 1'000'000).isNull());
        QVERIFY(rms(decodeAudio(path, 24000)) > 0.05);
        QVERIFY(media::computeWaveform(path));
        QVERIFY(!generateProxy(project, item, dir.filePath(QStringLiteral("Proxy")), m_cpu.get())
                     .isEmpty());
        if (readOnly) {
            // Rendering into the read-only folder fails with a message; it does not hang.
            const ExportResult er
                = exportItem(project, item, folder + QStringLiteral("/out.mp4"), m_cpu.get());
            const bool enforced = !QFileInfo(folder).isWritable();
            QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            if (enforced) {
                QVERIFY(!er.ok);
                QVERIFY(!er.error.isEmpty());
                qInfo().noquote() << "read-only render message:" << er.error;
            }
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRealWorld>(argc, argv);
}

#include "test_realworld.moc"
