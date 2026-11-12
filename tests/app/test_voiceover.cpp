// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of voice-over recording (docs/behavior-spec.md section 75). There is no
// microphone on the test machine: the input is a generated signal fed through the AudioInput
// interface (a tone with a click at a known sample), the transport is the timeline's playhead.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "media/WavWriter.h"
#include "playback/AudioInput.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/audio/VoiceOverMenu.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaStore.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;

namespace {

constexpr int kRate = 48000;
constexpr int kClickAt = 24000; ///< sample of the click (0.5 s into the take)

/// A microphone that delivers what the test feeds it.
class FakeInput final : public AudioInput
{
public:
    FakeInput(int channels, qint64 latencyUs)
        : m_channels(channels)
        , m_latencyUs(latencyUs)
    { }
    bool start() override
    {
        started = true;
        return true;
    }
    void stop() override { started = false; }
    int sampleRate() const override { return kRate; }
    int channels() const override { return m_channels; }
    qint64 latencyUs() const override { return m_latencyUs; }

    /// Feeds @p seconds of a 440 Hz tone (amplitude 0.25) with a click (0.9) at kClickAt of the
    /// whole signal, in blocks of 480 frames. @p offset: frames fed before.
    void feedTone(double seconds, int64_t offset = 0)
    {
        const int64_t total = int64_t(seconds * kRate);
        for (int64_t at = 0; at < total; at += 480) {
            const int64_t n = std::min<int64_t>(480, total - at);
            std::vector<float> block(size_t(n) * m_channels);
            for (int64_t i = 0; i < n; ++i) {
                const int64_t s = offset + at + i;
                float v = 0.25f * float(std::sin(2.0 * M_PI * 440.0 * double(s) / kRate));
                if (s == kClickAt)
                    v = 0.9f;
                for (int c = 0; c < m_channels; ++c)
                    block[size_t(i) * m_channels + c] = v;
            }
            emit samples(block);
        }
    }
    void feedConstant(float value, int frames)
    {
        emit samples(std::vector<float>(size_t(frames) * m_channels, value));
    }

