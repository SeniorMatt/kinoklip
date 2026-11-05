// SPDX-License-Identifier: GPL-3.0-or-later
// Keyframed clips on the CPU fallback (Bog's "keyframed clips stutter even at quarter playback
// resolution", spec 48.6). FrameConverter keeps its swscale context and colour details frame
// after frame at any output size (an odd width used to rebuild the context on every call and
// fall back to BT.601); a 1080p H.264 clip with keyframed zoom, crop and position renders at the
// playback resolution's size, and the time per frame of keyframed and static clips is printed
// for the record (no limit: other builds on the machine make timings noisy).
#include "../media/MediaTestUtil.h"
#include "RenderTestUtil.h"
#include "core/ParamKeys.h"
#include "media/FrameConverter.h"
#include "render/Compositor.h"

#include <QTest>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int64_t kFrames = 75; // 3 s at 25 fps
const QString kMediaId = QStringLiteral("m1");

TimelineData clipTimeline(bool keyframed)
{
    TimelineData tl = emptyTimeline(1920, 1080);
    ClipData clip = mediaClip(tl, kMediaId, 0, 200, 0, 8'000'000);
    if (keyframed) {
        // A slow push-in with a crop that opens up: a new zoom and crop on every frame.
        clip.transform.animated(keys::ZoomX, 1.0).setKey(0, 1.0);
        clip.transform.animated(keys::ZoomX, 1.0).setKey(kFrames, 1.6);
        clip.transform.animated(keys::ZoomY, 1.0).setKey(0, 1.0);
        clip.transform.animated(keys::ZoomY, 1.0).setKey(kFrames, 1.6);
        clip.transform.animated(keys::CropL, 0.0).setKey(0, 200.0);
        clip.transform.animated(keys::CropL, 0.0).setKey(kFrames, 0.0);
        clip.transform.animated(keys::PosX, 0.0).setKey(0, -100.0);
        clip.transform.animated(keys::PosX, 0.0).setKey(kFrames, 100.0);
    }
    addClip(tl, TrackType::Video, 0, clip);
    return tl;
}

} // namespace

Q_DECLARE_METATYPE(kk::Quality)

class TestKeyframedCpu : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_cpu;
    std::shared_ptr<const MediaPathMap> m_media;

    /// Median render time (ms) of frames 0..kFrames-1 for a viewer-sized output.
    double playMs(const TimelineData& tl, Quality quality, int* outWidth)
    {
        RenderRequest warm;
        warm.timeline = snap(tl);
        warm.media = m_media;
        warm.outSize = QSize(1000, 562);
        warm.quality = quality;
        warm.outputs = OutputImage;
        (void)m_cpu->renderAsync(warm).get(); // opens the decoder
        std::vector<double> times;
        for (int64_t f = 0; f < kFrames; ++f) {
            RenderRequest rq = warm;
            rq.frame = f;
            const RenderedFramePtr frame = m_cpu->renderAsync(rq).get();
            if (!frame || !frame->ok || frame->image.isNull()
                || px(frame->image, frame->image.width() / 2, frame->image.height() / 2).a != 255)
                return -1.0; // the clip covers the centre
            times.push_back(frame->renderMs);
            *outWidth = frame->image.width();
        }
        std::nth_element(times.begin(), times.begin() + kFrames / 2, times.end());
        return times[size_t(kFrames / 2)];
    }

private slots:
    void initTestCase()
    {
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        auto media = std::make_shared<MediaPathMap>();
        media->insert(kMediaId, fixture("perf_1080p.mp4"));
        m_media = media;
    }
    void cleanupTestCase() { m_cpu.reset(); }

    void spec48_6_keyframedClipRendersAtThePlaybackResolution_data()
    {
        QTest::addColumn<Quality>("quality");
        QTest::addColumn<int>("width");
        QTest::newRow("Full") << Quality::Full << 1000;
        QTest::newRow("Half") << Quality::Half << 500;
        QTest::newRow("Quarter") << Quality::Quarter << 250;
    }
    void spec48_6_keyframedClipRendersAtThePlaybackResolution()
    {
        QFETCH(Quality, quality);
        QFETCH(int, width);
        int outWidth = 0;
        const double still = playMs(clipTimeline(false), quality, &outWidth);
        const double keyed = playMs(clipTimeline(true), quality, &outWidth);
        QVERIFY(still > 0 && keyed > 0);
        QCOMPARE(outWidth, width);
        qInfo("static clip %.2f ms/frame, keyframed zoom + crop + position %.2f ms/frame", still,
            keyed);
    }

    void spec48_6_oddOutputWidthKeepsTheColours_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("4:2:0") << int(AV_PIX_FMT_YUV420P);
        QTest::newRow("4:4:4") << int(AV_PIX_FMT_YUV444P);
    }
    /// Every frame comes out with the same colours (BT.709 here), also at an odd width and for
    /// a source without chroma subsampling, where swscale adds a flag of its own.
    void spec48_6_oddOutputWidthKeepsTheColours()
    {
        QFETCH(int, format);
        AVFrame* frame = av_frame_alloc();
        frame->format = format;
        frame->width = 64;
        frame->height = 36;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_range = AVCOL_RANGE_MPEG;
        QCOMPARE(av_frame_get_buffer(frame, 0), 0);
        for (int plane = 0; plane < 3; ++plane) {
            const int rows = format == AV_PIX_FMT_YUV420P && plane > 0 ? 18 : 36;
            const int value = plane == 0 ? 100 : plane == 1 ? 90 : 180;
            memset(frame->data[plane], value, size_t(frame->linesize[plane]) * size_t(rows));
        }
        for (const QSize size : {QSize(33, 18), QSize(34, 18), QSize(33, 19)}) {
            media::detail::FrameConverter converter;
            const QImage first = converter.convert(frame, size);
            const QImage second = converter.convert(frame, size);
            QVERIFY(!first.isNull());
            // A rebuilt context would fall back to BT.601: a visibly different colour.
            QCOMPARE(second.pixel(16, 9), first.pixel(16, 9));
            QCOMPARE(second.pixel(5, 5), first.pixel(5, 5));
        }
        // The odd width matches the even one (same matrix).
        media::detail::FrameConverter odd, even;
        (void)odd.convert(frame, QSize(33, 18));
        const QColor a = odd.convert(frame, QSize(33, 18)).pixelColor(16, 9);
        const QColor b = even.convert(frame, QSize(34, 18)).pixelColor(16, 9);
        QVERIFY2(std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2
                && std::abs(a.blue() - b.blue()) <= 2,
            qPrintable(a.name() + QLatin1Char(' ') + b.name()));
        av_frame_free(&frame);
    }
};

QTEST_MAIN(TestKeyframedCpu)
#include "test_keyframedcpu.moc"
