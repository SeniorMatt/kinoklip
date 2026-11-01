// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the render module's pure logic: transition windows (PLAN §5.4), per-track
// evaluation, source-time clamping, clip geometry, YUV → RGB coefficients, title styles.
#include "RenderTestUtil.h"
#include "render/TimelineEval.h"
#include "render/TitleRenderer.h"
#include "render/YuvMath.h"

using namespace kk;
using namespace kk::render;
using namespace kk::test;

namespace {

TransitionData dissolve(int64_t frames)
{
    return TransitionData {QStringLiteral("cross_dissolve"), frames, {}};
}

/// Two adjacent solids A [0, 50) and B [50, 100) on V1.
TimelineData twoClips()
{
    TimelineData tl = emptyTimeline();
    addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
    addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::blue, 50, 50));
    return tl;
}

bool approx(float a, float b, float eps = 0.004f)
{
    return std::abs(a - b) <= eps;
}

} // namespace

class TestRenderMath : public QObject
{
    Q_OBJECT
private slots:
    void centredTransitionWindow()
    {
        TimelineData tl = twoClips();
        tl.videoTracks[0].clips[1].transitionIn = dissolve(25);
        const TrackData& t = tl.videoTracks[0];
        const auto head = headTransition(t, 1);
        QVERIFY(head);
        QVERIFY(head->centred);
        QCOMPARE(head->start, int64_t(38)); // 50 − 25/2
        QCOMPARE(head->end, int64_t(63));
        QCOMPARE(head->outgoing, &t.clips[0]);
        QCOMPARE(head->incoming, &t.clips[1]);
        // A sees the same span as its tail.
        const auto tail = tailTransition(t, 0);
        QVERIFY(tail);
        QCOMPARE(tail->start, head->start);
        QCOMPARE(tail->end, head->end);
        // Progress is exclusive at both ends and 0.5 at the cut for odd durations.
        QCOMPARE(head->progress(50), 0.5);
        QVERIFY(head->progress(38) > 0.0);
        QVERIFY(head->progress(62) < 1.0);
    }

    void incomingWinsAtSharedCut()
    {
        TimelineData tl = twoClips();
        tl.videoTracks[0].clips[0].transitionOut = TransitionData {QStringLiteral("wipe"), 10, {}};
        tl.videoTracks[0].clips[1].transitionIn = dissolve(20);
        const auto tail = tailTransition(tl.videoTracks[0], 0);
        QVERIFY(tail);
        QCOMPARE(tail->transition->type, QStringLiteral("cross_dissolve"));
        QCOMPARE(tail->length(), int64_t(20));
        // Without B's transitionIn, A's transitionOut is centred on the same cut.
        tl.videoTracks[0].clips[1].transitionIn.reset();
        const auto head = headTransition(tl.videoTracks[0], 1);
        QVERIFY(head);
        QCOMPARE(head->transition->type, QStringLiteral("wipe"));
        QCOMPARE(head->start, int64_t(45));
        QVERIFY(head->centred);
    }

    void fadesAtTrackEnds()
    {
        TimelineData tl = emptyTimeline();
        ClipData c = solidClip(tl, Qt::white, 10, 30);
        c.transitionIn = dissolve(8);
        c.transitionOut = dissolve(100); // longer than the clip: clamped
        addClip(tl, TrackType::Video, 0, c);
        const auto head = headTransition(tl.videoTracks[0], 0);
        QVERIFY(head && !head->centred);
        QCOMPARE(head->start, int64_t(10));
        QCOMPARE(head->end, int64_t(18));
        QVERIFY(!head->outgoing);
        const auto tail = tailTransition(tl.videoTracks[0], 0);
        QVERIFY(tail && !tail->centred);
        QCOMPARE(tail->start, int64_t(10));
        QCOMPARE(tail->end, int64_t(40));
        QVERIFY(!tail->incoming);
    }

    void gapMeansNoNeighbour()
    {
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
        ClipData b = solidClip(tl, Qt::blue, 51, 50); // one-frame gap
        b.transitionIn = dissolve(10);
        addClip(tl, TrackType::Video, 0, b);
        const auto head = headTransition(tl.videoTracks[0], 1);
        QVERIFY(head && !head->centred);
        QCOMPARE(head->start, int64_t(51));
    }

