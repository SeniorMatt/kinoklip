// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for Clip > Normalize Audio Levels (spec 37.35): the measurements (sample peak, true
// peak, BS.1770 loudness) of generated WAV files, the gain rules (relative / independent), the
// volume change (keys move with the constant), the dialog flow (one undo step, progress,
// cancel) and the command's targets.
#include "../export/ExportTestUtil.h"
#include "../timeline/TimelineTestUtils.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "render/AudioNormalize.h"
#include "ui/dialogs/NormalizeAudioDialog.h"

#include <QProgressBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <numbers>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

constexpr int kRate = 48000;

/// A 1 kHz sine at @p peak (linear) for @p seconds.
bool writeSine(const QString& path, double peak, double seconds)
{
    return writeWav(path, int(seconds * kRate),
        [peak](int i) { return peak * std::sin(2.0 * std::numbers::pi * 1000.0 * i / kRate); });
}

double db(double linear)
{
    return 20.0 * std::log10(linear);
}

} // namespace

class TestAudioNormalize : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_audionormalize");
        QVERIFY(m_dir.isValid());
        QVERIFY(writeSine(m_dir.filePath(QStringLiteral("loud.wav")), 0.5, 4.0)); // -6 dBFS
        QVERIFY(writeSine(m_dir.filePath(QStringLiteral("quiet.wav")), 0.125, 4.0)); // -18
    }

    void measuresPeaksAndLoudness()
    {
        audionorm::Source s;
        s.path = m_dir.filePath(QStringLiteral("loud.wav"));
        s.startUs = 0;
        s.endUs = 4'000'000;
        double lastProgress = 0.0;
        const audionorm::Measurement m
            = audionorm::measure(s, nullptr, [&](double f) { lastProgress = f; });
        QVERIFY2(m.valid, qPrintable(m.error));
        QVERIFY(std::abs(m.samplePeakDb - db(0.5)) < 0.05);
        QVERIFY(std::abs(m.truePeakDb - db(0.5)) < 0.3); // a sine's peaks fall between samples
        // A 1 kHz sine in both channels: about its peak level in LUFS (K-weighting adds
        // ~+0.7 dB at 1 kHz, the 0.5 mean square of a sine takes ~-3 dB, two channels +3 dB).
        QVERIFY2(std::abs(m.loudnessLufs - (db(0.5) + 0.0)) < 1.0,
            qPrintable(QString::number(m.loudnessLufs)));
        QCOMPARE(lastProgress, 1.0);
        QCOMPARE(audionorm::levelOf(m, audionorm::Mode::SamplePeak), m.samplePeakDb);
        QCOMPARE(audionorm::levelOf(m, audionorm::Mode::Loudness), m.loudnessLufs);
        // Only the source range is measured: half of the file is still the same sine.
        s.endUs = 2'000'000;
        QVERIFY(std::abs(audionorm::measure(s).samplePeakDb - db(0.5)) < 0.05);
    }

    void cancelStopsTheMeasurement()
    {
        audionorm::Source s;
        s.path = m_dir.filePath(QStringLiteral("loud.wav"));
        s.endUs = 4'000'000;
        std::atomic<bool> cancel {true};
        QVERIFY(!audionorm::measure(s, &cancel).valid);
        s.path = m_dir.filePath(QStringLiteral("missing.wav"));
        QVERIFY(!audionorm::measure(s).valid);
    }

    void gainRules()
    {
        const std::vector<double> levels {-6.0, -18.0, -200.0};
        const auto independent = audionorm::gains(levels, -9.0, audionorm::Scope::Independent);
        QCOMPARE(independent[0], -3.0);
        QCOMPARE(independent[1], 9.0);
        QVERIFY(std::isnan(independent[2])); // silence: left alone
        const auto relative = audionorm::gains(levels, -9.0, audionorm::Scope::Relative);
        QCOMPARE(relative[0], -3.0);
        QCOMPARE(relative[1], -3.0); // keeps its 12 dB below the loudest
        QVERIFY(std::isnan(relative[2]));
        QCOMPARE(audionorm::defaultTarget(audionorm::Mode::Loudness), -23.0);
    }

    void setClipGainKeepsTheAutomationShape()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("T"), {25, 1}, 1920, 1080, 2, 2);
        ClipData c = solidClip(0, 100);
        AnimatedValue& v = c.audio.animated(keys::VolumeDb, 0.0);
        v.base = -2.0;
        v.setKey(0, -10.0);
        v.setKey(50, -2.0);
        const quint64 id = ops::overwriteClip(d, {TrackType::Audio, 0}, c);
        QVERIFY(levelops::setClipGain(d, id, 4.0));
        const AnimatedValue& after = ops::clipById(d, id)->audio.values.value(keys::VolumeDb);
        QCOMPARE(after.base, 4.0);
        QCOMPARE(after.keys[0].value, -4.0);
        QCOMPARE(after.keys[1].value, 4.0);
        QVERIFY(!levelops::setClipGain(d, id, 4.0)); // no change
        QVERIFY(!levelops::setClipGain(d, 9999, 1.0));
    }

    void dialogNormalizesAsOneUndoStep()
    {
        TimelineFixture f;
        const quint64 loud = addAudioClip(f, QStringLiteral("loud.wav"), 0);
        const quint64 quiet = addAudioClip(f, QStringLiteral("quiet.wav"), 100);
        // Video selected too: only audio clips are targets, in timeline order.
        f.selection.setClips({quiet, loud});
        const QVector<quint64> targets = normalizeTargets(f.project, f.selection);
        QCOMPARE(targets, (QVector<quint64> {loud, quiet}));

        const int before = f.project.undoStack()->count();
        NormalizeAudioDialog dialog(&f.project, targets);
        dialog.setMode(audionorm::Mode::SamplePeak);
        QCOMPARE(dialog.target(), -9.0);
        dialog.setScope(audionorm::Scope::Independent);
        dialog.show();
        dialog.start();
        QVERIFY(dialog.findChild<QProgressBar*>(QStringLiteral("NormalizeProgress"))->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 10000);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Normalize Audio Levels"));
        auto volume = [&](quint64 id) {
            return ops::clipById(f.view(), id)->audio.values.value(keys::VolumeDb).base;
        };
        QVERIFY(std::abs(volume(loud) - (-9.0 - db(0.5))) < 0.1);
        QVERIFY(std::abs(volume(quiet) - (-9.0 - db(0.125))) < 0.1);
        f.project.undo();
        QCOMPARE(volume(loud), 0.0);
    }

    void dialogRelativeAndCancel()
    {
        TimelineFixture f;
        const quint64 loud = addAudioClip(f, QStringLiteral("loud.wav"), 0);
        const quint64 quiet = addAudioClip(f, QStringLiteral("quiet.wav"), 100);
        {
            NormalizeAudioDialog dialog(&f.project, {loud, quiet});
            dialog.setMode(audionorm::Mode::Loudness);
            QCOMPARE(dialog.target(), -23.0);
            dialog.setScope(audionorm::Scope::Relative);
            dialog.show();
            dialog.start();
            QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 10000);
            const double a = ops::clipById(f.view(), loud)->audio.values.value(keys::VolumeDb).base;
            const double b
                = ops::clipById(f.view(), quiet)->audio.values.value(keys::VolumeDb).base;
            QVERIFY(std::abs(a - b) < 1e-9); // one common gain
            QVERIFY(a < -10.0);
        }
        // Cancel while measuring: nothing changes, no undo step.
        const int before = f.project.undoStack()->count();
        NormalizeAudioDialog dialog(&f.project, {loud, quiet});
        dialog.show();
        dialog.start();
        dialog.reject();
        QVERIFY(!dialog.isRunning());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(f.project.undoStack()->count(), before);
    }

private:
    quint64 addAudioClip(TimelineFixture& f, const QString& file, int64_t start)
    {
        MediaItem item;
        item.path = m_dir.filePath(file);
        item.name = file;
        item.info.valid = item.info.hasAudio = true;
        item.info.durationUs = 4'000'000;
        const QString id = f.project.mediaPool()->addItem(item);
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = id;
        c.name = file;
        c.start = start;
        c.duration = 100; // 4 s at 25 fps
        c.sourceDurationUs = 4'000'000;
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 clipId = ops::overwriteClip(f.data(), {TrackType::Audio, 0}, c);
        f.project.endEdit();
        return clipId;
    }

    QTemporaryDir m_dir;
};

QTEST_MAIN(TestAudioNormalize)
#include "test_audionormalize.moc"
