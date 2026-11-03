// SPDX-License-Identifier: GPL-3.0-or-later
// Color page end to end (spec 44a): a clip grade (primaries, a soft circle window, a curve) and
// a timeline grade show the same pixels in the render (ExportJob, lossless PNG), the Edit page
// viewer (Compositor), playback (PlaybackEngine) and the CPU reference maths; "bypass all
// grades" changes only the viewer. Render cache chunks are ExportJob renders, so they get the
// same pictures (their keys change with the grade: test_graderender). Small frames: llvmpipe.
#include "ExportTestUtil.h"
#include "core/Grade.h"
#include "core/Timeline.h"
#include "export/ExportJob.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/GradeEval.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

int maxDiff(const QImage& a0, const QImage& b0)
{
    const QImage a = a0.convertToFormat(QImage::Format_RGB32);
    const QImage b = b0.convertToFormat(QImage::Format_RGB32);
    if (a.size() != b.size())
        return 999;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q))});
        }
    }
    return worst;
}

} // namespace

class TestGradeExport : public QObject
{
    Q_OBJECT
private slots:
    void gradedRenderMatchesEverywhere()
    {
        QTemporaryDir dir;
        constexpr int kW = 160, kH = 90;
        QImage plate(kW, kH, QImage::Format_RGB32);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                plate.setPixelColor(
                    x, y, QColor::fromHsvF(double(x) / kW, 0.7, 0.3 + 0.6 * y / kH));
        const QString platePath = dir.filePath(QStringLiteral("plate.png"));
        QVERIFY(plate.save(platePath));
        auto media = std::make_shared<MediaPathMap>();
        media->insert(QStringLiteral("plate"), platePath);

        // Clip grade: warmer and more contrast inside a soft circle, a Y curve on a second node.
        ClipGrade clipGrade;
        const quint64 a = go::initialize(clipGrade);
        clipGrade.node(a)->params.setBase(gk::Temperature, 0.5);
        clipGrade.node(a)->params.setBase(gk::Contrast, 1.4);
        const quint64 w = go::addWindow(clipGrade, a, GradeWindowShape::Circle);
        clipGrade.window(w)->params.setBase(wk::SoftOut, 0.1);
        const quint64 b = go::addSerialNode(clipGrade, a);
        clipGrade.node(b)->curves[GradeCurve::Luma] = {{0.3, 0.2}, {0.7, 0.8}};
        // Timeline grade: less saturation over everything.
        ClipGrade timelineGrade;
        go::initialize(timelineGrade);
        timelineGrade.nodes[0].params.setBase(gk::Saturation, 0.6);

        Project project;
        project.beginEdit(QStringLiteral("Setup"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = kW;
        tl.height = kH;
        tl.fps = {25, 1};
        tl.videoTracks[0].clips.clear();
        ClipData clip = mediaClip(tl, QStringLiteral("plate"), 0, 10);
        const quint64 clipId = clip.id;
        addClip(tl, TrackType::Video, 0, clip);
        project.endEdit();
        const quint64 uid = project.activeTimelineUid();
        QVERIFY(project.setClipGrade(uid, clipId, clipGrade, QStringLiteral("Grade")));
        QVERIFY(project.setTimelineGrade(uid, timelineGrade, QStringLiteral("Timeline Grade")));
        const auto snapshot = project.timeline()->snapshot();

        // The reference: the CPU maths on the plate.
        QImage reference = plate.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        render::applyGrade(clipGrade, 5, reference);
        render::applyGrade(timelineGrade, 5, reference);
        QVERIFY(maxDiff(reference, plate) > 30); // really graded

        auto compositor = std::make_shared<Compositor>();
        compositor->setMediaPaths(media);
        constexpr int64_t kFrame = 5;
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
        qInfo("render vs CPU reference: %d (%s)", maxDiff(rendered, reference),
            qPrintable(compositor->backendDescription()));
        QVERIFY2(maxDiff(rendered, reference) <= 3,
            qPrintable(QString::number(maxDiff(rendered, reference))));

        // The Edit page viewer.
        const QImage viewer = compositor->renderImage(snapshot, kFrame);
        QVERIFY2(
            maxDiff(viewer, rendered) <= 2, qPrintable(QString::number(maxDiff(viewer, rendered))));

        // Playback: the engine's still, then with every grade bypassed (viewer only).
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
        const RenderedFramePtr graded = engine.currentFrame();
        engine.setBypassGrades(true);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != graded; }, 5000));
        QVERIFY2(maxDiff(engine.currentFrame()->image, plate) <= 2,
            qPrintable(QString::number(maxDiff(engine.currentFrame()->image, plate))));
        // The scopes / still picture of the engine follows the bypass state too.
        const RenderedFramePtr still = engine.renderImage(kFrame, QSize(kW, kH)).get();
        QVERIFY(still && still->ok);
        QVERIFY(maxDiff(still->image, plate) <= 2);
        engine.setBypassGrades(false);
        const RenderedFramePtr gradedStill = engine.renderImage(kFrame, QSize(kW, kH)).get();
        QVERIFY(maxDiff(gradedStill->image, rendered) <= 2);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestGradeExport>(argc, argv);
}
#include "test_gradeexport.moc"