    void evaluateTrackFrames()
    {
        TimelineData tl = twoClips();
        tl.videoTracks[0].clips[1].transitionIn = dissolve(10);
        const TrackData& t = tl.videoTracks[0];
        QCOMPARE(evaluateTrack(t, 0).single, &t.clips[0]);
        QCOMPARE(evaluateTrack(t, 44).single, &t.clips[0]);
        const TrackFrame mid = evaluateTrack(t, 47); // window [45, 55)
        QVERIFY(!mid.single);
        QVERIFY(mid.transition);
        QCOMPARE(mid.transition->outgoing, &t.clips[0]);
        QVERIFY(evaluateTrack(t, 52).transition);
        QCOMPARE(evaluateTrack(t, 55).single, &t.clips[1]);
        QVERIFY(evaluateTrack(t, 100).isEmpty());
        QVERIFY(evaluateTrack(t, -1).isEmpty());
        QCOMPARE(clipIndexAt(t, 49), 0);
        QCOMPARE(clipIndexAt(t, 50), 1);
        QCOMPARE(clipIndexAt(t, 100), -1);
    }

    void sourceTimeClamping()
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, QStringLiteral("m"), 100, 50, 1'000'000, 3'000'000);
        QCOMPARE(clampedSourceTimeUs(c, 100, tl.fps), int64_t(1'000'000));
        QCOMPARE(clampedSourceTimeUs(c, 125, tl.fps), int64_t(2'000'000));
        // Handles: before the clip extrapolates backwards, clamped at 0 (freeze on frame 0)...
        QCOMPARE(clampedSourceTimeUs(c, 90, tl.fps), int64_t(600'000));
        QCOMPARE(clampedSourceTimeUs(c, 0, tl.fps), int64_t(0));
        // ... and after the media ends it freezes on the last frame.
        QCOMPARE(clampedSourceTimeUs(c, 400, tl.fps), int64_t(2'999'999));
        c.speed = 2.0;
        QCOMPARE(clampedSourceTimeUs(c, 110, tl.fps), int64_t(1'800'000));
    }

    void geometry()
    {
        QCOMPARE(fitSize(QSizeF(1280, 720), QSizeF(1920, 1080)), QSizeF(1920, 1080));
        QCOMPARE(fitSize(QSizeF(1080, 1920), QSizeF(1920, 1080)), QSizeF(607.5, 1080));
        QCOMPARE(displaySize(640, 360, {1, 1}, 90), QSizeF(360, 640));
        QCOMPARE(displaySize(720, 576, {16, 15}, 0), QSizeF(768, 576));

        TimelineData tl = emptyTimeline(1920, 1080);
        ClipData c = solidClip(tl, Qt::white, 0, 10);
        const QSizeF frame(1920, 1080);
        QCOMPARE(clipTransform(c, 0, frame).map(QPointF(0, 0)), QPointF(960, 540));
        c.transform.setBase(keys::PosX, 100);
        c.transform.setBase(keys::ZoomX, 2);
        c.transform.setBase(keys::ZoomY, 2);
        QCOMPARE(clipTransform(c, 0, frame).map(QPointF(10, 10)), QPointF(1080, 560));
        // Anchor = pivot: it stays put while zooming, and alone it moves nothing.
        c.transform.setBase(keys::AnchorX, 50);
        QCOMPARE(clipTransform(c, 0, frame).map(QPointF(50, 0)), QPointF(1110, 540));
        c.transform.setBase(keys::ZoomX, 1);
        c.transform.setBase(keys::ZoomY, 1);
        QCOMPARE(clipTransform(c, 0, frame).map(QPointF(0, 0)), QPointF(1060, 540));
        // Rotation is clockwise on screen (y down): +x maps to +y.
        c.transform = {};
        c.transform.setBase(keys::Rotation, 90);
        const QPointF p = clipTransform(c, 0, frame).map(QPointF(100, 0));
        QVERIFY(std::abs(p.x() - 960) < 1e-9 && std::abs(p.y() - 640) < 1e-9);
        // Flip mirrors around the anchor.
        c.transform = {};
        c.transform.setStatic(keys::FlipH, true);
        QCOMPARE(clipTransform(c, 0, frame).map(QPointF(100, 0)), QPointF(860, 540));
        QCOMPARE(maxZoom(c, 0), 1.0);
    }

    void yuvCoefficients709()
    {
        using media::ColorMatrix;
        using media::ColorRange;
        using media::PixelFormat;
        const YuvCoefficients lim
            = yuvCoefficients(ColorMatrix::Bt709, ColorRange::Limited, PixelFormat::Yuv420p);
        auto rgb = applyYuv(lim, 235 / 255.f, 128 / 255.f, 128 / 255.f);
        QVERIFY(approx(rgb[0], 1) && approx(rgb[1], 1) && approx(rgb[2], 1));
        rgb = applyYuv(lim, 16 / 255.f, 128 / 255.f, 128 / 255.f);
        QVERIFY(approx(rgb[0], 0) && approx(rgb[1], 0) && approx(rgb[2], 0));
        // BT.709 limited-range red: Y 63, Cb 102, Cr 240.
        rgb = applyYuv(lim, 63 / 255.f, 102 / 255.f, 240 / 255.f);
        QVERIFY2(approx(rgb[0], 1, 0.01f) && approx(rgb[1], 0, 0.01f) && approx(rgb[2], 0, 0.01f),
            qPrintable(QStringLiteral("%1 %2 %3").arg(rgb[0]).arg(rgb[1]).arg(rgb[2])));
        // BT.601 limited red: Y 81, Cb 90, Cr 240.
        const YuvCoefficients s601
            = yuvCoefficients(ColorMatrix::Bt601, ColorRange::Limited, PixelFormat::Nv12);
        rgb = applyYuv(s601, 81 / 255.f, 90 / 255.f, 240 / 255.f);
        QVERIFY(approx(rgb[0], 1, 0.01f) && approx(rgb[1], 0, 0.01f) && approx(rgb[2], 0, 0.01f));
        // Full range: Y 255 → white, no offset on luma.
        const YuvCoefficients full
            = yuvCoefficients(ColorMatrix::Bt709, ColorRange::Full, PixelFormat::Yuv444p);
        rgb = applyYuv(full, 1.f, 128 / 255.f, 128 / 255.f);
        QVERIFY(approx(rgb[0], 1) && approx(rgb[1], 1) && approx(rgb[2], 1));
        QCOMPARE(full.offset[0], 0.0f);
        // 10-bit limited: Y 940 → white, 64 → black (codes normalised by 1023).
        const YuvCoefficients ten
            = yuvCoefficients(ColorMatrix::Bt2020, ColorRange::Limited, PixelFormat::Yuv420p10);
        rgb = applyYuv(ten, 940 / 1023.f, 512 / 1023.f, 512 / 1023.f);
        QVERIFY(approx(rgb[0], 1) && approx(rgb[1], 1) && approx(rgb[2], 1));
        rgb = applyYuv(ten, 64 / 1023.f, 512 / 1023.f, 512 / 1023.f);
        QVERIFY(approx(rgb[0], 0) && approx(rgb[2], 0));
        // Sample scales: 8-bit 1, planar 10-bit (low bits) 65535/1023, P010 (high bits).
        QCOMPARE(lim.sampleScale, 1.0f);
        QVERIFY(std::abs(ten.sampleScale - 65535.f / 1023.f) < 1e-3f);
        QVERIFY(std::abs(yuvCoefficients(ColorMatrix::Bt709, ColorRange::Limited, PixelFormat::P010)
                             .sampleScale
                    - 65535.f / (1023.f * 64.f))
            < 1e-6f);
        QCOMPARE(bitDepth(PixelFormat::P010), 10);
        QCOMPARE(bitDepth(PixelFormat::Nv12), 8);
    }

    void titleStyleAndCache()
    {
        TimelineData tl = emptyTimeline();
        ClipData c = titleClip(tl, QStringLiteral("Hi"), 0, 10, 50);
        const TitleStyle a = resolveTitleStyle(c, 0, QSize(320, 180), 1.0);
        QCOMPARE(a.text, QStringLiteral("Hi"));
        QCOMPARE(a.size, 50.0);
        QCOMPARE(a.color, QColor(Qt::white));
        QCOMPARE(a.align, 1);
        QCOMPARE(a.lineSpacing, 100.0);
        const TitleStyle b = resolveTitleStyle(c, 5, QSize(320, 180), 1.0);
        QVERIFY(a == b); // static title: same key on every frame
        QCOMPARE(qHash(a), qHash(b));
        c.title.animated(keys::Size, 50).setKey(0, 50);
        c.title.animated(keys::Size, 50).setKey(10, 100);
        QVERIFY(!(resolveTitleStyle(c, 5, QSize(320, 180), 1.0) == a));
        QVERIFY(!(
            resolveTitleStyle(c, 0, QSize(640, 360), 2.0) == a)); // output scale is part of the key

        const TitleImage image = renderTitle(a);
        QVERIFY(!image.image.isNull());
        QVERIFY(QRectF(0, 0, 320, 180).contains(image.rect)); // tightly cropped, centred
        QVERIFY(std::abs(image.rect.center().x() - 160) < 6);
        TitleStyle empty = a;
        empty.text.clear();
        QVERIFY(renderTitle(empty).image.isNull());
        // Multi-line + background box: taller than one line, box colour present.
        TitleStyle box = a;
        box.text = QStringLiteral("one\ntwo");
        box.bgEnabled = true;
        box.bgOpacity = 100;
        box.bgColor = Qt::red;
        const TitleImage boxed = renderTitle(box);
        QVERIFY(boxed.rect.height() > image.rect.height() * 1.6);
        QCOMPARE(QColor(boxed.image.pixel(3, 3)), QColor(Qt::red));
    }
};

QTEST_MAIN(TestRenderMath)
#include "test_rendermath.moc"
