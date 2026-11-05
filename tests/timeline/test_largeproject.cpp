// SPDX-License-Identifier: GPL-3.0-or-later
// Stress benchmark (polish round 4, docs/polish-v3.md "Large-project stress pass"): a one hour
// project with 8 video and 8 audio tracks and about 1,500 clips (tests/timeline/LargeProject.h)
// in the Edit page's widgets (timeline, Edit Index). Measures project load / save and
// autosave, timeline paint while scrolling and zooming, edit latency (blade, ripple delete,
// undo, redo) including the views' updates, memory, Edit Index rebuilds and the timeline
// animator. Prints every number (`ctest -V -R test_largeproject`); the asserts are generous
// ceilings that only catch gross regressions on a loaded machine.
#include "LargeProject.h"
#include "TimelineTestUtils.h"
#include "app/AutosaveController.h"
#include "core/Preferences.h"
#include "ui/editindex/EditIndexModel.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/timeline/TimelineAnimator.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;

namespace {

// Ceilings (ms). The measured numbers are recorded in docs/polish-v3.md; these are several
// times higher so that a loaded CI machine does not fail them.
constexpr double kMaxSaveMs = 400.0;
constexpr double kMaxLoadMs = 600.0;
constexpr double kMaxAutosaveBlockMs = 50.0; ///< the GUI-thread part of an autosave
constexpr double kMaxPaintMs = 60.0;
constexpr double kMaxEditMs = 100.0; ///< one edit including the views' updates
constexpr double kMaxIndexRebuildMs = 60.0;
constexpr double kMaxAnimatorMs = 30.0;
constexpr double kMaxUndoStepMb = 8.0; ///< memory one more undo step may add
constexpr int kRepeats = 10;

/// Resident set size of this process in MB (Linux /proc; 0 elsewhere).
double residentMb()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return 0.0;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith("VmRSS:"))
            return line.mid(6).trimmed().split(' ').value(0).toDouble() / 1024.0;
    }
    return 0.0;
}

/// Mean wall time of @p repeats runs of @p fn, in ms.
double timeMs(const std::function<void()>& fn, int repeats = 1)
{
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < repeats; ++i)
        fn();
    return t.nsecsElapsed() / 1e6 / repeats;
}

} // namespace

