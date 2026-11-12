// SPDX-License-Identifier: GPL-3.0-or-later
// (prefix copied from test_editplaying) Editing while the timeline plays (spec 48.7, 48.8; owner request): every timeline command runs
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

class TestPasteAudioLive : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { initTimelineTestCase("test_pasteaudiolive"); }
    void cleanupTestCase() { g_compositor.reset(); }

    /// Spec 16.8 (owner report): many linked clips copied in one timeline and pasted into
    /// another one with fewer audio tracks are heard at once, no reload.
    void spec16_8_pastedAudioIsHeardWithoutReload_data()
    {
        QTest::addColumn<bool>("whilePlaying");
        QTest::newRow("stopped") << false;
        QTest::newRow("playing") << true;
    }
    void spec16_8_pastedAudioIsHeardWithoutReload()
    {
        QFETCH(bool, whilePlaying);
        QTemporaryDir dir;
        const QString wav = dir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeTone(wav, 0.25, 20.0));
        auto device = std::make_unique<CapturingAudioOutput>();
        CapturingAudioOutput* out = device.get();
        Rig r(std::move(device), false);
        Project& p = r.f.project;
        const QString media = p.importMedia({wav}).importedIds.value(0);
        QVERIFY(!media.isEmpty());
        const quint64 first = p.timelineUids().value(0);
        const quint64 second = p.createTimeline(QStringLiteral("B"), QString(), 1, 1);
        QVERIFY(second);
        // Timeline A: four tracks of linked pairs.
        QVERIFY(p.setActiveTimeline(first));
        p.beginEdit(QStringLiteral("setup"));
        TimelineData a = makeDefaultTimeline(QStringLiteral("A"), Rational {25, 1}, 640, 360, 4, 4);
        a.uid = first;
        QVector<quint64> all;
        for (int track = 0; track < 4; ++track) {
            for (int k = 0; k < 30; ++k) {
                ClipData v;
                v.id = a.allocateId();
                v.kind = ClipKind::Media;
                v.mediaId = media;
                v.start = k * 20;
                v.duration = 20;
                v.sourceDurationUs = 20'000'000;
                v.sourceInUs = k * 700'000;
                ClipData au = v;
                au.id = a.allocateId();
                v.linkGroup = au.linkGroup = a.allocateId();
                a.videoTracks[size_t(track)].clips.push_back(v);
                a.audioTracks[size_t(track)].clips.push_back(au);
                all << v.id << au.id;
            }
        }
        r.f.data() = a;
        p.endEdit();
        r.f.selection.setClips(all);
        r.c.copy();
        QVERIFY(!r.c.clipboard().isEmpty());
        // Timeline B: one clip on A1.
        QVERIFY(p.setActiveTimeline(second));
        QCOMPARE(int(r.f.view().audioTracks.size()), 1);
        ClipData one;
        one.kind = ClipKind::Media;
        one.mediaId = media;
        one.start = 0;
        one.duration = 50;
        one.sourceDurationUs = 20'000'000;
        r.put(kA1, one);
        QVERIFY(r.playFrom(0));
        QTest::qWait(300);
        if (!whilePlaying)
            r.engine->pause();
        r.f.selection.clear();
        r.tl()->setPlayhead(100);
        r.c.paste();
        QCOMPARE(int(r.f.view().audioTracks.size()), 4);
        if (whilePlaying) {
            QVERIFY(QTest::qWaitFor([&] { return r.engine->position() > 100; }, 3000));
        } else {
            QVERIFY(r.playFrom(100));
        }
        const int64_t mark = int64_t(out->captured().size());
        QTest::qWait(1200);
        const std::vector<float> got = out->captured();
        QVERIFY2(peak(got, mark + 4800, int64_t(got.size()) - mark - 4800) > 0.1,
            "the pasted audio is silent");
    }
};

QTEST_MAIN(TestPasteAudioLive)
#include "test_pasteaudiolive.moc"
