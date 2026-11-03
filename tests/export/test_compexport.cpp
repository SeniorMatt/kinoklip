// SPDX-License-Identifier: GPL-3.0-or-later
// End to end (spec 42a): a clip with a composition (a Background and a Text merged over its
// MediaIn) exported to a file through ExportJob, the same path as the Deliver page, then
// decoded and checked; the project round trip keeps the composition. A green-screen key with
// text and a moving Transform gives the same pixels in playback, the viewers and the render.
#include "ExportTestUtil.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/VideoEffectKeys.h"
#include "export/ExportJob.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;

class TestCompExport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void compositionInExport()
    {
        QString why;
        Project project;
        if (!buildExportProject(project, &why))
            QSKIP(qPrintable(why));
        QTemporaryDir dir;
        // The first clip (h264_25fps.mp4, frames 0..29): a half-transparent blue Background
        // merged over the clip's own picture and a large white "X" over that.
        const TimelineData& tl = project.timeline()->data();
        const ClipData& first = tl.videoTracks[0].clips[0];
        Composition c = co::defaultComposition();
        quint64 in = 0;
        for (const CompNode& n : c.nodes)
            in = n.type == nodetypes::MediaIn ? n.id : in;
        const co::AddResult bg = co::addNodeAfter(c, nodetypes::Background, in);
        c.node(bg.node)->params.setStatic(nodekeys::Color, QColor(0, 0, 255, 128));
        const co::AddResult text = co::addNodeAfter(c, nodetypes::Text, bg.merge);
        c.node(text.node)->params.setStatic(nodekeys::Text, QStringLiteral("XXXX"));
        c.node(text.node)->params.setBase(nodekeys::FontSize, 40.0);
        c.node(text.node)->params.setBase(nodekeys::CenterY, 0.2);
        QVERIFY(project.setClipComposition(
            project.activeTimelineUid(), first.id, c, QStringLiteral("Composition")));

        // The project file keeps it.
        const QString projectPath = dir.filePath(QStringLiteral("comp.kkproj"));
        QVERIFY(project.save(projectPath, &why));
        Project loaded;
        QVERIFY2(loaded.load(projectPath, &why), qPrintable(why));
        QCOMPARE(loaded.timeline()->data().videoTracks[0].clips[0].composition, std::optional(c));

        Compositor compositor;
        ExportSettings s;
        s.outputPath = dir.filePath(QStringLiteral("comp.mp4"));
        ExportJob job(
            s, loaded.timeline()->snapshot(), makeMediaPathMap(loaded.mediaPool()->state()));
        job.setCompositor(&compositor);
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));

        // Frame 5: inside the composition. The upper part (no text) is blue-tinted video:
        // blue clearly above red and green; the text area is bright.
        const QImage frame = decodeAt(r.outputPath, 5 * 40'000 + 20'000);
        QVERIFY(!frame.isNull());
        const QImage plainRef
            = decodeAt(fixture("h264_25fps.mp4"), 5 * 40'000 + 20'000).scaled(frame.size());
        const QPoint probe(frame.width() / 8, int(frame.height() * 0.4));
        const QColor got = frame.pixelColor(probe);
        const QColor src = plainRef.pixelColor(probe);
        // 50 % blue over the source (encoding tolerance).
        QVERIFY2(std::abs(got.blue() - (src.blue() + 255) / 2) < 24,
            qPrintable(QStringLiteral("got %1 src %2").arg(got.name(), src.name())));
        QVERIFY2(std::abs(got.red() - src.red() / 2) < 24, qPrintable(got.name()));
        // The white text (centre y 0.2 = the lower fifth) is over the tint: under the 50 %
        // blue no pixel keeps a red above ~140, so bright red marks the text strokes.
        int textPixels = 0, abovePixels = 0;
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                if (frame.pixelColor(x, y).red() > 200)
                    ++(y > frame.height() * 0.65 ? textPixels : abovePixels);
            }
        }
        QVERIFY2(textPixels > 200 && abovePixels < 20,
            qPrintable(QStringLiteral("%1 %2").arg(textPixels).arg(abovePixels)));
        // Frame 40 (the second clip, no composition) is not tinted.
        const QImage later = decodeAt(r.outputPath, 45 * 40'000 + 20'000);
        const QColor l = later.pixelColor(probe);
        QVERIFY2(!(l.blue() > l.red() + 60 && l.blue() > l.green() + 60), qPrintable(l.name()));
    }

    void greenScreenTextTransformEverywhereTheSame()
    {
        // Spec 42a.20: a small composition built the way a user builds it - a green-screen
        // MediaIn keyed (Chroma Key node), moved right by an animated Transform, merged over a
        // blue Background, with white Text on top - shows the same picture in the Edit page
        // viewer / playback (PlaybackEngine), in the Compositing page viewer (MediaOut through
        // the viewer API) and in the render (ExportJob, lossless PNG). Small frames: llvmpipe.
        QTemporaryDir dir;
        constexpr int kW = 320, kH = 180;
        QImage plate(kW, kH, QImage::Format_RGB32);
        plate.fill(QColor(20, 220, 40)); // the green screen
        for (int y = 60; y < 120; ++y)
            for (int x = 40; x < 100; ++x)
                plate.setPixel(x, y, qRgb(220, 30, 30)); // the "actor"
        const QString platePath = dir.filePath(QStringLiteral("greenscreen.png"));
        QVERIFY(plate.save(platePath));
        auto media = std::make_shared<MediaPathMap>();
        media->insert(QStringLiteral("gs"), platePath);

        Project project;
        project.beginEdit(QStringLiteral("Setup"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = kW;
        tl.height = kH;
        tl.fps = {25, 1};
        tl.videoTracks[0].clips.clear();
        ClipData clip = mediaClip(tl, QStringLiteral("gs"), 0, 10);
        const quint64 clipId = clip.id;
        addClip(tl, TrackType::Video, 0, clip);
        project.endEdit();

        Composition c = co::defaultComposition();
        quint64 in = 0;
        for (const CompNode& n : c.nodes)
            in = n.type == nodetypes::MediaIn ? n.id : in;
        const quint64 key
            = co::addNode(c, NodeRegistry::effectNodeType(QLatin1String(fxid::ChromaKey)), {});
        const quint64 move = co::addNode(c, nodetypes::Transform, {});
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        const quint64 over = co::addNode(c, nodetypes::Merge, {});
        const quint64 text = co::addNode(c, nodetypes::Text, {});
        const quint64 top = co::addNode(c, nodetypes::Merge, {});
        QVERIFY(key && move && bg && over && text && top);
        QCOMPARE(co::connect(c, in, 0, key, nodeports::Input), co::ConnectError::None);
        QCOMPARE(co::connect(c, key, 0, move, nodeports::Input), co::ConnectError::None);
        QCOMPARE(co::connect(c, bg, 0, over, nodeports::Background), co::ConnectError::None);
        QCOMPARE(co::connect(c, move, 0, over, nodeports::Foreground), co::ConnectError::None);
        QCOMPARE(co::connect(c, over, 0, top, nodeports::Background), co::ConnectError::None);
        QCOMPARE(co::connect(c, text, 0, top, nodeports::Foreground), co::ConnectError::None);
        QCOMPARE(co::connect(c, top, 0, c.mediaOutId, nodeports::Input), co::ConnectError::None);
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(0, 0, 255));
        // The actor moves right: centre x 0.6 at frame 0 to 0.8 at frame 9 (0.7 at frame 5 is
        // x + 64 px: the box at 104..164).
        AnimatedValue& cx = c.node(move)->params.animated(nodekeys::CenterX, 0.5);
        cx.setKey(0, 0.6);
        cx.setKey(10, 0.8);
        c.node(text)->params.setStatic(nodekeys::Text, QStringLiteral("XX"));
        c.node(text)->params.setBase(nodekeys::FontSize, 50.0);
        c.node(text)->params.setBase(nodekeys::CenterX, 0.8);
        c.node(text)->params.setBase(nodekeys::CenterY, 0.2);
        QVERIFY(project.setClipComposition(
            project.activeTimelineUid(), clipId, c, QStringLiteral("Composition")));
        const auto snapshot = project.timeline()->snapshot();

        auto compositor = std::make_shared<Compositor>();
        compositor->setMediaPaths(media);
        constexpr int64_t kFrame = 5;
        // The render.
        ExportSettings s;
        s.container = QStringLiteral("png_still");
        s.video.codec = QStringLiteral("png");
        s.range = RangeMode::Custom;
        s.rangeIn = kFrame;
        s.rangeOut = kFrame + 1;
        s.outputPath = dir.filePath(QStringLiteral("frame.png"));
        ExportJob job(s, snapshot, media);
        job.setCompositor(compositor.get());
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        const QImage rendered = QImage(s.outputPath).convertToFormat(QImage::Format_RGB32);
        QCOMPARE(rendered.size(), QSize(kW, kH));

        // The picture: blue where the screen was green, the red actor moved right, white
        // text at the lower right.
        auto isBlue = [](QColor q) { return q.blue() > 200 && q.red() < 50 && q.green() < 60; };
        auto isRed = [](QColor q) { return q.red() > 170 && q.green() < 80 && q.blue() < 80; };
        QVERIFY2(
            isBlue(rendered.pixelColor(20, 20)), qPrintable(rendered.pixelColor(20, 20).name()));
        QVERIFY2(
            isBlue(rendered.pixelColor(60, 90)), qPrintable(rendered.pixelColor(60, 90).name()));
        QVERIFY2(
            isRed(rendered.pixelColor(134, 90)), qPrintable(rendered.pixelColor(134, 90).name()));
        QVERIFY(isBlue(rendered.pixelColor(200, 90)));
        int white = 0, whiteAbove = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const QColor q = rendered.pixelColor(x, y);
                if (q.red() > 200 && q.green() > 200 && q.blue() > 200)
                    ++(y > kH / 2 && x > kW / 2 ? white : whiteAbove);
            }
        }
        QVERIFY2(white > 100 && whiteAbove == 0,
            qPrintable(QStringLiteral("%1 %2").arg(white).arg(whiteAbove)));

        auto maxDiff = [](const QImage& a0, const QImage& b0) {
            const QImage a = a0.convertToFormat(QImage::Format_RGB32);
            const QImage b = b0.convertToFormat(QImage::Format_RGB32);
            if (a.size() != b.size())
                return 999;
            int worst = 0;
            for (int y = 0; y < a.height(); ++y) {
                for (int x = 0; x < a.width(); ++x) {
                    const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                    worst = std::max({worst, std::abs(qRed(p) - qRed(q)),
                        std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))});
                }
            }
            return worst;
        };
        // The Edit page viewer's render of the timeline frame.
        const QImage viewer = compositor->renderImage(snapshot, kFrame);
        QVERIFY2(
            maxDiff(viewer, rendered) <= 2, qPrintable(QString::number(maxDiff(viewer, rendered))));
        // Playback: the engine's paused still and the frames it presents while playing.
        PlaybackEngine::Options options;
        options.compositor = compositor;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPaths(media);
        engine.setQualityMode(PlaybackEngine::QualityMode::Full);
        const int client = 0;
        engine.setOutputClient(&client, QSize(kW, kH), false);
        engine.setTimeline(project.timeline());
        engine.seek(kFrame);
        QVERIFY(QTest::qWaitFor(
            [&] { return engine.currentFrame() && engine.currentFrame()->frame == kFrame; }, 5000));
        QVERIFY2(maxDiff(engine.currentFrame()->image, rendered) <= 2,
            qPrintable(QString::number(maxDiff(engine.currentFrame()->image, rendered))));
        int checked = 0;
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (!f || f->image.isNull() || checked >= 3)
                    return;
                QImage reference = compositor->renderImage(snapshot, f->frame);
                if (maxDiff(f->image, reference) <= 2)
                    ++checked;
                else
                    checked = -1000; // a played frame that differs
            });
        engine.seek(0);
        engine.play();
        QTest::qWaitFor([&] { return checked >= 3 || checked < 0; }, 5000);
        engine.pause();
        QVERIFY2(checked >= 3, qPrintable(QString::number(checked)));
        // The Compositing page viewer showing MediaOut (no Edit page attributes on the clip).
        const RenderedFramePtr node
            = engine.renderCompositionNode(snapshot, clipId, 0, kFrame, QSize(kW, kH), OutputImage)
                  .get();
        QVERIFY(node && node->ok);
        QVERIFY2(maxDiff(node->image, rendered) <= 2,
            qPrintable(QString::number(maxDiff(node->image, rendered))));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompExport>(argc, argv);
}
#include "test_compexport.moc"
