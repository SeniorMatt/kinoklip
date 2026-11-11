// SPDX-License-Identifier: GPL-3.0-or-later
// GUI-thread load while playing into a selected clip with animated Transform (spec 72). The
// whole main window (Inspector, Keyframes panel, on-screen Transform controls, viewer) follows
// the playhead; the engine's tick and the display run on the same thread. Measured: how long
// the longest stall of the GUI event loop and the frames of
// the keyed clip's first second that never reached the screen, playing across the cut from the
// previous clip (b) against starting on the keyed clip (a), and with the clip unkeyed (c).
// Asserts only relations between the cases (generous margins), never absolute speed.
#include "../playback/PlaybackTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <vector>

using namespace kk;

namespace {

constexpr int kFps = 30;
constexpr int kClipFrames = 90;
constexpr int kLead = 45; ///< frames played in the previous clip before the cut

bool makeFile(const QString& path)
{
    if (QFile::exists(path))
        return true;
    QProcess ffmpeg;
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            "testsrc2=s=1280x720:r=30:d=30", "-f", "lavfi", "-i", "sine=frequency=440:d=30", "-c:v",
            "libx264", "-preset", "veryfast", "-crf", "20", "-g", "250", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-shortest", path + ".tmp.mp4"});
    if (!ffmpeg.waitForFinished(5 * 60 * 1000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(path + ".tmp.mp4", path);
}

struct Result
{
    double lost = 0;
    double stallMs = 0; ///< longest GUI event-loop stall while playing
};

} // namespace

class TestPlayheadLoad : public QObject
{
    Q_OBJECT

    Result play(bool keyed, bool startOnClip)
    {
        QElapsedTimer total;
        total.start();
        Project project;
        const ImportResult imported = project.importMedia({m_file});
        Q_ASSERT(imported.importedIds.size() == 1);
        project.beginEdit(QStringLiteral("t"));
        TimelineData& tl = project.timeline()->mutableData();
        const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
        tl = makeDefaultTimeline(
            QStringLiteral("t"), {kFps, 1}, info.displayWidth, info.displayHeight, 2, 2);
        quint64 keyedId = 0;
        for (int i = 0; i < 2; ++i) {
            ClipData clip;
            clip.id = tl.allocateId();
            clip.mediaId = imported.importedIds[0];
            clip.start = i * kClipFrames;
            clip.duration = kClipFrames;
            clip.sourceDurationUs = info.durationUs;
            clip.sourceInUs = framesToUs(40 + i * (kClipFrames + 120), Rational {kFps, 1});
            clip.linkGroup = quint64(i + 1);
            if (i == 1) {
                keyedId = clip.id;
                if (keyed) {
                    const int at[] = {0, 20, 40, 60};
                    const double zoom[] = {1.39, 1.75, 1.59, 1.39};
                    const double rot[] = {0, 9.2, 6.4, 0};
                    for (int k = 0; k < 4; ++k) {
                        clip.transform.animated(keys::ZoomX, 1.0).setKey(at[k], zoom[k]);
                        clip.transform.animated(keys::ZoomY, 1.0).setKey(at[k], zoom[k]);
                        clip.transform.animated(keys::Rotation, 0.0).setKey(at[k], rot[k]);
                    }
                }
            }
            tl.videoTracks[0].clips.push_back(clip);
            clip.id = tl.allocateId();
            tl.audioTracks[0].clips.push_back(clip);
        }
        project.endEdit();

        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        auto* audio = new test::FakeAudioOutput(10, 40);
        PlaybackEngine::Options options;
        options.audio.reset(audio);
        PlaybackEngine playback(std::move(options));
        playback.setMediaPool(project.mediaPool());
        playback.setTimeline(project.timeline());
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &playback;
        MainWindowOptions mw;
        mw.autosave = false;
        MainWindow window(ctx, mw);
        window.resize(1920, 1080);
        window.show();
        (void)QTest::qWaitForWindowExposed(&window);
        actions.action("view.toggle_keyframes")->trigger();
        if (QAction* a = actions.action("view.onscreen_transform"); a && !a->isChecked())
            a->trigger();
        selection.selectClip(keyedId); // Inspector, Keyframes panel and overlay on this clip
        qInfo("window ready %lld ms", (long long)total.elapsed());
        playback.seek(startOnClip ? kClipFrames : kClipFrames - kLead);
        QTest::qWait(1500);

        std::vector<int64_t> shown;
        connect(&playback, &PlaybackEngine::frameReady, &playback, [&](const RenderedFramePtr& f) {
            if (f && f->ok)
                shown.push_back(f->frame);
        });
        Result r;
        // Event-loop latency probe.
        QElapsedTimer sinceTick;
        QTimer probe;
        probe.setInterval(2);
        probe.setTimerType(Qt::PreciseTimer);
        sinceTick.start();
        connect(&probe, &QTimer::timeout, this, [&] {
            r.stallMs = std::max(r.stallMs, sinceTick.nsecsElapsed() / 1e6 - 2.0);
            sinceTick.restart();
        });
        probe.start();
        const int64_t endFrame = kClipFrames + kFps + 10;
        playback.play();
        (void)QTest::qWaitFor([&] { return playback.position() >= endFrame; }, 20000);
        playback.pause();
        probe.stop();
        qInfo("played, position %lld, %lld ms", (long long)playback.position(),
            (long long)total.elapsed());
        int seen = 0;
        for (const int64_t f : shown)
            seen += (f >= kClipFrames && f < kClipFrames + kFps) ? 1 : 0;
        r.lost = kFps - seen;
        window.hide(); // close() would ask to save the modified project
        qInfo("closed");
        return r;
    }

    Result best(bool keyed, bool startOnClip)
    {
        Result a = play(keyed, startOnClip);
        const Result b = play(keyed, startOnClip);
        return b.lost < a.lost ? b : a;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
        QSettings().clear();
        QVERIFY(m_temp.isValid());
        m_file = m_temp.path() + QStringLiteral("/screen.mp4");
        QVERIFY(makeFile(m_file));
    }

    void crossingIntoSelectedKeyedClip()
    {
        const Result a = best(true, true);
        const Result b = best(true, false);
        const Result c = best(false, false);
        qInfo("a keyed, start on the clip: lost %.0f of %d, longest GUI stall %.1f ms", a.lost,
            kFps, a.stallMs);
        qInfo("b keyed, start in the previous clip: lost %.0f, longest GUI stall %.1f ms", b.lost,
            b.stallMs);
        qInfo("c unkeyed, start in the previous clip: lost %.0f, longest GUI stall %.1f ms", c.lost,
            c.stallMs);
        QVERIFY2(b.lost <= c.lost + 3, "keys make the crossing lose frames");
        QVERIFY2(b.lost <= a.lost + 3, "crossing loses more than starting on the clip");
        QVERIFY2(b.stallMs <= std::max(c.stallMs, a.stallMs) + 60.0,
            "the GUI thread stalls longer when crossing into the keyed clip");
    }

private:
    QTemporaryDir m_temp;
    QString m_file;
};

QTEST_MAIN(TestPlayheadLoad)
#include "test_playheadload.moc"
