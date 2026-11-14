// SPDX-License-Identifier: GPL-3.0-or-later
// Editing while the timeline plays (spec 48.7, 48.8; owner request): every timeline command runs
// during playback on the program engine, the edit applies at once, the playhead keeps its
// position, picture and sound follow in step, and each edit stays one undo step. All sound here
// goes through fake sound devices: the ring, the RingPuller and the clock are the real ones, a
// real device's own buffer is not.
#include "../media/MediaTestUtil.h"
#include "../timeline/ParityTestUtils.h"
#include "PlaybackTestUtil.h"
#include "playback/AudioRouting.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/TimelineEval.h"
#include "ui/cut/CutEditor.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <mutex>
#include <numbers>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

namespace {

constexpr int kSamplesPerFrame = 48000 / 25;

/// A sound device (10 ms periods, 40 ms latency) pulling through the real RingPuller and keeping
/// what it pulled: sample n of captured() is what device frame n played.
class CapturingAudioOutput final : public AudioOutput
{
public:
    ~CapturingAudioOutput() override { stop(); }
    bool isAvailable() const override { return true; }
    QString description() const override { return QStringLiteral("capturing fake"); }
    void setVolume(float) override { }
    bool start(AudioRing* ring) override
    {
        stop();
        m_puller = std::make_unique<RingPuller>(ring, false);
        m_startUs = monotonicUs();
        m_played = 0;
        m_atUs = m_startUs;
        {
            std::lock_guard lock(m_mutex);
            m_left.clear();
        }
        m_quit = false;
        m_thread = std::thread([this] { run(); });
        return true;
    }
    void stop() override
    {
        if (m_thread.joinable()) {
            m_quit = true;
            m_thread.join();
        }
    }
    void playedFrames(int64_t* frames, int64_t* atUs) const override
    {
        *atUs = m_atUs.load();
        *frames = m_played.load();
    }
    /// Left channel since start().
    [[nodiscard]] std::vector<float> captured() const
    {
        std::lock_guard lock(m_mutex);
        return m_left;
    }

private:
    void run()
    {
        std::vector<float> buffer;
        int64_t pulled = 0;
        while (!m_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const int64_t now = monotonicUs();
            const auto target = int64_t(double(now - m_startUs) / 1e6 * 48000.0);
            const int64_t want = target - pulled;
            if (want <= 0)
                continue;
            buffer.resize(size_t(want) * 2);
            m_puller->pull(reinterpret_cast<char*>(buffer.data()), size_t(want));
            {
                std::lock_guard lock(m_mutex);
                for (int64_t i = 0; i < want; ++i)
                    m_left.push_back(buffer[size_t(i) * 2]);
            }
            pulled = target;
            m_played = std::max<int64_t>(0, pulled - 48 * 40);
            m_atUs = now;
        }
    }

