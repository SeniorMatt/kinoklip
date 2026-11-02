// SPDX-License-Identifier: GPL-3.0-or-later
// Clip fades and Clip Attributes in the renderers and the mixer (spec section 31): fade
// handles (video opacity ramp, audio equal-power ramp), channel routing (left only to both
// sides, swap, mute), pixel aspect and extra rotation (GPU and CPU), data levels and
// deinterlacing (GPU).
#include "RenderTestUtil.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

/// Peak absolute value of channel @p ch of interleaved stereo @p buf.
float peak(const std::vector<float>& buf, int ch)
{
    float p = 0.0f;
    for (size_t i = size_t(ch); i < buf.size(); i += 2)
        p = std::max(p, std::abs(buf[i]));
    return p;
}

bool runFfmpeg(const QStringList& args)
{
    QProcess p;
    p.start(QStringLiteral("ffmpeg"),
        QStringList {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")} + args);
    return p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

} // namespace

class TestClipAttributes : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_dir;
    QString m_stereo; ///< left: 1 kHz sine at 0.5, right: silence (2 s)
    QString m_tone; ///< mono 1 kHz sine at 0.5 (2 s)
    QString m_black; ///< limited-range black YUV video (1 s, 64x64)
    QString m_halves; ///< 80x80 PNG: left half red, right half blue
    QString m_lines; ///< 64x64 PNG: even rows red, odd rows blue

    Compositor* backend()
    {
        QFETCH(bool, useGpu);
        if (useGpu && !m_gpu->isGpu())
            return nullptr;
        return useGpu ? m_gpu.get() : m_cpu.get();
    }
    void backendRows()
    {
        QTest::addColumn<bool>("useGpu");
        QTest::newRow("gpu") << true;
        QTest::newRow("cpu") << false;
    }
    static std::shared_ptr<MediaPathMap> media(const QString& id, const QString& path)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(id, path);
        return map;
    }
    /// Mixes @p frames video frames (25 fps) of @p tl from frame @p from.
    static std::vector<float> mix(
        AudioMixer& mixer, const TimelineData& tl, int64_t from, int frames)
    {
        const int samples = frames * 1920;
        std::vector<float> out(size_t(samples) * 2);
        mixer.mix(tl, framesToSamples48k(from, tl.fps), samples, out.data());
        return out;
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        QVERIFY(m_dir.isValid());
        QImage halves(80, 80, QImage::Format_ARGB32);
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 80; ++x)
                halves.setPixel(x, y, x < 40 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        m_halves = m_dir.filePath(QStringLiteral("halves.png"));
        QVERIFY(halves.save(m_halves));
        QImage lines(64, 64, QImage::Format_ARGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                lines.setPixel(x, y, y % 2 == 0 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        m_lines = m_dir.filePath(QStringLiteral("lines.png"));
        QVERIFY(lines.save(m_lines));
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            return;
        m_stereo = m_dir.filePath(QStringLiteral("left_only.wav"));
        m_tone = m_dir.filePath(QStringLiteral("tone.wav"));
        m_black = m_dir.filePath(QStringLiteral("black.mkv"));
        if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("aevalsrc=0.5*sin(2*PI*1000*t)|0:s=48000:d=2"), m_stereo}))
            m_stereo.clear();
        if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("aevalsrc=0.5*sin(2*PI*1000*t):s=48000:d=2"), m_tone}))
            m_tone.clear();
        if (!runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("color=c=black:s=64x64:d=1:r=25"), QStringLiteral("-pix_fmt"),
                QStringLiteral("yuv420p"), QStringLiteral("-color_range"), QStringLiteral("tv"),
                QStringLiteral("-c:v"), QStringLiteral("ffv1"), m_black}))
            m_black.clear();
    }

    // ---- Fades ----

    void fadeMaths()
    {
        // Video: linear, 0 on the first frame, full after fadeIn frames; mirrored at the end.
        QCOMPARE(videoFadeFactor(0, 100, 10, 0), 0.0);
        QCOMPARE(videoFadeFactor(5, 100, 10, 0), 0.5);
        QCOMPARE(videoFadeFactor(10, 100, 10, 0), 1.0);
        QCOMPARE(videoFadeFactor(99, 100, 0, 10), 0.0);
        QCOMPARE(videoFadeFactor(94, 100, 0, 10), 0.5);
        QCOMPARE(videoFadeFactor(50, 100, 10, 10), 1.0);
        // Audio: equal power.
        QCOMPARE(audioFadeGain(0.0, 100, 10, 0), 0.0);
        QVERIFY(std::abs(audioFadeGain(5.0, 100, 10, 0) - std::sin(std::numbers::pi / 4)) < 1e-9);
        QCOMPARE(audioFadeGain(10.0, 100, 10, 0), 1.0);
        QVERIFY(audioFadeGain(100.0, 100, 0, 10) < 1e-12);
    }

    void videoFadesRender_data() { backendRows(); }
    void videoFadesRender()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL 3.3");
        TimelineData tl = emptyTimeline(64, 36);
        ClipData clip = solidClip(tl, Qt::white, 0, 50);
        clip.fadeInFrames = 10;
        clip.fadeOutFrames = 20;
        addClip(tl, TrackType::Video, 0, clip);
        const auto shot = snap(tl);
        // Premultiplied output over transparent: alpha = opacity.
        auto alphaAt = [&](int64_t f) { return qAlpha(c->renderImage(shot, f).pixel(32, 18)); };
        QCOMPARE(alphaAt(0), 0);
        QVERIFY(std::abs(alphaAt(5) - 128) <= 2);
        QCOMPARE(alphaAt(20), 255);
        QVERIFY(std::abs(alphaAt(39) - 128) <= 2); // 10 frames before the last one, of 20
        QCOMPARE(alphaAt(49), 0);
    }

    void audioFadesMix()
    {
        if (m_tone.isEmpty())
            QSKIP("ffmpeg not available");
        TimelineData tl = emptyTimeline();
        ClipData clip = mediaClip(tl, QStringLiteral("t"), 0, 50, 0, 2'000'000);
        clip.fadeInFrames = 25;
        clip.fadeOutFrames = 10;
        addClip(tl, TrackType::Audio, 0, clip);
        AudioMixer mixer;
        mixer.setMediaPaths(media(QStringLiteral("t"), m_tone));
        // The first frame is nearly silent, the middle of the fade-in at sin(pi/4), full level
        // after it, and the last frames fade to silence.
        QVERIFY(peak(mix(mixer, tl, 0, 1), 0) < 0.5f * 0.07f);
        const float mid = peak(mix(mixer, tl, 12, 1), 0);
        QVERIFY2(std::abs(mid - 0.5f * float(std::sin(std::numbers::pi / 4))) < 0.03f,
            qPrintable(QString::number(mid)));
        QVERIFY(std::abs(peak(mix(mixer, tl, 30, 1), 0) - 0.5f) < 0.01f);
        QVERIFY(peak(mix(mixer, tl, 49, 1), 0) < 0.5f * 0.17f);
    }

    void volumeKeysMix()
    {
        if (m_tone.isEmpty())
            QSKIP("ffmpeg not available");
        // Level line keys (as Alt+click and a key drag make them): 0 dB at 0, -6 dB at 20.
        TimelineData tl = emptyTimeline();
        ClipData clip = mediaClip(tl, QStringLiteral("t"), 0, 50, 0, 2'000'000);
        AnimatedValue& v = clip.audio.animated(keys::VolumeDb, 0.0);
        v.setKey(0, 0.0);
        v.setKey(20, -6.0206);
        addClip(tl, TrackType::Audio, 0, clip);
        AudioMixer mixer;
        mixer.setMediaPaths(media(QStringLiteral("t"), m_tone));
        QVERIFY(std::abs(peak(mix(mixer, tl, 0, 1), 0) - 0.5f) < 0.03f);
        QVERIFY(std::abs(peak(mix(mixer, tl, 30, 1), 0) - 0.25f) < 0.01f); // after the key
        const float mid = peak(mix(mixer, tl, 10, 1), 0); // about -3 dB
        QVERIFY2(mid > 0.33f && mid < 0.38f, qPrintable(QString::number(mid)));
    }

    // ---- Channel mapping ----

    void channelMatrixMaths()
    {
        ClipAttributes a;
        double m[2 * 3];
        QVERIFY(!channelMatrix(a, 2, m)); // Auto: the decoder's default layout
        a.channelFormat = ChannelFormat::Mono;
        a.left = 0;
        QVERIFY(channelMatrix(a, 2, m));
        QCOMPARE(m[0], 1.0);
        QCOMPARE(m[1], 0.0);
        QCOMPARE(m[2], 1.0); // right output row
        QCOMPARE(m[3], 0.0);
        a.left = kMixAllChannels;
        QVERIFY(channelMatrix(a, 3, m));
        QCOMPARE(m[0], 1.0 / 3);
        QCOMPARE(m[5], 1.0 / 3);
        a.channelFormat = ChannelFormat::Stereo;
        a.left = 1;
        a.right = kMutedChannel;
        QVERIFY(channelMatrix(a, 2, m));
        QCOMPARE(m[1], 1.0);
        QCOMPARE(m[2] + m[3], 0.0);
    }

    void channelMappingMix()
    {
        if (m_stereo.isEmpty())
            QSKIP("ffmpeg not available");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Audio, 0, mediaClip(tl, QStringLiteral("s"), 0, 50, 0, 2'000'000));
        AudioMixer mixer;
        mixer.setMediaPaths(media(QStringLiteral("s"), m_stereo));
        auto at = [&] { return mix(mixer, tl, 10, 2); };
        ClipData& clip = tl.audioTracks[0].clips[0];

        std::vector<float> out = at(); // as in the file: left only
        QVERIFY(std::abs(peak(out, 0) - 0.5f) < 0.01f);
        QVERIFY(peak(out, 1) < 1e-4f);

        clip.attributes.channelFormat = ChannelFormat::Mono; // left channel on both sides
        clip.attributes.left = 0;
        out = at();
        QVERIFY(std::abs(peak(out, 0) - 0.5f) < 0.01f);
        QVERIFY2(std::abs(peak(out, 1) - 0.5f) < 0.01f, qPrintable(QString::number(peak(out, 1))));

        clip.attributes.channelFormat = ChannelFormat::Stereo; // swapped
        clip.attributes.left = 1;
        clip.attributes.right = 0;
        out = at();
        QVERIFY(peak(out, 0) < 1e-4f);
        QVERIFY(std::abs(peak(out, 1) - 0.5f) < 0.01f);

        clip.attributes.right = kMutedChannel; // muted right
        out = at();
        QVERIFY(peak(out, 0) < 1e-4f && peak(out, 1) < 1e-4f);

        clip.attributes = ClipAttributes {}; // back to the file's layout
        out = at();
        QVERIFY(std::abs(peak(out, 0) - 0.5f) < 0.01f);
        QVERIFY(peak(out, 1) < 1e-4f);
    }

    // ---- Video attributes ----

    void pixelAspectAndRotation_data() { backendRows(); }
    void pixelAspectAndRotation()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL 3.3");
        c->setMediaPaths(media(QStringLiteral("h"), m_halves));
        TimelineData tl = emptyTimeline(320, 180);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("h"), 0, 25));
        ClipData& clip = tl.videoTracks[0].clips[0];
        // Square pixels: 80x80 fitted to 180x180 at x 70..250 (red left, blue right).
        QImage img = c->renderImage(snap(tl), 0);
        QCOMPARE(qAlpha(img.pixel(40, 90)), 0);
        QVERIFY(near(px(img, 100, 90), 255, 0, 0, 255, 8));
        QVERIFY(near(px(img, 220, 90), 0, 0, 255, 255, 8));
        // 2:1 pixels: 160x80 display, fitted to 320x160 (y 10..170).
        clip.attributes.pixelAspect = Rational {2, 1};
        img = c->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 40, 90), 255, 0, 0, 255, 8));
        QVERIFY(near(px(img, 280, 90), 0, 0, 255, 255, 8));
        QCOMPARE(qAlpha(img.pixel(160, 4)), 0);
        // Rotation 90 degrees clockwise (square pixels again): the left half goes on top.
        clip.attributes.pixelAspect = Rational {0, 1};
        clip.attributes.rotation = 90;
        img = c->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 160, 40), 255, 0, 0, 255, 8));
        QVERIFY(near(px(img, 160, 140), 0, 0, 255, 255, 8));
        // 270: the left half at the bottom.
        clip.attributes.rotation = 270;
        img = c->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 160, 140), 255, 0, 0, 255, 8));
    }

    void dataLevels()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL 3.3");
        if (m_black.isEmpty())
            QSKIP("ffmpeg not available");
        m_gpu->setMediaPaths(media(QStringLiteral("b"), m_black));
        TimelineData tl = emptyTimeline(64, 64);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("b"), 0, 25, 0, 1'000'000));
        ClipData& clip = tl.videoTracks[0].clips[0];
        auto grey = [&] { return qRed(m_gpu->renderImage(snap(tl), 3).pixel(32, 32)); };
        QVERIFY(grey() <= 2); // limited range: Y 16 is black
        clip.attributes.levels = DataLevels::Full; // read as full range: 16 shows as dark grey
        const int full = grey();
        QVERIFY2(full >= 13 && full <= 19, qPrintable(QString::number(full)));
        clip.attributes.levels = DataLevels::Video;
        QVERIFY(grey() <= 2);
    }

    void deinterlace()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL 3.3");
        m_gpu->setMediaPaths(media(QStringLiteral("l"), m_lines));
        TimelineData tl = emptyTimeline(64, 64);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("l"), 0, 25));
        ClipData& clip = tl.videoTracks[0].clips[0];
        QImage img = m_gpu->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 32, 31), 0, 0, 255, 255, 8)); // odd row: blue
        clip.attributes.deinterlace = true; // upper field kept: every row red
        img = m_gpu->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 32, 30), 255, 0, 0, 255, 8));
        QVERIFY(near(px(img, 32, 31), 255, 0, 0, 255, 8));
        clip.attributes.fieldOrder = FieldOrder::LowerFirst; // lower field: blue
        img = m_gpu->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 32, 30), 0, 0, 255, 255, 8));
        clip.attributes.fieldOrder = FieldOrder::Progressive; // nothing to do
        img = m_gpu->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 32, 30), 255, 0, 0, 255, 8));
        QVERIFY(near(px(img, 32, 31), 0, 0, 255, 255, 8));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestClipAttributes>(argc, argv);
}

#include "test_clipattributes.moc"