class TestLargeProject : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_largeproject");
        prefs::setAnimationsEnabled(true); // the default; the animator is part of the cost
    }

    void buildAndMemory()
    {
        const double before = residentMb();
        TimelineFixture f;
        const LargeProjectStats s = buildLargeProject(f.project);
        KK_VERIFY_TIMELINE(f.view());
        QVERIFY(s.clips >= 1400);
        const double built = residentMb();
        qInfo("stress project: %d clips, %d transitions, %d keyframes, %d markers, %lld frames",
            s.clips, s.transitions, s.keyframes, s.markers, qint64(f.view().duration()));

        // Undo history growth: each committed edit keeps its before / after snapshots.
        TimelineController c(f.ctx);
        f.project.timeline()->setPlayhead(1000);
        const double edits0 = residentMb();
        constexpr int kEdits = 20;
        for (int i = 0; i < kEdits; ++i) {
            f.project.timeline()->setPlayhead(1000 + i * 1777);
            c.splitAtPlayhead();
        }
        const double perStep = (residentMb() - edits0) / kEdits;
        qInfo("memory: %.1f MB before, %.1f MB with the project, +%.2f MB per undo step "
              "(%d steps)",
            before, built, perStep, int(f.project.undoStack()->count()));
        QVERIFY2(perStep < kMaxUndoStepMb, qPrintable(QString::number(perStep)));
    }

    void saveLoadAutosave()
    {
        TimelineFixture f;
        buildLargeProject(f.project);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("stress.kkproj"));

        const double saveMs = timeMs([&] { QVERIFY(f.project.save(path)); }, 3);
        const qint64 bytes = QFileInfo(path).size();
        Project loaded;
        const double loadMs = timeMs([&] { QVERIFY(loaded.load(path)); }, 3);
        QCOMPARE(loaded.timeline()->data().videoTracks.size(), size_t(8));

        // Autosave: the time the GUI thread is blocked by the timer's autosave
        // (AutosaveController::startAutosave(); the write itself runs in the background).
        f.project.beginEdit(QStringLiteral("Dirty"));
        f.project.timeline()->mutableData().markIn = 10;
        f.project.endEdit();
        AutosaveController autosave(&f.project);
        double autosaveMs = 0.0;
        for (int i = 0; i < 3; ++i) {
            f.project.beginEdit(QStringLiteral("Dirty"));
            f.project.timeline()->mutableData().markIn = 11 + i;
            f.project.endEdit();
            autosaveMs = std::max(autosaveMs, timeMs([&] { QVERIFY(autosave.startAutosave()); }));
            QVERIFY(autosave.waitForPendingWrite());
            QVERIFY(QFileInfo::exists(autosave.autosavePath()));
        }
        autosave.discard();
        qInfo("save %.1f ms (%.1f MB), load %.1f ms, autosave blocks the GUI thread %.1f ms",
            saveMs, bytes / 1e6, loadMs, autosaveMs);
        QVERIFY2(saveMs < kMaxSaveMs, qPrintable(QString::number(saveMs)));
        QVERIFY2(loadMs < kMaxLoadMs, qPrintable(QString::number(loadMs)));
        QVERIFY2(autosaveMs < kMaxAutosaveBlockMs, qPrintable(QString::number(autosaveMs)));
    }

    void paintScrollAndZoom()
    {
        TimelineFixture f;
        buildLargeProject(f.project);
        TimelineController c(f.ctx);
        TimelineWidget w(f.ctx, &c);
        QImage thumb(96, 54, QImage::Format_ARGB32_Premultiplied);
        thumb.fill(QColor(80, 120, 160));
        w.setThumbnailProvider([&thumb](const QString&, int64_t, int) { return thumb; });
        w.setWaveformProvider(
            [](const QString&, double startUs, double usPerColumn, int columns, float* peaks,
                float* rms) {
                for (int i = 0; i < columns; ++i) {
                    peaks[i] = float(0.5 + 0.45 * std::sin((startUs + i * usPerColumn) * 1e-5));
                    rms[i] = 0.5f * peaks[i];
                }
                return true;
            });
        w.resize(1920 + 20, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QWidget* vp = w.viewport();
        QImage target(vp->size(), QImage::Format_ARGB32_Premultiplied);
        auto paint = [&] {
            QPainter p(&target);
            vp->render(&p);
        };

        struct Case
        {
            const char* name;
            double ppf; ///< 0 = zoom to fit
        };
        for (const Case& z : {Case {"fully zoomed out", 0}, Case {"normal 2 px/frame", 2.0},
                 Case {"close 10 px/frame", 10.0}}) {
            if (z.ppf == 0)
                w.zoomToFit();
            else
                w.setPixelsPerFrame(z.ppf, kHeaderWidth);
            w.scrollToFrame(0);
            paint(); // warm caches
            // Scrolling: one paint per scroll step of a quarter view.
            double worst = 0.0;
            double sum = 0.0;
            for (int i = 0; i < kRepeats; ++i) {
                w.scrollToFrame(double(i) * 4000.0);
                const double ms = timeMs(paint);
                worst = std::max(worst, ms);
                sum += ms;
            }
            qInfo("paint while scrolling, %-18s: mean %.2f ms, worst %.2f ms", z.name,
                sum / kRepeats, worst);
            QVERIFY2(worst < kMaxPaintMs, z.name);
        }
        // Zooming in steps from fully out to close, one paint per step.
        w.zoomToFit();
        double worst = 0.0;
        double ppf = 0.02;
        for (int i = 0; i < 12; ++i, ppf *= 1.8) {
            w.setPixelsPerFrame(ppf, 900);
            worst = std::max(worst, timeMs(paint));
        }
        qInfo("paint while zooming (12 steps): worst %.2f ms", worst);
        QVERIFY(worst < kMaxPaintMs);
    }

    void editLatency()
    {
        TimelineFixture f;
        buildLargeProject(f.project);
        TimelineController c(f.ctx);
        TimelineWidget w(f.ctx, &c);
        EditIndexPanel index(f.ctx);
        w.resize(1600, 700);
        w.show();
        index.resize(800, 400);
        index.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QWidget* vp = w.viewport();
        QImage target(vp->size(), QImage::Format_ARGB32_Premultiplied);
        w.setPixelsPerFrame(2.0, kHeaderWidth);
        // The edit, the queued view updates (Edit Index rebuild, animator) and one paint.
        auto settle = [&] {
            QCoreApplication::processEvents();
            QPainter p(&target);
            vp->render(&p);
        };
        settle();

        double blade = 0, ripple = 0, undo = 0, redo = 0, opOnly = 0;
        for (int i = 0; i < kRepeats; ++i) {
            const int64_t at = 30000 + i * 997;
            f.project.timeline()->setPlayhead(at);
            w.scrollToFrame(double(at - 200));
            settle();
            blade += timeMs([&] {
                c.splitAtPlayhead();
                settle();
            });
            const ClipData* clip = f.view().videoTracks[0].clipAt(at + 5);
            QVERIFY(clip);
            f.selection.setClips({clip->id});
            settle();
            const quint64 before = f.project.undoStack()->count();
            opOnly += timeMs([&] { c.rippleDeleteSelection(); });
            settle();
            QCOMPARE(f.project.undoStack()->count(), before + 1);
            f.project.undo();
            settle();
            ripple += timeMs([&] {
                f.project.redo();
                settle();
            });
            undo += timeMs([&] {
                f.project.undo();
                settle();
            });
            redo += timeMs([&] {
                f.project.redo();
                settle();
            });
        }
        qInfo("edit latency incl. views and one paint: blade %.1f ms, ripple delete %.1f ms "
              "(edit alone %.1f ms), undo %.1f ms, redo %.1f ms",
            blade / kRepeats, ripple / kRepeats, opOnly / kRepeats, undo / kRepeats,
            redo / kRepeats);
        for (const double ms : {blade, ripple, undo, redo})
            QVERIFY2(ms / kRepeats < kMaxEditMs, qPrintable(QString::number(ms / kRepeats)));
    }

    void editIndexRebuild()
    {
        TimelineFixture f;
        buildLargeProject(f.project);
        EditIndexModel model(&f.project);
        model.setMode(EditIndexModel::Mode::Edits);
        const double edits = timeMs([&] { model.rebuild(); }, kRepeats);
        const int eventRows = model.rowCount();
        model.setMode(EditIndexModel::Mode::Markers);
        const double markers = timeMs([&] { model.rebuild(); }, kRepeats);
        // The whole panel (sort / filter proxy and view) after one edit.
        EditIndexPanel panel(f.ctx);
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const double panelMs = timeMs([&] { panel.rebuildNow(); }, kRepeats);
        qInfo("Edit Index rebuild: %d events %.2f ms, %d markers %.2f ms, panel (proxy + view) "
              "%.2f ms",
            eventRows, edits, model.rowCount(), markers, panelMs);
        QVERIFY(edits < kMaxIndexRebuildMs);
        QVERIFY(markers < kMaxIndexRebuildMs);
        QVERIFY(panelMs < kMaxIndexRebuildMs);
    }

    void animatorCost()
    {
        TimelineFixture f;
        buildLargeProject(f.project);
        TimelineLayout layout;
        layout.setViewSize(QSize(1600, 700));
        layout.rebuild(f.view());
        TimelineAnimator anim([] {});
        Timeline* t = f.project.timeline();
        anim.resync(t->plainSnapshot(), 1, t->revision());
        // A ripple delete near the start moves almost every clip on its tracks: the worst case
        // for the per-clip comparison.
        TimelineController c(f.ctx);
        f.selection.setClips({f.view().videoTracks[0].clips[3].id});
        c.rippleDeleteSelection();
        const double ms
            = timeMs([&] { anim.modelChanged(t->plainSnapshot(), 1, t->revision(), layout); });
        const double tick = timeMs([&] { anim.tick(); });
        qInfo("animator: modelChanged after a ripple delete %.2f ms, tick %.3f ms", ms, tick);
        QVERIFY(ms < kMaxAnimatorMs);
    }
};

QTEST_MAIN(TestLargeProject)
#include "test_largeproject.moc"