    std::unique_ptr<RingPuller> m_puller;
    std::thread m_thread;
    std::atomic<bool> m_quit {false};
    std::atomic<int64_t> m_played {0};
    std::atomic<int64_t> m_atUs {0};
    int64_t m_startUs = 0;
    mutable std::mutex m_mutex;
    std::vector<float> m_left;
};

/// Writes a 48 kHz stereo float WAV: a 1 kHz sine of amplitude @p amp.
bool writeTone(const QString& path, double amp, double seconds)
{
    const int n = int(seconds * 48000);
    QByteArray data;
    data.resize(qsizetype(n) * 8);
    auto* f = reinterpret_cast<float*>(data.data());
    for (int i = 0; i < n; ++i) {
        const auto v = float(amp * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
        f[size_t(i) * 2] = v;
        f[size_t(i) * 2 + 1] = v;
    }
    QByteArray h;
    auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
    h.append("RIFF");
    u32(quint32(36 + data.size()));
    h.append("WAVEfmt ");
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(48000);
    u32(48000 * 8);
    u16(8);
    u16(32);
    h.append("data");
    u32(quint32(data.size()));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(h) == h.size()
        && file.write(data) == data.size();
}

/// Peak of @p samples over [@p from, @p from + @p count).
double peak(const std::vector<float>& samples, int64_t from, int64_t count)
{
    double p = 0.0;
    for (int64_t i = std::max<int64_t>(0, from); i < from + count && i < int64_t(samples.size());
         ++i)
        p = std::max(p, double(std::abs(samples[size_t(i)])));
    return p;
}

std::shared_ptr<Compositor> g_compositor;

/// What must hold after any edit: no overlaps, positive lengths, unique ids.
QString inconsistency(const TimelineData& d)
{
    QSet<quint64> ids;
    for (const auto* list : {&d.videoTracks, &d.audioTracks, &d.subtitleTracks}) {
        for (const TrackData& t : *list) {
            int64_t end = 0;
            for (const ClipData& c : t.clips) {
                if (c.duration < 1)
                    return QStringLiteral("clip %1 has length %2").arg(c.id).arg(c.duration);
                if (c.start < end)
                    return QStringLiteral("clip %1 overlaps at %2").arg(c.id).arg(c.start);
                if (ids.contains(c.id))
                    return QStringLiteral("clip id %1 twice").arg(c.id);
                ids.insert(c.id);
                end = c.end();
            }
        }
    }
    return {};
}

/// Program engine wired as in main.cpp, with red / green / blue solids on V1, 100 frames each.
struct Rig : Ctl
{
    std::unique_ptr<PlaybackEngine> engine;
    quint64 red = 0;
    quint64 green = 0;
    quint64 blue = 0;

    explicit Rig(std::unique_ptr<AudioOutput> device = nullptr, bool solids = true)
    {
        if (!g_compositor)
            g_compositor = std::make_shared<Compositor>();
        if (solids) {
            ClipData r = solidClip(0, 100);
            r.solidColor = Qt::red;
            ClipData g = solidClip(100, 100);
            g.solidColor = Qt::green;
            ClipData b = solidClip(200, 100);
            b.solidColor = Qt::blue;
            red = put(kV1, r);
            green = put(kV1, g);
            blue = put(kV1, b);
        }
        PlaybackEngine::Options options;
        options.compositor = g_compositor;
        options.audio = device ? std::move(device) : std::make_unique<FakeAudioOutput>(10, 40);
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        engine->setMediaPool(f.project.mediaPool());
        engine->setTimeline(tl());
        f.ctx.playback = engine.get();
        QObject::connect(
            &f.project, &Project::editCommitted, engine.get(), &PlaybackEngine::noteEditCommitted);
    }
    ~Rig() { f.ctx.playback = nullptr; }

    void stopOnEdit(bool on)
    {
        PlaybackBehaviour b;
        b.stopOnEdit = on;
        engine->setBehaviour(b);
    }
    /// Plays from @p frame until it really runs (the clock has moved).
    bool playFrom(int64_t frame)
    {
        tl()->setPlayhead(frame);
        engine->play();
        return QTest::qWaitFor([&] { return engine->position() > frame; }, 3000);
    }
    /// Main colour of the frame on screen.
    [[nodiscard]] QColor shownColor() const
    {
        const RenderedFramePtr frame = engine->currentFrame();
        if (!frame || frame->image.isNull())
            return {};
        return frame->image.pixelColor(frame->image.width() / 2, frame->image.height() / 2);
    }
};

bool isGreen(const QColor& c)
{
    return c.isValid() && c.green() > 200 && c.red() < 60 && c.blue() < 60;
}

bool isRed(const QColor& c)
{
    return c.isValid() && c.red() > 200 && c.green() < 60 && c.blue() < 60;
}

} // namespace

class TestEditPlaying : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { initTimelineTestCase("test_editplaying"); }
    void cleanupTestCase() { g_compositor.reset(); }