    bool started = false;

private:
    int m_channels;
    qint64 m_latencyUs;
};

/// Copies stored media to a "project file" folder, like a single-file project does.
class FakeStore final : public MediaStore
{
public:
    explicit FakeStore(const QString& folder)
        : m_folder(folder)
    { }
    bool storesMedia() const override { return true; }
    QString containerPath() const override { return m_folder + QStringLiteral("/film.kinoklip"); }
    bool storeMedia(QStringList& paths, QWidget*) override
    {
        QDir().mkpath(m_folder);
        for (QString& p : paths) {
            const QString target = m_folder + QLatin1Char('/') + QFileInfo(p).fileName();
            QFile::remove(target);
            if (!QFile::copy(p, target))
                return false;
            p = target;
            ++stored;
        }
        return true;
    }
    int stored = 0;

private:
    QString m_folder;
};

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    QTemporaryDir dir;
    FakeInput* input = nullptr; ///< the most recent input made by the factory
    int channels = 1;
    qint64 latencyUs = 0;
    bool devices = true;
    bool saved = false;
    std::unique_ptr<VoiceOverController> vo;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        vo = std::make_unique<VoiceOverController>(
            ctx, [this](const QByteArray&, QString*) -> std::unique_ptr<AudioInput> {
                auto in = std::make_unique<FakeInput>(channels, latencyUs);
                input = in.get();
                return in;
            });
        vo->setDeviceSource(
            [this] {
                return devices ? QList<AudioInputDevice> {{"mic1", "Test Microphone", true},
                                     {"mic2", "Second Microphone", false}}
                               : QList<AudioInputDevice> {};
            },
            [this] {
                return devices ? QString()
                               : QStringLiteral("No microphone or other audio input "
                                                "was found. Connect one and try again.");
            });
        // A saved project: takes go to "Voice Over" next to the file.
        // Two audio tracks (A1, A2), whatever the default timeline has.
        if (project.timeline()->data().audioTracks.size() < 2) {
            project.beginEdit(QStringLiteral("Add Track"));
            editops::addTrack(project.timeline()->mutableData(), TrackType::Audio);
            project.endEdit();
        }
        saved = project.save(dir.filePath(QStringLiteral("film.kkproj")));
    }

    const TimelineData& data() const { return project.timeline()->data(); }
    quint64 a1() const { return data().audioTracks[0].id; }
    quint64 a2() const { return data().audioTracks[1].id; }
    int steps() const { return project.undoStack()->count(); }
    int64_t frameOfUs(int64_t us) const { return usToFrames(us, data().fps); }

    /// A 5 s WAV in the pool and as a clip on A1 at frame 0 (the audio an overwrite hits).
    int64_t placeBackground()
    {
        const QString path = dir.filePath(QStringLiteral("background.wav"));
        media::WavWriter w;
        if (!w.open(path, kRate, 1, 16))
            return 0;
        const std::vector<float> silence(kRate, 0.1f);
        for (int i = 0; i < 5; ++i)
            w.write(silence.data(), kRate);
        w.close();
        const ImportResult r = project.importMedia({path});
        if (r.importedIds.isEmpty())
            return 0;
        const MediaItem* item = project.mediaPool()->item(r.importedIds.first());
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = item->id;
        clip.name = item->name;
        clip.start = 0;
        clip.duration = usToFrames(item->info.durationUs, data().fps);
        clip.sourceDurationUs = item->info.durationUs;
        project.beginEdit(QStringLiteral("Background"));
        editops::overwriteClip(
            project.timeline()->mutableData(), TrackRef {TrackType::Audio, 0}, clip);
        project.endEdit();
        return clip.duration;
    }

    /// Reads a WAV: samples of channel 0 as -1..1, plus the format.
    struct Wav
    {
        int channels = 0, rate = 0, bits = 0;
        std::vector<float> ch0;
        bool ok = false;
    };
    static Wav readWav(const QString& path)
    {
        Wav w;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return w;
        const QByteArray b = f.readAll();
        if (b.size() < 44 || b.left(4) != "RIFF" || b.mid(8, 4) != "WAVE" || b.mid(36, 4) != "data")
            return w;
        auto u16 = [&](int o) { return quint16(quint8(b[o]) | (quint8(b[o + 1]) << 8)); };
        auto u32 = [&](int o) { return quint32(u16(o) | (quint32(u16(o + 2)) << 16)); };
        w.channels = u16(22);
        w.rate = int(u32(24));
        w.bits = u16(34);
        const quint32 dataBytes = u32(40);
        if (qsizetype(dataBytes) != b.size() - 44 || u32(4) != dataBytes + 36)
            return w; // the header sizes must match the file
        const int width = w.bits / 8;
        const int frames = int(dataBytes) / (width * w.channels);
        for (int i = 0; i < frames; ++i) {
            const int o = 44 + i * width * w.channels;
            qint32 v = w.bits == 16 ? qint16(u16(o))
                                    : qint32((quint32(quint8(b[o])) | quint32(quint8(b[o + 1])) << 8
                                                 | quint32(quint8(b[o + 2])) << 16)
                                          << 8)
                    >> 8;
            w.ch0.push_back(float(v) / float(w.bits == 16 ? 32768 : 8388608));
        }
        w.ok = true;
        return w;
    }

    /// Arm A1 and record @p seconds of tone from the playhead (no count-in unless set).
    bool recordTone(double seconds)
    {
        if (!vo->isArmed(a1()) && !vo->arm(a1()))
            return false;
        if (!vo->startRecording())
            return false;
        if (vo->state() == VoiceOverController::State::CountIn) {
            QElapsedTimer t;
            t.start();
            while (vo->state() == VoiceOverController::State::CountIn && t.elapsed() < 15000)
                QTest::qWait(20);
        }
        input->feedTone(seconds);
        vo->stopRecording();
        return !vo->lastTake().path.isEmpty();
    }
};

} // namespace

