// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Mix to Track / Bounce Selected Tracks (spec 176): the mix, or one track after its
// fader, rendered with the real mixer into a WAV file, a media pool item and a clip on a new
// audio track; In / Out range, selection rules, cancel and undo.
#include "RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/AudioMixer.h"
#include "ui/audio/AudioBounce.h"

#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

/// Samples of a 24-bit stereo WAV written by media::WavWriter (44-byte header), as floats.
std::vector<float> readWav24(const QString& path, int* channels = nullptr)
{
    QFile f(path);
    std::vector<float> out;
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF")
        return out;
    if (channels)
        *channels = int(quint8(bytes[22]));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData()) + 44;
    const size_t samples = size_t(bytes.size() - 44) / 3;
    out.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
        int v = p[3 * i] | (p[3 * i + 1] << 8) | (p[3 * i + 2] << 16);
        if (v & 0x800000)
            v -= 0x1000000;
        out[i] = float(v) / 8388608.0f;
    }
    return out;
}

float maxDifference(const std::vector<float>& a, const std::vector<float>& b, size_t count)
{
    float worst = 0.0f;
    for (size_t i = 0; i < count; ++i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

class TestAudioBounce : public QObject
{
    Q_OBJECT

    QString m_skip;
    QTemporaryDir m_dir;

    /// A project with the ramp on A1 (frames 0-49) and a quieter copy on A2 (frames 10-59).
    void makeProject(Project& project, QString* mediaId)
    {
        const ImportResult imported = project.importMedia({fixture("mono_ramp_48k.wav")});
        QVERIFY(imported.importedIds.size() == 1);
        *mediaId = imported.importedIds.first();
        project.beginEdit(QStringLiteral("fill"));
        TimelineData& tl = project.timeline()->mutableData();
        ClipData a = mediaClip(tl, *mediaId, 0, 50, 0, 5'000'000);
        QVERIFY(editops::overwriteClip(tl, TrackRef {TrackType::Audio, 0}, a));
        editops::addTrack(tl, TrackType::Audio);
        ClipData b = mediaClip(tl, *mediaId, 10, 50, 1'000'000, 5'000'000);
        b.audio.setBase(keys::VolumeDb, -6.0206);
        QVERIFY(editops::overwriteClip(tl, TrackRef {TrackType::Audio, 1}, b));
        project.endEdit();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
    }

    void mixIntoANewTrack()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Project project;
        QString id;
        makeProject(project, &id);
        QCOMPARE(int(project.timeline()->data().audioTracks.size()), 2);
        const int steps = project.undoStack()->count();

        audiobounce::Options options;
        options.folder = m_dir.filePath(QStringLiteral("mix"));
        options.mix = true;
        double lastProgress = 0.0;
        const audiobounce::Result result = audiobounce::bounce(
            project, options, nullptr, [&](double f) { lastProgress = f; });
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(lastProgress, 1.0);
        QCOMPARE(result.newTrackIds.size(), 1);
        QCOMPARE(result.files.size(), 1);
        QVERIFY(QFileInfo(result.files.front()).isFile());

        const TimelineData& tl = project.timeline()->data();
        QCOMPARE(int(tl.audioTracks.size()), 3);
        const TrackData& added = tl.audioTracks.back();
        QCOMPARE(added.id, result.newTrackIds.front());
        QCOMPARE(added.clips.size(), size_t(1));
        QCOMPARE(added.clips[0].start, int64_t(0));
        QCOMPARE(added.clips[0].duration, int64_t(60)); // the timeline's length
        QVERIFY(!added.name.isEmpty());
        QCOMPARE(project.undoStack()->count(), steps + 1); // one undo step

        // The media pool item sits in the bin "Bounced Audio".
        const MediaItem* item = project.mediaPool()->item(added.clips[0].mediaId);
        QVERIFY(item);
        QCOMPARE(item->path, result.files.front());
        bool inBin = false;
        for (const Bin& bin : project.mediaPool()->bins())
            inBin = inBin || bin.name == QLatin1String("Bounced Audio");
        QVERIFY(inBin);

        // The file is the main mix: the same samples the mixer gives.
        int channels = 0;
        const std::vector<float> wav = readWav24(result.files.front(), &channels);
        QCOMPARE(channels, 2);
        const int64_t total = framesToSamples48k(60, tl.fps);
        QCOMPARE(int64_t(wav.size() / 2), total);
        // Mix the original two tracks (the bounce is not part of this check).
        TimelineData before = tl;
        before.audioTracks.pop_back();
        AudioMixer mixer;
        mixer.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        std::vector<float> expected(size_t(total) * 2);
        mixer.mix(before, 0, int(total), expected.data());
        QVERIFY2(maxDifference(wav, expected, size_t(total) * 2) < 1e-5f,
            qPrintable(QString::number(maxDifference(wav, expected, size_t(total) * 2))));
        float peak = 0.0f;
        for (const float v : wav)
            peak = std::max(peak, std::abs(v));
        QVERIFY(peak > 0.1f); // not silence

        // Undo removes the track and the clip in one step.
        project.undo();
        QCOMPARE(int(project.timeline()->data().audioTracks.size()), 2);
    }

    void selectedTracksEachIntoATrack()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Project project;
        QString id;
        makeProject(project, &id);
        const TimelineData& tl = project.timeline()->data();
        const quint64 a1 = tl.audioTracks[0].id;
        const quint64 a2 = tl.audioTracks[1].id;
        SelectionModel selection;

        // Selected clip decides; with nothing selected the tracks with the selector on.
        selection.setClips({tl.audioTracks[1].clips[0].id});
        QCOMPARE(audiobounce::selectedAudioTracks(tl, selection), QVector<quint64>({a2}));
        selection.setClips({});
        QCOMPARE(audiobounce::selectedAudioTracks(tl, selection), QVector<quint64>({a1, a2}));
        project.beginEdit(QStringLiteral("selector"));
        project.timeline()->mutableData().audioTracks[0].autoSelect = false;
        project.endEdit();
        QCOMPARE(audiobounce::selectedAudioTracks(project.timeline()->data(), selection),
            QVector<quint64>({a2}));
        project.undo();

        audiobounce::Options options;
        options.folder = m_dir.filePath(QStringLiteral("tracks"));
        options.trackIds = {a1, a2};
        const audiobounce::Result result = audiobounce::bounce(project, options);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.newTrackIds.size(), 2);
        QCOMPARE(result.files.size(), 2);
        QVERIFY(result.files[0] != result.files[1]);
        const TimelineData& after = project.timeline()->data();
        QCOMPARE(int(after.audioTracks.size()), 4);

        // Each file is its track alone: A2 starts at frame 10 and is 6 dB down.
        const std::vector<float> first = readWav24(result.files[0]);
        const std::vector<float> second = readWav24(result.files[1]);
        const int64_t total = framesToSamples48k(60, after.fps);
        QCOMPARE(int64_t(first.size() / 2), total);
        TimelineData before = after;
        before.audioTracks.resize(2);
        AudioMixer mixer;
        mixer.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        std::vector<float> tap(size_t(total) * 2), out(size_t(total) * 2);
        mixer.setTap(MixTarget::track(a1), tap.data());
        mixer.mix(before, 0, int(total), out.data());
        QVERIFY(maxDifference(first, tap, size_t(total) * 2) < 1e-5f);
        AudioMixer mixer2;
        mixer2.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        mixer2.setTap(MixTarget::track(a2), tap.data());
        mixer2.mix(before, 0, int(total), out.data());
        QVERIFY(maxDifference(second, tap, size_t(total) * 2) < 1e-5f);
        QVERIFY(maxDifference(first, second, size_t(total) * 2) > 0.05f); // really different
        // Before frame 10 the second track is silent.
        const size_t silentSamples = size_t(framesToSamples48k(10, after.fps)) * 2;
        for (size_t i = 0; i < silentSamples; ++i)
            QVERIFY(std::abs(second[i]) < 1e-6f);
    }

    void inOutRangeAndNothingToBounce()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Project project;
        QString id;
        makeProject(project, &id);
        project.beginEdit(QStringLiteral("marks"));
        project.timeline()->mutableData().markIn = 20;
        project.timeline()->mutableData().markOut = 45;
        project.endEdit();
        audiobounce::Options options;
        options.folder = m_dir.filePath(QStringLiteral("range"));
        options.mix = true;
        const audiobounce::Result result = audiobounce::bounce(project, options);
        QVERIFY2(result.ok, qPrintable(result.error));
        const TrackData& added = project.timeline()->data().audioTracks.back();
        QCOMPARE(added.clips[0].start, int64_t(20));
        QCOMPARE(added.clips[0].duration, int64_t(25));
        const std::vector<float> wav = readWav24(result.files.front());
        QCOMPARE(int64_t(wav.size() / 2), framesToSamples48k(45, project.timeline()->data().fps)
                - framesToSamples48k(20, project.timeline()->data().fps));

        // No tracks named: a refusal, nothing changes.
        const int tracks = int(project.timeline()->data().audioTracks.size());
        audiobounce::Options none;
        none.folder = options.folder;
        const audiobounce::Result refused = audiobounce::bounce(project, none);
        QVERIFY(!refused.ok);
        QVERIFY(!refused.error.isEmpty());
        QCOMPARE(int(project.timeline()->data().audioTracks.size()), tracks);

        // An empty timeline has nothing to bounce.
        Project empty;
        audiobounce::Options emptyOptions;
        emptyOptions.folder = options.folder;
        emptyOptions.mix = true;
        QVERIFY(!audiobounce::bounce(empty, emptyOptions).ok);
    }

    void cancelLeavesNothing()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Project project;
        QString id;
        makeProject(project, &id);
        const int steps = project.undoStack()->count();
        audiobounce::Options options;
        options.folder = m_dir.filePath(QStringLiteral("cancel"));
        options.mix = true;
        std::atomic<bool> cancel(false);
        const audiobounce::Result result = audiobounce::bounce(
            project, options, &cancel, [&](double f) { cancel = f > 0.3; });
        QVERIFY(!result.ok);
        QVERIFY(result.cancelled);
        QCOMPARE(int(project.timeline()->data().audioTracks.size()), 2);
        QCOMPARE(project.undoStack()->count(), steps);
        QVERIFY(QDir(options.folder).entryList(QDir::Files).isEmpty());
        // A file name that is taken is not overwritten.
        const audiobounce::Result first = audiobounce::bounce(project, options);
        QVERIFY2(first.ok, qPrintable(first.error));
        const audiobounce::Result second = audiobounce::bounce(project, options);
        QVERIFY2(second.ok, qPrintable(second.error));
        QVERIFY(first.files.front() != second.files.front());
    }
};

QTEST_GUILESS_MAIN(TestAudioBounce)
#include "test_audiobounce.moc"
