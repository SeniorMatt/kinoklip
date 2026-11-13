// SPDX-License-Identifier: GPL-3.0-or-later
// Compositing page viewer (docs/behavior-spec.md 99.3, issue #7): a clip's own composition is
// what the viewer shows, not the tracks above it. A clip without a stored graph (the default
// MediaIn -> MediaOut) and the same clip with a stored graph render the same picture, on the GPU
// and the CPU backend, while a clip on V2 covers it in the timeline.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "render/Compositor.h"

#include <QTest>

using namespace kk;
using namespace kk::test;

class TestCompViewerView : public QObject
{
    Q_OBJECT
private slots:
    void emptyAndStoredGraphShowTheClipNotTheTrackAbove()
    {
        TimelineData tl = emptyTimeline(160, 90);
        ClipData v1 = solidClip(tl, QColor(220, 20, 20), 0, 50);
        ClipData v2 = solidClip(tl, QColor(20, 20, 220), 10, 50);
        const quint64 v1Id = v1.id;
        addClip(tl, TrackType::Video, 0, v1);
        addClip(tl, TrackType::Video, 1, v2);

        TimelineData stored = tl;
        QVERIFY(editops::setClipComposition(stored, v1Id, compops::defaultComposition()));

        for (const bool cpu : {false, true}) {
            Compositor::Options options;
            options.forceCpu = cpu;
            Compositor comp(options);
            const char* name = cpu ? "cpu" : "gpu";
            // Frame 15 is inside the overlap (V2 starts at 10).
            const TimelineData* variants[] = {&tl, &stored};
            for (int k = 0; k < 2; ++k) {
                const auto result = comp.renderCompositionNode(
                    snap(*variants[k]), v1Id, 0, 15, {}, Quality::Full, OutputImage).get();
                QVERIFY(result && result->ok);
                const Px p = px(result->image, 80, 45);
                QVERIFY2(near(p, 220, 20, 20, 255, 6),
                    qPrintable(QStringLiteral("%1 %2 graph: %3")
                                   .arg(name, k ? "stored" : "empty",
                                       QString::fromLatin1(describe(p)))));
            }
            // The timeline itself still shows V2 over V1 there.
            const auto program = comp.render(snap(tl), 15, {}, Quality::Full, OutputImage);
            QVERIFY(program && program->ok);
            QVERIFY(near(px(program->image, 80, 45), 20, 20, 220, 255, 6));
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompViewerView>(argc, argv);
}

#include "test_compviewerview.moc"