class TestVoiceOver : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("voiceover"));
    }
    void init()
    {
        QSettings s;
        s.clear();
        s.setValue(QLatin1String(prefs::kVoiceOverCountIn), 0);
        s.setValue(QLatin1String(prefs::kVoiceOverLatencyMs), 0);
        s.setValue(QLatin1String(prefs::kVoiceOverBits), 24);
    }

    // ---- 75.x no microphone ----
    void noDeviceRefusesWithMessage()
    {
        Fixture f;
        QVERIFY(f.saved);
        f.devices = false;
        QSignalSpy messages(f.vo.get(), &VoiceOverController::message);
        QVERIFY(!f.vo->arm(f.a1()));
        QVERIFY(!f.vo->isArmed(f.a1()));
        QVERIFY(f.vo->lastMessage().contains(QLatin1String("microphone")));
        QCOMPARE(messages.count(), 1);
        // The arm button's tooltip names the problem.
        QVERIFY(voiceOverArmTooltip(f.vo.get(), f.a1()).contains(QLatin1String("No microphone")));
        // Record refuses too and makes no take, no undo step, no folder.
        const int before = f.steps();
        QVERIFY(!f.vo->startRecording());
        QCOMPARE(f.vo->state(), VoiceOverController::State::Idle);
        QCOMPARE(f.steps(), before);
        QVERIFY(!QDir(f.vo->takeFolder()).exists());
    }

    void deviceThatCannotOpenRefusesArm()
    {
        Fixture f;
        f.vo = std::make_unique<VoiceOverController>(
            f.ctx, [](const QByteArray&, QString* error) -> std::unique_ptr<AudioInput> {
                *error = QStringLiteral("The microphone is used by another program.");
                return nullptr;
            });
        f.vo->setDeviceSource([] { return QList<AudioInputDevice> {{"mic1", "Mic", true}}; },
            [] { return QString(); });
        QVERIFY(!f.vo->arm(f.a1()));
        QVERIFY(f.vo->lastMessage().contains(QLatin1String("another program")));
        QVERIFY(f.vo->armedTrackId() == 0);
    }

    // ---- 75.x arming and the level meter ----
    void onlyOneTrackIsArmedAndTheInputIsMetered()
    {
        Fixture f;
        QVERIFY(f.vo->arm(f.a1()));
        QVERIFY(f.vo->isArmed(f.a1()));
        QVERIFY(f.input && f.input->started);
        QVERIFY(f.vo->arm(f.a2())); // moves the arm
        QVERIFY(!f.vo->isArmed(f.a1()));
        QVERIFY(f.vo->isArmed(f.a2()));
        f.input->feedConstant(0.5f, 480);
        QVERIFY(std::abs(f.vo->peak() - 0.5f) < 0.001f);
        QVERIFY(!f.vo->clipped());
        f.input->feedConstant(1.0f, 480);
        QVERIFY(f.vo->clipped());
        f.vo->clearClip();
        QVERIFY(!f.vo->clipped());
        // Nothing is recorded while only armed: no file, no undo step.
        QVERIFY(!QDir(f.vo->takeFolder()).exists());
        f.vo->disarm();
        QCOMPARE(f.vo->armedTrackId(), quint64(0));
    }

    void lockedTrackCannotBeArmed()
    {
        Fixture f;
        f.project.beginEdit(QStringLiteral("lock"));
        f.project.timeline()->mutableData().audioTracks[0].locked = true;
        f.project.endEdit();
        QVERIFY(!f.vo->arm(f.a1()));
        QVERIFY(f.vo->lastMessage().contains(QLatin1String("locked")));
    }

    void removedTrackDisarms()
    {
        Fixture f;
        QVERIFY(f.vo->arm(f.a2()));
        f.project.beginEdit(QStringLiteral("remove"));
        f.project.timeline()->mutableData().audioTracks.pop_back();
        f.project.endEdit();
        QCOMPARE(f.vo->armedTrackId(), quint64(0));
    }

    void inputChoicePerTrack()
    {
        Fixture f;
        QCOMPARE(f.vo->inputName(f.a1()), QStringLiteral("Test Microphone"));
        f.vo->setInputFor(f.a1(), "mic2");
        QCOMPARE(f.vo->inputName(f.a1()), QStringLiteral("Second Microphone"));
        QCOMPARE(f.vo->inputName(f.a2()), QStringLiteral("Test Microphone"));
        f.vo->setInputFor(f.a1(), {});
        QCOMPARE(f.vo->inputName(f.a1()), QStringLiteral("Test Microphone"));
    }

    // ---- 75.x count-in ----
    void countInTimingAndNoRecordingBeforeIt()
    {
        Fixture f;
        QSettings().setValue(QLatin1String(prefs::kVoiceOverCountIn), 2);
        QVERIFY(f.vo->arm(f.a1()));
        QList<int> ticks;
        connect(f.vo.get(), &VoiceOverController::countInTick, this, [&](int n) { ticks << n; });
        QElapsedTimer clock;
        clock.start();
        QVERIFY(f.vo->startRecording());
        QCOMPARE(f.vo->state(), VoiceOverController::State::CountIn);
        QCOMPARE(f.vo->countInRemaining(), 2);
        f.input->feedTone(0.2); // sound during the count-in is metered, not recorded
        QVERIFY(f.vo->peak() > 0.2f);
        QTRY_COMPARE_WITH_TIMEOUT(f.vo->state(), VoiceOverController::State::Recording, 5000);
        const qint64 elapsed = clock.elapsed();
        QVERIFY2(elapsed >= 1950 && elapsed < 2400, qPrintable(QString::number(elapsed)));
        QCOMPARE(ticks, (QList<int> {2, 1, 0}));
        QVERIFY(QFileInfo(f.vo->takeFolder() + QStringLiteral("/A1 take 1.wav")).exists());
        f.input->feedConstant(0.1f, 4800);
        f.vo->stopRecording();
        const auto wav = Fixture::readWav(f.vo->lastTake().path);
        QVERIFY(wav.ok);
        QCOMPARE(int(wav.ch0.size()), 4800); // only what came after the count-in
    }

    void cancellingTheCountInMakesNothing()
    {
        Fixture f;
        QSettings().setValue(QLatin1String(prefs::kVoiceOverCountIn), 3);
        QVERIFY(f.vo->arm(f.a1()));
        const int before = f.steps();
        QVERIFY(f.vo->startRecording());
        QVERIFY(f.vo->cancelCountIn());
        QCOMPARE(f.vo->state(), VoiceOverController::State::Idle);
        QTest::qWait(150);
        QCOMPARE(f.steps(), before);
        QVERIFY(!QDir(f.vo->takeFolder()).exists());
        QVERIFY(!f.vo->cancelCountIn());
    }

    // ---- 75.x the take ----
    void takeWavContentAndLength()
    {
        Fixture f;
        QVERIFY(f.recordTone(1.0));
        const auto& take = f.vo->lastTake();
        QCOMPARE(take.path, f.dir.filePath(QStringLiteral("Voice Over/A1 take 1.wav")));
        const auto wav = Fixture::readWav(take.path);
        QVERIFY(wav.ok);
        QCOMPARE(wav.channels, 1);
        QCOMPARE(wav.rate, kRate);
        QCOMPARE(wav.bits, 24);
        QCOMPARE(int(wav.ch0.size()), kRate); // exactly the samples fed
        QVERIFY(std::abs(wav.ch0[kClickAt] - 0.9f) < 0.001f); // the click is where it was fed
        QVERIFY(std::abs(wav.ch0[kClickAt - 1]) < 0.3f);
        QVERIFY(
            std::abs(wav.ch0[100] - 0.25f * std::sin(2.0 * M_PI * 440.0 * 100 / kRate)) < 0.001f);
        // The pool item was probed by FFmpeg: one second long.
        const MediaItem* item = f.project.mediaPool()->item(take.mediaId);
        QVERIFY(item);
        QVERIFY(std::abs(item->info.durationUs - 1000000) < 2000);
    }

    void sixteenBitStereoTake()
    {
        Fixture f;
        QSettings().setValue(QLatin1String(prefs::kVoiceOverBits), 16);
        f.channels = 2;
        QVERIFY(f.recordTone(0.6));
        const auto wav = Fixture::readWav(f.vo->lastTake().path);
        QVERIFY(wav.ok);
        QCOMPARE(wav.channels, 2);
        QCOMPARE(wav.bits, 16);
        QCOMPARE(int(wav.ch0.size()), int(0.6 * kRate));
        QVERIFY(std::abs(wav.ch0[kClickAt] - 0.9f) < 0.001f); // 16 bit: still exact enough
    }

    void clipIsPlacedAtTheStartFrameWithLatencyApplied()
    {
        Fixture f;
        f.latencyUs = 40000; // reported by the device
        QSettings().setValue(QLatin1String(prefs::kVoiceOverLatencyMs), 20); // extra
        f.project.timeline()->setPlayhead(50);
        QVERIFY(f.recordTone(1.0));
        const auto& take = f.vo->lastTake();
        QCOMPARE(take.startFrame, int64_t(50));
        QCOMPARE(take.latencyUs, int64_t(60000));
        const TrackData& a1 = f.data().audioTracks[0];
        QCOMPARE(a1.clips.size(), size_t(1));
        const ClipData& clip = a1.clips[0];
        QCOMPARE(clip.id, take.clipId);
        QCOMPARE(clip.start, int64_t(50));
        QCOMPARE(clip.sourceInUs, int64_t(60000)); // the latency is skipped at the head
        QCOMPARE(clip.mediaId, take.mediaId);
        QCOMPARE(clip.duration, f.frameOfUs(1000000 - 60000));
        QCOMPARE(clip.name, QStringLiteral("A1 take 1"));
        // The click, 0.5 s into the file, shows 0.5 s - 60 ms after the clip start.
        const double clickSeconds = double(kClickAt) / kRate - 0.06;
        QVERIFY(clickSeconds > 0.43 && clickSeconds < 0.45);
    }

    void zeroLatencyClipStartsAtTheHeadOfTheFile()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(10);
        QVERIFY(f.recordTone(1.0));
        const ClipData& clip = f.data().audioTracks[0].clips[0];
        QCOMPARE(clip.start, int64_t(10));
        QCOMPARE(clip.sourceInUs, int64_t(0));
        QCOMPARE(clip.duration, f.frameOfUs(1000000));
    }

    void overwritesWhatIsUnderTheTake()
    {
        Fixture f;
        const int64_t bgFrames = f.placeBackground();
        QVERIFY(bgFrames > 100);
        f.project.timeline()->setPlayhead(30);
        const int before = f.steps();
        QVERIFY(f.recordTone(1.0));
        const TrackData& a1 = f.data().audioTracks[0];
        const ClipData& take = *std::find_if(a1.clips.begin(), a1.clips.end(),
            [&](const ClipData& c) { return c.id == f.vo->lastTake().clipId; });
        QCOMPARE(a1.clips.size(), size_t(3)); // the background is cut in two around the take
        QCOMPARE(a1.clips[0].start, int64_t(0));
        QCOMPARE(a1.clips[0].end(), int64_t(30));
        QCOMPARE(take.start, int64_t(30));
        QCOMPARE(a1.clips[2].start, take.end());
        QCOMPARE(a1.clips[2].end(), bgFrames);
        // The other track is untouched.
        QVERIFY(f.data().audioTracks[1].clips.empty());
        // One undo step brings the background back whole and removes the take from the pool;
        // redo puts it back.
        QCOMPARE(f.steps(), before + 1);
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].clips.size(), size_t(1));
        QCOMPARE(f.data().audioTracks[0].clips[0].duration, bgFrames);
        QVERIFY(!f.project.mediaPool()->item(f.vo->lastTake().mediaId));
        QVERIFY(QFileInfo::exists(f.vo->lastTake().path)); // the WAV stays on disk
        f.project.redo();
        QCOMPARE(f.data().audioTracks[0].clips.size(), size_t(3));
        QVERIFY(f.project.mediaPool()->item(f.vo->lastTake().mediaId));
    }

    void takeIsImportedIntoTheVoiceOverBinAndNumbered()
    {
        Fixture f;
        QVERIFY(f.recordTone(0.3));
        const QString first = f.vo->lastTake().mediaId;
        QCOMPARE(f.vo->lastTake().number, 1);
        QString binId;
        for (const Bin& b : f.project.mediaPool()->bins()) {
            if (b.name == QLatin1String("Voice Over"))
                binId = b.id;
        }
        QVERIFY(!binId.isEmpty());
        QCOMPARE(f.project.mediaPool()->item(first)->binId, binId);
        QCOMPARE(f.project.mediaPool()->item(first)->name, QStringLiteral("A1 take 1.wav"));
        // A second take (further along) is take 2 in the same bin; take 1 stays.
        f.project.timeline()->setPlayhead(200);
        QVERIFY(f.recordTone(0.3));
        QCOMPARE(f.vo->lastTake().number, 2);
        QCOMPARE(f.project.mediaPool()->item(f.vo->lastTake().mediaId)->name,
            QStringLiteral("A1 take 2.wav"));
        QVERIFY(f.project.mediaPool()->item(first));
        QCOMPARE(f.project.mediaPool()->item(f.vo->lastTake().mediaId)->binId, binId);
        int bins = 0;
        for (const Bin& b : f.project.mediaPool()->bins())
            bins += b.name == QLatin1String("Voice Over");
        QCOMPARE(bins, 1);
        // Another track numbers on its own.
        QVERIFY(f.vo->arm(f.a2()));
        f.project.timeline()->setPlayhead(400);
        QVERIFY(f.vo->startRecording());
        f.input->feedTone(0.2);
        f.vo->stopRecording();
        QCOMPARE(f.vo->lastTake().number, 1);
        QVERIFY(f.vo->lastTake().path.endsWith(QLatin1String("A2 take 1.wav")));
        // A new controller finds the files on disk and carries on counting.
        f.vo = std::make_unique<VoiceOverController>(f.ctx);
        QCOMPARE(f.vo->nextTakeFileName(f.a1()), QStringLiteral("A1 take 3.wav"));
    }

    void trackNameIsUsedInTheFileName()
    {
        Fixture f;
        f.project.beginEdit(QStringLiteral("rename"));
        f.project.timeline()->mutableData().audioTracks[0].name = QStringLiteral("Narr/ator");
        f.project.endEdit();
        QVERIFY(f.recordTone(0.2));
        QVERIFY(f.vo->lastTake().path.endsWith(QLatin1String("Voice Over/Narr_ator take 1.wav")));
    }

    void singleFileProjectStoresTheTake()
    {
        Fixture f;
        FakeStore store(f.dir.filePath(QStringLiteral("stored")));
        f.ctx.mediaStore = &store;
        QVERIFY(f.recordTone(0.3));
        QCOMPARE(store.stored, 1);
        const auto& take = f.vo->lastTake();
        QCOMPARE(take.path, f.dir.filePath(QStringLiteral("stored/A1 take 1.wav")));
        QCOMPARE(f.project.mediaPool()->item(take.mediaId)->path, take.path);
        QVERIFY(f.data().audioTracks[0].clips.size() == 1);
        // The aside copy was removed.
        QVERIFY(!QFileInfo::exists(f.vo->takeFolder() + QStringLiteral("/A1 take 1.wav")));
        f.ctx.mediaStore = nullptr;
    }

    void recordingNeedsAnArmedTrackAndStopsWhenTheInputBreaks()
    {
        Fixture f;
        QVERIFY(!f.vo->startRecording());
        QVERIFY(f.vo->lastMessage().contains(QLatin1String("Arm")));
        QVERIFY(f.vo->arm(f.a1()));
        QVERIFY(f.vo->startRecording());
        f.input->feedTone(0.3);
        emit f.input->failed(QStringLiteral("The microphone stopped delivering sound."));
        // What was recorded is kept as a take; the arm is released.
        QCOMPARE(f.vo->state(), VoiceOverController::State::Idle);
        QVERIFY(!f.vo->lastTake().path.isEmpty());
        QCOMPARE(f.data().audioTracks[0].clips.size(), size_t(1));
        QCOMPARE(f.vo->armedTrackId(), quint64(0));
    }

    void emptyTakeIsDiscarded()
    {
        Fixture f;
        QVERIFY(f.vo->arm(f.a1()));
        QVERIFY(f.vo->startRecording());
        const int before = f.steps();
        f.vo->stopRecording();
        QCOMPARE(f.steps(), before);
        QVERIFY(f.vo->lastMessage().contains(QLatin1String("Nothing was recorded")));
        QVERIFY(!QFileInfo::exists(f.vo->takeFolder() + QStringLiteral("/A1 take 1.wav")));
    }

    // ---- 75.x the arm buttons ----
    void armButtonsOnEditAudioAndMixer()
    {
        Fixture f;
        MainWindowOptions options;
        options.autosave = false;
        MainWindow window(f.ctx, options);
        window.resize(1800, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto click = [](TimelineWidget* tl, TrackRef ref) {
            const TimelineLayout& L = tl->layout();
            const int row = L.rowOf(ref);
            QVERIFY(row >= 0);
            const QRect r = L.headerButtonRect(row, HeaderButton::Arm);
            QVERIFY(!r.isEmpty());
            QTest::mouseClick(tl->viewport(), Qt::LeftButton, {}, r.center());
        };
        // Edit page.
        TimelineWidget* edit = window.editPage()->timeline()->timelineWidget();
        const int before = f.steps();
        click(edit, TrackRef {TrackType::Audio, 0});
        QVERIFY(f.vo->isArmed(f.a1()));
        click(edit, TrackRef {TrackType::Audio, 1}); // moves the arm
        QVERIFY(f.vo->isArmed(f.a2()) && !f.vo->isArmed(f.a1()));
        click(edit, TrackRef {TrackType::Audio, 1}); // disarms
        QCOMPARE(f.vo->armedTrackId(), quint64(0));
        QCOMPARE(f.steps(), before); // arming is not an edit
        // Video tracks have no arm button.
        QVERIFY(edit->layout()
                    .headerButtonRect(
                        edit->layout().rowOf(TrackRef {TrackType::Video, 0}), HeaderButton::Arm)
                    .isEmpty());

        // Audio page header and mixer strip share the state.
        f.actions.action(QStringLiteral("view.page_audio"))->trigger();
        QCoreApplication::processEvents();
        click(window.audioPage()->timeline(), TrackRef {TrackType::Audio, 0});
        QVERIFY(f.vo->isArmed(f.a1()));
        MixerStrip* strip = window.audioPage()->mixer()->strip(MixTarget::track(f.a1()));
        QVERIFY(strip->armButton()->isEnabled());
        QVERIFY(strip->armButton()->isChecked());
        strip->armButton()->click(); // disarms
        QVERIFY(!f.vo->isArmed(f.a1()));
        QVERIFY(!strip->armButton()->isChecked());
        strip->armButton()->click();
        QVERIFY(f.vo->isArmed(f.a1()));
    }

    void armButtonWithoutMicrophoneDoesNotArm()
    {
        Fixture f;
        f.devices = false;
        MainWindowOptions options;
        options.autosave = false;
        MainWindow window(f.ctx, options);
        window.resize(1800, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        TimelineWidget* edit = window.editPage()->timeline()->timelineWidget();
        const TimelineLayout& L = edit->layout();
        const QRect r
            = L.headerButtonRect(L.rowOf(TrackRef {TrackType::Audio, 0}), HeaderButton::Arm);
        QSignalSpy messages(f.vo.get(), &VoiceOverController::message);
        // (The window's message box would block: replace the handler by closing it at once.)
        QTimer::singleShot(100, [] {
            if (QWidget* w = QApplication::activeModalWidget())
                w->close();
        });
        QTest::mouseClick(edit->viewport(), Qt::LeftButton, {}, r.center());
        QVERIFY(!f.vo->isArmed(f.a1()));
        QCOMPARE(messages.count(), 1);
    }

    void recordActionIsRegistered()
    {
        Fixture f;
        QVERIFY(f.actions.contains(QStringLiteral("transport.record")));
        // The key is in the default profile and in every other built-in profile, once, and
        // no other command has it there.
        const QDir keymaps(QStringLiteral(KK_TEST_DATA_DIR "/../../resources/keymaps"));
        const QStringList files = keymaps.entryList({QStringLiteral("*.json")});
        QVERIFY2(
            files.size() >= 7, qPrintable(keymaps.absolutePath() + QString::number(files.size())));
        for (const QString& name : files) {
            QFile file(keymaps.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray text = file.readAll();
            QCOMPARE(text.count("\"Ctrl+Alt+R\""), 1);
            QVERIFY2(text.contains("\"transport.record\": [\"Ctrl+Alt+R\"]"), qPrintable(name));
        }
    }
};

QTEST_MAIN(TestVoiceOver)
#include "test_voiceover.moc"
