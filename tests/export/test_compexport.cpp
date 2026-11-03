// SPDX-License-Identifier: GPL-3.0-or-later
// End to end (spec 42a): a clip with a composition (a Background and a Text merged over its
// MediaIn) exported to a file through ExportJob, the same path as the Deliver page, then
// decoded and checked; the project round trip keeps the composition.
#include "ExportTestUtil.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "export/ExportJob.h"
#include "render/Compositor.h"

#include <QTemporaryDir>
#include <QTest>

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
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompExport>(argc, argv);
}
#include "test_compexport.moc"
