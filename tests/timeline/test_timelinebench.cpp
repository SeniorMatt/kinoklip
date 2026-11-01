// SPDX-License-Identifier: GPL-3.0-or-later
// Benchmark: full repaint of a 2,000-clip timeline offscreen at several zoom levels, with
// synthetic thumbnail/waveform providers so the media drawing paths are exercised too, plus
// the cost of one live drag update (state copy + moveClips). Prints the timings; the asserts
// are generous upper bounds that only catch gross regressions on a shared machine.
//
// Reference numbers (i5 desktop, offscreen raster, 1920x600, RelWithDebInfo):
// see the qInfo output of this test (`ctest -V -R test_timelinebench`).
#include "TimelineTestUtils.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;

namespace {
constexpr int kClips = 2000;
constexpr int kRepeats = 20;
/// Generous ceiling for one full repaint (ms); real numbers are far below.
constexpr double kMaxRepaintMs = 60.0;
} // namespace

class TestTimelineBench : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinebench"); }

    void repaint2000Clips()
    {
        TimelineFixture f;
        f.project.beginEdit(QStringLiteral("bench"));
        buildManyClips(f.data(), kClips);
        f.project.endEdit();
        KK_VERIFY_TIMELINE(f.view());
        TimelineController c(f.ctx);
        TimelineWidget w(f.ctx, &c);

        // Media-like providers: a constant thumbnail and a synthetic waveform, so the cost
        // measured is the timeline's own drawing, not decoding.
        QImage thumb(96, 54, QImage::Format_ARGB32_Premultiplied);
        thumb.fill(QColor(80, 120, 160));
        w.setThumbnailProvider([&thumb](const QString&, int64_t, int) { return thumb; });
        w.setWaveformProvider(
            [](const QString&, double startUs, double usPerColumn, int columns, float* peaks) {
                for (int i = 0; i < columns; ++i)
                    peaks[i] = float(0.5 + 0.45 * std::sin((startUs + i * usPerColumn) * 1e-5));
                return true;
            });
        // Bench clips are solids; make every 3rd one a media clip so thumbnails/waveforms run.
        MediaItem item;
        item.path = QStringLiteral("/bench/media.mp4");
        item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
        item.info.width = 1920;
        item.info.height = 1080;
        const QString mediaId = f.project.mediaPool()->addItem(item);
        f.project.beginEdit(QStringLiteral("media"));
        int n = 0;
        for (auto* list : {&f.data().videoTracks, &f.data().audioTracks}) {
            for (TrackData& t : *list) {
                for (ClipData& clip : t.clips) {
                    if (n++ % 3 == 0) {
                        clip.kind = ClipKind::Media;
                        clip.mediaId = mediaId;
                    }
                }
            }
        }
        f.project.endEdit();

        w.resize(1920 + 20, 620);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QWidget* vp = w.viewport();
        QImage target(vp->size(), QImage::Format_ARGB32_Premultiplied);

        struct Zoom
        {
            const char* name;
            double ppf; ///< 0 = fit
            double scrollFrame;
        };
        const Zoom zooms[] = {
            {"fit (all clips)", 0, 0},
            {"medium 1 px/frame", 1.0, 5000},
            {"close 8 px/frame", 8.0, 5000},
            {"max 48 px/frame", kMaxPixelsPerFrame, 5000},
        };
        for (const Zoom& z : zooms) {
            if (z.ppf == 0) {
                w.zoomToFit();
            } else {
                w.setPixelsPerFrame(z.ppf, kHeaderWidth);
                w.scrollToFrame(z.scrollFrame);
            }
            {
                QPainter warm(&target);
                vp->render(&warm); // warm caches (elided names, glyphs)
            }
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < kRepeats; ++i) {
                QPainter p(&target);
                vp->render(&p);
            }
            const double ms = timer.nsecsElapsed() / 1e6 / kRepeats;
            qInfo("timeline repaint, %d clips, %-20s: %.2f ms", kClips, z.name, ms);
            QVERIFY2(ms < kMaxRepaintMs, z.name);
        }

        // One live drag update: restore the press-time copy and re-apply a move.
        const TimelineData origin = f.view();
        QVector<quint64> ids;
        for (int i = 0; i < 50; ++i)
            ids.append(origin.videoTracks[0].clips[size_t(i * 10)].id);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < kRepeats; ++i) {
            TimelineData copy = origin;
            editops::moveClips(copy, ids, 3 + i, 0);
        }
        const double dragMs = timer.nsecsElapsed() / 1e6 / kRepeats;
        qInfo("timeline drag update (copy + move 50 of %d clips): %.2f ms", kClips, dragMs);
        QVERIFY(dragMs < kMaxRepaintMs);
    }
};

QTEST_MAIN(TestTimelineBench)
#include "test_timelinebench.moc"