    /// Every command during playback: not refused, one undo step, the playhead runs on from where
    /// it was, in sync; undo and redo during playback do the same; the project stays consistent.
    void spec48_7_everyCommandRunsDuringPlayback_data()
    {
        using Command = std::function<void(Rig&)>;
        QTest::addColumn<Command>("command");
        QTest::addColumn<int>("steps");
        const auto row = [](const char* name, int steps, Command command) {
            QTest::newRow(name) << command << steps;
        };
        const auto selectGreen = [](Rig& r) { r.f.selection.setClips(QVector<quint64> {r.green}); };
        row("ripple delete", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.rippleDeleteSelection();
        });
        row("delete", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.deleteSelection();
        });
        row("ripple cut", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.rippleCut();
        });
        row("split", 1, [](Rig& r) { r.c.splitAtPlayhead(); });
        row("trim start", 1, [](Rig& r) { r.c.trimStartToPlayhead(); });
        row("trim end", 1, [](Rig& r) { r.c.trimEndToPlayhead(); });
        row("ripple trim start", 1, [](Rig& r) { r.c.rippleTrimStartToPlayhead(); });
        row("ripple trim end", 1, [](Rig& r) { r.c.rippleTrimEndToPlayhead(); });
        row("extend edit", 1, [](Rig& r) {
            r.c.setSelectedEdit({r.green, r.blue});
            r.c.extendEditToPlayhead();
        });
        row("nudge", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.nudge(5);
        });
        row("copy and paste", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.copy();
            r.c.paste();
        });
        row("paste insert", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.copy();
            r.c.pasteInsert();
        });
        row("transition", 1, [=](Rig& r) {
            r.c.setSelectedEdit({r.green, r.blue});
            QVERIFY(r.c.addTransition(QStringLiteral("cross_dissolve")));
        });
        row("disable", 1, [=](Rig& r) {
            selectGreen(r);
            r.c.toggleClipsEnabled();
        });
        row("link", 1, [](Rig& r) {
            r.f.selection.setClips(QVector<quint64> {r.green, r.blue});
            r.c.linkSelection();
        });
        row("add track", 1, [](Rig& r) { r.c.addTrack(TrackType::Video); });
        row("remove track", 1, [](Rig& r) { r.c.removeTrack(kA1); });
        row("mark in", 1, [](Rig& r) { r.c.markIn(); });
        row("mark out", 1, [](Rig& r) { r.c.markOut(); });
    }
    void spec48_7_everyCommandRunsDuringPlayback()
    {
        QFETCH(std::function<void(Rig&)>, command);
        QFETCH(int, steps);
        Rig r;
        QVERIFY(r.playFrom(130));
        const int undoBefore = r.f.project.undoStack()->index();
        const TimelineData before = r.f.view();
        const int64_t at = r.engine->position();

        command(r);
        if (QTest::currentTestFailed())
            return;

        QCOMPARE(r.f.project.undoStack()->index(), undoBefore + steps);
        QVERIFY2(!(r.f.view() == before), "the edit did nothing");
        QVERIFY(r.engine->isPlaying());
        QVERIFY2(std::abs(r.engine->position() - at) < 5, "the playhead jumped");
        QVERIFY2(inconsistency(r.f.view()).isEmpty(), qPrintable(inconsistency(r.f.view())));
        const TimelineData edited = r.f.view();
        QTest::qWait(250);
        QVERIFY(r.engine->isPlaying());
        // About 0.25 s later (6 frames), allowing for a loaded machine, never backwards.
        const int64_t later = r.engine->position();
        QVERIFY2(later > at + 1 && later < at + 40,
            qPrintable(QStringLiteral("%1 -> %2").arg(at).arg(later)));
        QVERIFY(r.engine->stats().audioMaster);
        QVERIFY2(std::abs(r.engine->stats().avOffsetFrames) < 3.0,
            qPrintable(QString::number(r.engine->stats().avOffsetFrames)));

        // Undo and redo while playing.
        const int64_t beforeUndo = r.engine->position();
        r.f.project.undo();
        QVERIFY2(r.f.view() == before, "undo did not restore the timeline");
        QVERIFY(r.engine->isPlaying());
        QVERIFY(std::abs(r.engine->position() - beforeUndo) < 5);
        QTest::qWait(120);
        r.f.project.redo();
        QVERIFY2(r.f.view() == edited, "redo did not restore the edit");
        QTest::qWait(120);
        QVERIFY(r.engine->isPlaying());
        QVERIFY2(std::abs(r.engine->stats().avOffsetFrames) < 3.0,
            qPrintable(QString::number(r.engine->stats().avOffsetFrames)));
        QVERIFY2(r.engine->position() > beforeUndo, "playback did not run on through undo");
    }

    /// Source edits on the Edit and the Cut page keep the playhead while playing (paused they
    /// move it after the new clip, spec 4.10, 35.12).
    void spec48_7_sourceEditsKeepThePlayheadWhilePlaying()
    {
        QTemporaryDir dir;
        const QString wav = dir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(wav, 0.25, 10.0));
        Rig r;
        const QString media = r.f.project.importMedia({wav}).importedIds.value(0);
        QVERIFY(!media.isEmpty());
        CutEditor cut(r.f.ctx, &r.c);
        const QVector<SourceSelection> material {{media, 0, 2'000'000}};
        struct Edit
        {
            const char* name;
            std::function<void()> run;
        };
        const std::vector<Edit> edits {
            {"insert", [&] { r.c.insertSource(media, 0, 2'000'000, SourceEditMode::Insert); }},
            {"overwrite",
                [&] { r.c.insertSource(media, 0, 2'000'000, SourceEditMode::Overwrite); }},
            {"append", [&] { r.c.insertSource(media, 0, 2'000'000, SourceEditMode::AppendAtEnd); }},
            {"smart insert", [&] { QVERIFY(cut.run(CutEdit::SmartInsert, material)); }},
            {"cut append", [&] { QVERIFY(cut.run(CutEdit::Append, material)); }},
            {"place on top", [&] { QVERIFY(cut.run(CutEdit::PlaceOnTop, material)); }},
        };
        for (const Edit& edit : edits) {
            QVERIFY2(r.playFrom(130), edit.name);
            const int undoBefore = r.f.project.undoStack()->index();
            const int64_t at = r.engine->position();
            edit.run();
            QVERIFY2(r.f.project.undoStack()->index() == undoBefore + 1, edit.name);
            QVERIFY2(r.engine->isPlaying(), edit.name);
            QTest::qWait(150);
            QVERIFY2(r.engine->position() >= at && r.engine->position() < at + 30, edit.name);
            QVERIFY2(std::abs(r.tl()->playhead() - r.engine->position()) < 4, edit.name);
            QVERIFY2(inconsistency(r.f.view()).isEmpty(), edit.name);
            r.engine->pause();
            QTest::qWait(30);
        }
        // Paused, the playhead still goes after the new clip.
        r.tl()->setPlayhead(0);
        r.c.insertSource(media, 0, 2'000'000, SourceEditMode::Insert);
        QCOMPARE(r.tl()->playhead(), int64_t(50));
    }

    /// Moving a clip with the mouse while playing: one undo step, at the release.
    void spec48_7_dragMoveDuringPlayback()
    {
        View v;
        ClipData c = solidClip(0, 200);
        const quint64 first = v.put(kV1, c);
        v.put(kV1, solidClip(400, 100));
        auto engine = [&] {
            if (!g_compositor)
                g_compositor = std::make_shared<Compositor>();
            PlaybackEngine::Options options;
            options.compositor = g_compositor;
            options.audio = std::make_unique<FakeAudioOutput>(10, 40);
            return std::make_unique<PlaybackEngine>(std::move(options));
        }();
        engine->setTimeline(v.tl());
        v.f.ctx.playback = engine.get();
        connect(&v.f.project, &Project::editCommitted, engine.get(),
            &PlaybackEngine::noteEditCommitted);
        PlaybackBehaviour stop;
        stop.stopOnEdit = true;
        engine->setBehaviour(stop);
        engine->play();
        QVERIFY(QTest::qWaitFor([&] { return engine->position() > 0; }, 3000));
        const int undoBefore = v.f.project.undoStack()->index();
        const QPoint from = v.at(100.5, kV1);
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i) {
            QTest::mouseMove(v.vp(), from + QPoint(100 * i, 0));
            QVERIFY2(engine->isPlaying(), "the moves of a drag are no finished edit");
        }
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, from + QPoint(400, 0));
        QCOMPARE(v.f.project.undoStack()->index(), undoBefore + 1);
        QCOMPARE(ops::clipById(v.f.view(), first)->start, int64_t(40));
        QVERIFY2(!engine->isPlaying(), "the finished move is one edit");
        v.f.ctx.playback = nullptr;
    }

    /// Ripple deleting the clip being played: playback goes on at the same timeline position with
    /// what now sits there, frame by frame, within a few frames, in sync.
    void spec48_7_rippleDeleteOfThePlayingClipPlaysWhatFollows()
    {
        Rig r;
        QVERIFY(r.playFrom(50));
        QTRY_VERIFY_WITH_TIMEOUT(isRed(r.shownColor()), 3000);
        QVector<int64_t> positions;
        const auto conn = connect(r.engine.get(), &PlaybackEngine::positionChanged, this,
            [&](int64_t frame) { positions.append(frame); });
        int64_t firstGreen = -1;
        const auto frames = connect(
            r.engine.get(), &PlaybackEngine::frameReady, this, [&](const RenderedFramePtr& frame) {
                if (firstGreen < 0 && frame && frame->ok
                    && isGreen(frame->image.pixelColor(
                        frame->image.width() / 2, frame->image.height() / 2)))
                    firstGreen = frame->frame;
            });
        const int undoBefore = r.f.project.undoStack()->index();
        r.f.selection.setClips(QVector<quint64> {r.red});
        const int64_t at = r.engine->position();
        r.c.rippleDeleteSelection();
        QCOMPARE(r.f.project.undoStack()->index(), undoBefore + 1);
        QCOMPARE(ops::clipById(r.f.view(), r.green)->start, int64_t(0));
        QCOMPARE(r.f.view().duration(), int64_t(200));
        QTest::qWait(500);
        disconnect(conn);
        disconnect(frames);
        QVERIFY(r.engine->isPlaying());
        QVERIFY(isGreen(r.shownColor()));
        QVERIFY2(firstGreen >= 0, "the following clip never showed");
        qInfo() << "new picture" << firstGreen - at << "frame(s) after the edit";
        QVERIFY2(firstGreen - at <= 8, qPrintable(QString::number(firstGreen - at)));
        // The playhead keeps its timeline position and advances frame by frame.
        int64_t previous = at;
        for (const int64_t p : std::as_const(positions)) {
            QVERIFY2(p >= previous && p - previous <= 4,
                qPrintable(QStringLiteral("%1 after %2").arg(p).arg(previous)));
            previous = p;
        }
        QVERIFY2(previous > at + 5, qPrintable(QStringLiteral("%1 -> %2").arg(at).arg(previous)));
        QVERIFY2(std::abs(r.engine->stats().avOffsetFrames) < 3.0,
            qPrintable(QString::number(r.engine->stats().avOffsetFrames)));

        // Undo during playback: the red clip is back under the same position.
        r.f.project.undo();
        QVERIFY(r.engine->isPlaying());
        QVERIFY(ops::clipById(r.f.view(), r.red));
        QTRY_VERIFY_WITH_TIMEOUT(isRed(r.shownColor()), 500);
        QVERIFY2(r.engine->position() > at, "the playhead went back");
        QVERIFY2(std::abs(r.engine->stats().avOffsetFrames) < 3.0,
            qPrintable(QString::number(r.engine->stats().avOffsetFrames)));
    }

    /// The same on real decoded media: how long the new picture takes, with sound running.
    void spec48_7_rippleDeleteOnDecodedMedia()
    {
#ifdef KK_SANITIZED
        QSKIP("seek timing comparison: sanitized decoders are too slow");
#endif
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Rig r(nullptr, false);
        ImportResult imported = r.f.project.importMedia({fixture("perf_1080p.mp4")});
        QVERIFY(!imported.importedIds.isEmpty());
        const QString id = imported.importedIds[0];
        const media::MediaInfo info = r.f.project.mediaPool()->item(id)->info;
        const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
        quint64 clips[2] = {0, 0};
        r.f.project.beginEdit(QStringLiteral("setup"));
        TimelineData& tl = r.f.data();
        tl = makeDefaultTimeline(QStringLiteral("t"), fps, 960, 540, 1, 1);
        for (int i = 0; i < 2; ++i) {
            ClipData c;
            c.id = clips[i] = tl.allocateId();
            c.mediaId = id;
            c.sourceDurationUs = info.durationUs;
            c.start = 100 * i;
            c.duration = i == 0 ? 100 : 140; // long enough not to reach the end on a slow machine
            c.sourceInUs = framesToUs(50 * i, fps);
            tl.videoTracks[0].clips.push_back(c);
        }
        r.f.project.endEdit();
        QVERIFY(r.playFrom(10));
        QTest::qWait(400);
        r.engine->resetStats();
        const int64_t at = r.engine->position();
        int shownAfterEdit = -1;
        const auto conn = connect(
            r.engine.get(), &PlaybackEngine::frameReady, this, [&](const RenderedFramePtr& frame) {
                if (shownAfterEdit < 0 && frame)
                    shownAfterEdit = int(frame->frame - at);
            });
        QElapsedTimer sinceEdit;
        sinceEdit.start();
        r.f.selection.setClips(QVector<quint64> {clips[0]});
        r.c.rippleDeleteSelection();
        QVERIFY(ops::clipById(r.f.view(), clips[1])->start == 0);
        // The new clip's decoder has to seek: how long depends on the machine and its load.
        QTRY_VERIFY_WITH_TIMEOUT(shownAfterEdit >= 0, 10000);
        const qint64 firstFrameMs = sinceEdit.elapsed();
        disconnect(conn);
        QTest::qWait(800);
        const PlaybackEngine::Stats stats = r.engine->stats();
        QVERIFY(r.engine->isPlaying());
        QVERIFY2(std::abs(stats.avOffsetFrames) < 3.0,
            qPrintable(QString::number(stats.avOffsetFrames)));
        QVERIFY2(r.engine->position() > at, "stopped moving");
        // For comparison: a plain seek to another place of the same source while playing.
        r.engine->resetStats();
        r.engine->seek(r.engine->position() + 40);
        QTRY_VERIFY_WITH_TIMEOUT(r.engine->stats().lastSeekMs > 0.0, 10000);
        const double seekMs = r.engine->stats().lastSeekMs;
        // Undo: both decoders are open now, so this is the cost of the edit itself.
        QTest::qWait(300);
        const int64_t atUndo = r.engine->position();
        int shownAfterUndo = -1;
        const auto undoConn = connect(
            r.engine.get(), &PlaybackEngine::frameReady, this, [&](const RenderedFramePtr& frame) {
                if (shownAfterUndo < 0 && frame)
                    shownAfterUndo = int(frame->frame - atUndo);
            });
        sinceEdit.restart();
        r.f.project.undo();
        QTRY_VERIFY_WITH_TIMEOUT(shownAfterUndo >= 0, 10000);
        const qint64 undoMs = sinceEdit.elapsed();
        disconnect(undoConn);
        qInfo() << "first new frame after" << firstFrameMs << "ms," << shownAfterEdit
                << "frames (decoder opening); after undo" << undoMs << "ms," << shownAfterUndo
                << "frames; plain seek" << seekMs << "ms;" << stats.presented << "shown,"
                << stats.dropped << "dropped; A/V" << stats.avOffsetFrames;
        r.engine->stop();
    }

    /// The sound follows the edit within the ring's kept 43 ms without a device restart and
    /// without a hole, and keeps exactly in step with the timeline: after a ripple delete and
    /// after its undo, the next cut is heard on the device frame that belongs to that cut.
    void spec48_7_soundFollowsTheEditInSync()
    {
        QTemporaryDir dir;
        const QString paths[3] = {dir.filePath(QStringLiteral("a.wav")),
            dir.filePath(QStringLiteral("b.wav")), dir.filePath(QStringLiteral("c.wav"))};
        const double amps[3] = {0.1, 0.3, 0.6};
        for (int i = 0; i < 3; ++i)
            QVERIFY(writeTone(paths[i], amps[i], 10.0));
        auto device = std::make_unique<CapturingAudioOutput>();
        CapturingAudioOutput* out = device.get();
        Rig r(std::move(device), false);
        quint64 ids[3] = {0, 0, 0};
        const ImportResult imported = r.f.project.importMedia({paths[0], paths[1], paths[2]});
        QCOMPARE(imported.importedIds.size(), 3);
        for (int i = 0; i < 3; ++i) {
            ClipData c = solidClip(i * 100, 100);
            c.kind = ClipKind::Media;
            c.mediaId = imported.importedIds[i];
            c.sourceDurationUs = 10'000'000;
            ids[i] = r.put(kA1, c);
        }
        constexpr int64_t kStart = 50;
        r.tl()->setPlayhead(kStart);
        r.engine->play();
        QTRY_VERIFY_WITH_TIMEOUT(out->captured().size() > 9600, 3000); // 200 ms of A
        const auto editPulled = int64_t(out->captured().size());
        const auto level = [&](const std::vector<float>& s, int64_t at) {
            return peak(s, at, 480); // 10 ms holds several periods of the tone
        };

        // Ripple delete A: B moves under the playhead, C's cut comes to frame 100.
        r.f.selection.setClips(QVector<quint64> {ids[0]});
        r.c.rippleDeleteSelection();
        QCOMPARE(ops::clipById(r.f.view(), ids[2])->start, int64_t(100));
        QTRY_VERIFY_WITH_TIMEOUT(
            int64_t(out->captured().size()) > editPulled + 24000, 3000); // 0.5 s on
        std::vector<float> s = out->captured();
        // Old sound, 2048 kept frames + one device period, then B: no later than 100 ms.
        const int64_t newSound = editPulled + 4800;
        QVERIFY2(std::abs(level(s, newSound) - amps[1]) < 0.03,
            qPrintable(QStringLiteral("level %1 at the switch").arg(level(s, newSound))));
#ifndef KK_SANITIZED // sanitized builds play too slowly to keep the audio fed
        // No hole: never silent around the switch.
        for (int64_t at = editPulled - 4800; at < editPulled + 12000; at += 480)
            QVERIFY2(level(s, at) > 0.02,
                qPrintable(QStringLiteral("hole at +%1 frames").arg(at - editPulled)));
#endif
        QVERIFY2(r.engine->isPlaying() && std::abs(r.engine->stats().avOffsetFrames) < 3.0,
            qPrintable(QString::number(r.engine->stats().avOffsetFrames)));

        // In step: device frame n plays timeline sample kStart * rate + n, so B -> C (now at
        // frame 100) is heard at n = 50 frames.
        const int64_t cut = (100 - kStart) * kSamplesPerFrame;
        QTRY_VERIFY_WITH_TIMEOUT(int64_t(out->captured().size()) > cut + 9600, 4000);
        s = out->captured();
        QVERIFY2(std::abs(level(s, cut - 2400) - amps[1]) < 0.03,
            qPrintable(QStringLiteral("before the cut: %1").arg(level(s, cut - 2400))));
        QVERIFY2(std::abs(level(s, cut + 2400) - amps[2]) < 0.03,
            qPrintable(QStringLiteral("after the cut: %1").arg(level(s, cut + 2400))));

        // Undo during playback: at frame ~150 of the old timeline that is B again (C's start is
        // back at 200, heard at n = 150 frames).
        const auto undoPulled = int64_t(out->captured().size());
        QVERIFY2(undoPulled < 148 * kSamplesPerFrame - kStart * kSamplesPerFrame,
            "the machine is too slow for this check");
        r.f.project.undo();
        QVERIFY(r.engine->isPlaying());
        const int64_t back = (200 - kStart) * kSamplesPerFrame;
        QTRY_VERIFY_WITH_TIMEOUT(int64_t(out->captured().size()) > back + 9600, 6000);
        s = out->captured();
        QVERIFY2(std::abs(level(s, undoPulled + 4800) - amps[1]) < 0.03,
            qPrintable(QStringLiteral("after undo: %1").arg(level(s, undoPulled + 4800))));
        QVERIFY2(std::abs(level(s, back - 2400) - amps[1]) < 0.03,
            qPrintable(QStringLiteral("before C: %1").arg(level(s, back - 2400))));
        QVERIFY2(std::abs(level(s, back + 2400) - amps[2]) < 0.03,
            qPrintable(QStringLiteral("after C: %1").arg(level(s, back + 2400))));
        r.engine->stop();
    }

    /// A sound-only edit away from what the ring holds, or a picture edit, leaves the sound alone.
    void spec48_7_soundArrangementOnlyLooksAtTheRing()
    {
        TimelineData a = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180, 1, 1);
        ClipData c = solidClip(0, 100);
        c.id = a.allocateId();
        a.audioTracks[0].clips.push_back(c);
        ClipData d = solidClip(500, 100);
        d.id = a.allocateId();
        a.audioTracks[0].clips.push_back(d);
        TimelineData b = a;
        b.audioTracks[0].clips[1].start = 550; // far away
        QVERIFY(!render::soundArrangementDiffers(a, b, 50, 60));
        QVERIFY(render::soundArrangementDiffers(a, b, 480, 510));
        b = a;
        b.audioTracks[0].clips[0].audio.setBase(keys::VolumeDb, -6.0); // a value
        QVERIFY(!render::soundArrangementDiffers(a, b, 50, 60));
        b = a;
        b.audioTracks[0].clips[0].enabled = false;
        QVERIFY(render::soundArrangementDiffers(a, b, 50, 60));
    }

    /// A link, a colour or a track's lock are no change of pictures: nothing in flight is thrown
    /// away. Real picture changes still are.
    void spec48_7_uiOnlyChangesKeepTheFramesInFlight()
    {
        TimelineData a = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180, 1, 1);
        ClipData c = solidClip(0, 100);
        c.id = a.allocateId();
        a.videoTracks[0].clips.push_back(c);
        TimelineData b = a;
        b.videoTracks[0].clips[0].linkGroup = 7;
        b.videoTracks[0].clips[0].label = Qt::red;
        b.videoTracks[0].clips[0].name = QStringLiteral("renamed");
        b.videoTracks[0].locked = true;
        b.videoTracks[0].autoSelect = false;
        b.videoTracks[0].height = 99;
        QVERIFY(render::samePictures(a, b));
        for (const auto& change : std::vector<std::function<void(ClipData&)>> {
                 [](ClipData& x) { x.enabled = false; }, [](ClipData& x) { x.start = 3; },
                 [](ClipData& x) { x.duration = 50; }, [](ClipData& x) { x.fadeInFrames = 5; },
                 [](ClipData& x) { x.solidColor = Qt::blue; }}) {
            b = a;
            change(b.videoTracks[0].clips[0]);
            QVERIFY(!render::samePictures(a, b));
        }
    }

    /// The end of the timeline follows edits during playback (it stayed where it was when the
    /// play started: an append stopped at the old end, a shortened timeline played on in black).
    void spec48_7_theEndFollowsTheEdit()
    {
        // Longer: playback goes past the old end.
        {
            Rig r;
            QVERIFY(r.playFrom(292));
            r.f.project.beginEdit(QStringLiteral("append"));
            ops::overwriteClip(r.f.data(), kV1, solidClip(300, 200));
            r.f.project.endEdit();
            QTRY_VERIFY_WITH_TIMEOUT(r.engine->position() > 305, 4000);
            QVERIFY(r.engine->isPlaying());
            r.engine->pause();
        }
        // Shorter, the playhead now past the end: normal end handling, on the last frame.
        {
            Rig r;
            QVERIFY(r.playFrom(250));
            r.f.selection.setClips(QVector<quint64> {r.green});
            r.c.rippleDeleteSelection();
            QCOMPARE(r.f.view().duration(), int64_t(200));
            QTRY_VERIFY_WITH_TIMEOUT(!r.engine->isPlaying(), 1000);
            QCOMPARE(r.engine->position(), int64_t(199));
            QCOMPARE(r.tl()->playhead(), int64_t(199));
            // Undo while stopped does not start anything.
            r.f.project.undo();
            QVERIFY(!r.engine->isPlaying());
        }
        // Everything deleted: stops as well, without a crash.
        {
            Rig r;
            QVERIFY(r.playFrom(50));
            r.f.selection.setClips(QVector<quint64> {r.red, r.green, r.blue});
            r.c.rippleDeleteSelection();
            QCOMPARE(r.f.view().duration(), int64_t(0));
            QTRY_VERIFY_WITH_TIMEOUT(!r.engine->isPlaying(), 1000);
            r.f.project.undo();
            QCOMPARE(r.f.view().duration(), int64_t(300));
        }
    }

    /// Reverse, shuttle and a loop whose range changes.
    void spec48_7_otherRatesAndLoops()
    {
        Rig r;
        r.tl()->setPlayhead(150);
        r.engine->setRate(-2.0);
        QTRY_VERIFY_WITH_TIMEOUT(r.engine->position() < 148, 3000);
        r.f.selection.setClips(QVector<quint64> {r.blue});
        r.c.rippleDeleteSelection();
        QVERIFY(r.engine->isPlaying());
        const int64_t at = r.engine->position();
        QTest::qWait(150);
        QVERIFY2(r.engine->position() < at, "reverse play stopped moving");
        r.engine->stop();

        // Loop over the whole timeline: a ripple delete shortens the loop.
        Rig l;
        l.engine->setLoop(true);
        QVERIFY(l.playFrom(250));
        l.f.selection.setClips(QVector<quint64> {l.blue});
        l.c.rippleDeleteSelection();
        QCOMPARE(l.f.view().duration(), int64_t(200));
        QTest::qWait(200);
        QVERIFY(l.engine->isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(l.engine->position() < 199, 1000);
        // Marks made while playing become the new loop at once.
        const int64_t now = l.engine->position();
        l.c.markIn();
        QVERIFY(l.engine->isPlaying());
        QVERIFY(std::abs(l.engine->position() - now) < 5);
        QTRY_VERIFY_WITH_TIMEOUT(l.engine->position() > now + 2, 1000);
        l.engine->stop();

        // Play in to out: a shorter timeline ends the range early.
        Rig io;
        io.setMarks(100, 280);
        io.tl()->setPlayhead(100);
        io.engine->playInToOut();
        QTRY_VERIFY_WITH_TIMEOUT(io.engine->position() > 101, 3000);
        io.f.selection.setClips(QVector<quint64> {io.blue});
        io.c.deleteSelection();
        QVERIFY(io.engine->isPlaying());
        io.engine->stop();
    }

    /// The preference: off keeps playing, on stops at the current frame showing the new timeline.
    void spec48_8_stopPlaybackAfterAnEdit()
    {
        Rig r;
        r.stopOnEdit(true);
        QVERIFY(r.playFrom(50));
        const int64_t at = r.engine->position();
        r.f.selection.setClips(QVector<quint64> {r.red});
        const int undoBefore = r.f.project.undoStack()->index();
        r.c.rippleDeleteSelection();
        QVERIFY2(!r.engine->isPlaying(), "the edit should stop playback");
        QCOMPARE(r.f.project.undoStack()->index(), undoBefore + 1);
        QVERIFY(std::abs(r.engine->position() - at) < 5);
        QCOMPARE(r.tl()->playhead(), r.engine->position());
        QTRY_VERIFY_WITH_TIMEOUT(isGreen(r.shownColor()), 3000);

        // Undo is an edit too.
        QVERIFY(r.playFrom(r.engine->position()));
        r.f.project.undo();
        QVERIFY(!r.engine->isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(isRed(r.shownColor()), 3000);
    }

    /// What does not count as an edit, with the preference on: spec 48.8.
    void spec48_8_valueChangesDoNotStopPlayback()
    {
        Rig r;
        r.stopOnEdit(true);
        QVERIFY(r.playFrom(50));
        const auto stillPlaying = [&](const char* what, const std::function<void()>& run) {
            run();
            QVERIFY2(r.engine->isPlaying(), what);
        };
        stillPlaying("mark in", [&] { r.c.markIn(); });
        stillPlaying("mark out", [&] { r.c.markOut(); });
        stillPlaying("marker", [&] { r.c.addMarker(); });
        stillPlaying("a value in an edit (keyframe, grade...)", [&] {
            r.f.project.beginEdit(QStringLiteral("Opacity"));
            ops::clipById(r.f.data(), r.red)->transform.setBase(keys::ZoomX, 1.2);
            r.f.project.endEdit();
        });
        stillPlaying(
            "track lock", [&] { r.c.toggleTrackFlag(kA1, TimelineController::FlagLocked); });
        stillPlaying("Inspector speed (changes the length)", [&] {
            r.f.project.beginEdit(QStringLiteral("Change Clip Speed"));
            r.f.project.markEditKeepsPlaying();
            ops::clipById(r.f.data(), r.blue)->duration = 50;
            r.f.project.endEdit();
        });
        // The next real edit stops; the baseline is not confused by the ones above.
        r.c.splitAtPlayhead();
        QVERIFY(!r.engine->isPlaying());
    }

    /// Dynamic Trim (spec 8.17, 8.21) keeps its semantics, also with the preference on.
    void spec48_8_dynamicTrimIsNoEdit()
    {
        Rig r;
        r.stopOnEdit(true);
        r.c.setSelectedEdit({r.red, r.green});
        r.c.setDynamicTrim(true);
        QVERIFY(r.playFrom(100));
        QTRY_VERIFY_WITH_TIMEOUT(r.engine->position() > 104, 3000);
        QVERIFY(r.engine->isPlaying());
        const int undoBefore = r.f.project.undoStack()->index();
        r.c.setDynamicTrim(false); // closes the run
        QCOMPARE(r.f.project.undoStack()->index(), undoBefore + 1);
        QVERIFY(r.engine->isPlaying());
        r.engine->stop();
    }

    void spec48_8_preferenceIsStoredAndDefaultsToKeepPlaying()
    {
        QSettings s;
        s.remove(QStringLiteral("playback/stopOnEdit"));
        QVERIFY(!PlaybackBehaviour::fromSettings().stopOnEdit);
        {
            PreferencesDialog dialog(nullptr);
            auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("AfterEditDuringPlayback"));
            QVERIFY(combo);
            QCOMPARE(combo->currentText(), QStringLiteral("Keep playing"));
            QCOMPARE(combo->count(), 2);
            combo->setCurrentIndex(1);
            QCOMPARE(combo->currentText(), QStringLiteral("Stop playback"));
            dialog.accept();
        }
        QVERIFY(PlaybackBehaviour::fromSettings().stopOnEdit);
        s.remove(QStringLiteral("playback/stopOnEdit"));
    }

    /// The ring's rewind: the consumer carries it out, ring positions stay exact.
    void spec48_7_ringRewind()
    {
        AudioRing ring(8192);
        std::vector<float> data(2 * 6000, 1.0f);
        QCOMPARE(ring.write(data.data(), 6000), size_t(6000));
        std::vector<float> out(2 * 1000);
        QCOMPARE(ring.read(out.data(), 1000), size_t(1000));
        ring.requestRewind(2048);
        QVERIFY(ring.rewindPending());
        QCOMPARE(ring.read(out.data(), 1000), size_t(1000));
        QVERIFY(!ring.rewindPending());
        QCOMPARE(ring.rewoundFrames(), size_t(6000 - 1000 - 2048));
        QCOMPARE(ring.available(), size_t(1048));
        // The kept sound fades out to nearly nothing, the first of it is untouched.
        std::vector<float> rest(2 * 1048);
        QCOMPARE(ring.read(rest.data(), 1048), size_t(1048));
        QCOMPARE(rest[0], 1.0f);
        QVERIFY(rest[2 * 1047] < 0.01f);
        // Asked to keep more than there is: nothing is taken back.
        ring.clear();
        QCOMPARE(ring.write(data.data(), 500), size_t(500));
        ring.requestRewind(2048);
        QCOMPARE(ring.read(out.data(), 100), size_t(100));
        QCOMPARE(ring.rewoundFrames(), size_t(0));
        QCOMPARE(ring.available(), size_t(400));
    }
};

QTEST_MAIN(TestEditPlaying)
#include "test_editplaying.moc"
