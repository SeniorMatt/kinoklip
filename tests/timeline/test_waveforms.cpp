// SPDX-License-Identifier: GPL-3.0-or-later
// test_waveforms — audio clip waveforms like Resolve's: linear amplitude relative to full scale
// (never normalised per clip), per-pixel-column peaks / RMS from the multi-resolution summary
// at every zoom level, following the clip volume (constant and keyed); cache format version.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "media/MediaCache.h"
#include "media/WaveformData.h"
#include "ui/timeline/LevelLines.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineWidget.h"

#include <QDataStream>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <random>

using namespace kk;
using namespace kk::test;
using namespace kk::media;

namespace {
constexpr TrackRef kA1 {TrackType::Audio, 0};

bool runFfmpeg(const QStringList& args)
{
    QProcess p;
    p.start(QStringLiteral("ffmpeg"),
        QStringList {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")} + args);
    return p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

/// A mono 440 Hz tone WAV: amplitude @p loud for the first @p split s, then @p quiet.
bool makeTone(const QString& path, double loud, double quiet, double split, double seconds)
{
    const QString expr = QStringLiteral("if(lt(t\\,%1)\\,%2\\,%3)*sin(2*PI*440*t):s=48000:d=%4")
                             .arg(split)
                             .arg(loud)
                             .arg(quiet)
                             .arg(seconds);
    return runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("aevalsrc=") + expr, path});
}

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

/// Brute force peak over blocks [i0, i1) of level 0 (reference for range()).
float bruteMax(const WaveformData& w, int i0, int i1)
{
    float m = 0.0f;
    for (int i = i0; i < i1; ++i)
        m = std::max(m, w.levels[0].peak[size_t(i)]);
    return m;
}
} // namespace

class TestWaveforms : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_halves; ///< 2 s at -3 dBFS, then 2 s at -30 dBFS
    QString m_tone6; ///< 4 s at -6 dBFS
    std::shared_ptr<const WaveformData> m_halvesWave, m_tone6Wave;

    /// A timeline with one audio clip of @p path (whole file, 25 fps) on a tall A1 and a
    /// provider reading @p wave synchronously.
    struct View
    {
        TimelineFixture f;
        TimelineController c {f.ctx};
        TimelineWidget w {f.ctx, &c};
        quint64 clip = 0;
        View(const QString& path, std::shared_ptr<const WaveformData> wave)
        {
            PreparedMedia media = MediaPool::prepareFile(path);
            f.project.beginEdit(QStringLiteral("setup"));
            const QString id = f.project.mediaPool()->addItem(media.item);
            f.data().audioTracks[0].height = timelinemetrics::kMaxTrackHeight;
            ClipData cd;
            cd.kind = ClipKind::Media;
            cd.name = QStringLiteral("Tone");
            cd.mediaId = id;
            cd.start = 0;
            cd.duration = 100; // 4 s
            clip = editops::overwriteClip(f.data(), kA1, cd);
            f.project.endEdit();
            w.setThumbnailProvider([](const QString&, int64_t, int) { return QImage(); });
            w.setWaveformProvider([wave](const QString&, double startUs, double usPerColumn,
                                      int columns, float* peaks, float* rms) {
                for (int c = 0; c < columns; ++c) {
                    const double t0 = startUs + c * usPerColumn;
                    const WaveformColumn v = wave->range(t0, t0 + usPerColumn);
                    peaks[c] = v.peak;
                    rms[c] = v.rms;
                }
                return true;
            });
            w.resize(1400, 700);
            w.show();
            w.setPixelsPerFrame(10.0, timelinemetrics::kHeaderWidth);
            w.scrollToFrame(0);
            QApplication::processEvents();
        }
        QImage grab()
        {
            QApplication::processEvents();
            QImage img(w.viewport()->size(), QImage::Format_RGB32);
            img.fill(Qt::black);
            QPainter p(&img);
            w.viewport()->render(&p);
            return img;
        }
        QRectF clipRect() const
        {
            const ClipData* c = editops::clipById(f.view(), clip);
            return w.layout().clipRect(w.layout().rowOf(kA1), *c);
        }
        /// Drawn waveform half height at timeline frame @p frame as a fraction of the full
        /// half height (the clip body's half height less the 2 px margin), measured below the
        /// centre line (the level line stays above it for volumes over -18 dB).
        double heightAt(const QImage& img, double frame) const
        {
            const QRectF content = levellines::levelArea(clipRect());
            const int x = int(w.layout().frameToX(frame));
            const double mid = content.center().y();
            const double half = content.height() / 2.0 - 2;
            const int below = int(content.bottom()) - 1; // under the waveform's full height
            const QRgb bg = img.pixel(x, int(content.top()) + 1); // above its full height
            int lowest = -1;
            for (int y = int(mid) + 1; y < below; ++y) {
                if (img.pixel(x, y) != bg)
                    lowest = y;
            }
            return lowest < 0 ? 0.0 : (lowest + 1 - mid) / half;
        }
    };

private slots:
    void initTestCase()
    {
        initTimelineTestCase("test_waveforms");
        QVERIFY(m_dir.isValid());
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg CLI not found");
        m_halves = m_dir.filePath(QStringLiteral("halves.wav"));
        m_tone6 = m_dir.filePath(QStringLiteral("tone6.wav"));
        QVERIFY(makeTone(m_halves, dbToGain(-3), dbToGain(-30), 2.0, 4.0));
        QVERIFY(makeTone(m_tone6, dbToGain(-6), dbToGain(-6), 4.0, 4.0));
        m_halvesWave = computeWaveform(m_halves);
        m_tone6Wave = computeWaveform(m_tone6);
        QVERIFY(m_halvesWave && m_tone6Wave);
    }

    /// Peaks and RMS are linear relative to full scale, not normalised per file.
    void peaksAreLinearFullScale()
    {
        const WaveformData& w = *m_halvesWave;
        QCOMPARE(w.peaksPerSecond, kWaveformPeaksPerSecond);
        const WaveformColumn loud = w.range(0.5e6, 1.5e6);
        const WaveformColumn quiet = w.range(2.5e6, 3.5e6);
        QVERIFY2(std::abs(loud.peak - dbToGain(-3)) < 0.01, qPrintable(QString::number(loud.peak)));
        QVERIFY2(std::abs(quiet.peak - dbToGain(-30)) < 0.002,
            qPrintable(QString::number(quiet.peak)));
        QVERIFY(std::abs(loud.rms - dbToGain(-3) / std::sqrt(2.0)) < 0.01);
        QVERIFY(std::abs(quiet.rms - dbToGain(-30) / std::sqrt(2.0)) < 0.002);
        const WaveformColumn six = m_tone6Wave->range(1e6, 2e6);
        QVERIFY(std::abs(six.peak - 0.501) < 0.01);
        // Zoomed in (a range narrower than a block) stays at the same level.
        const WaveformColumn fine = w.range(1.0e6, 1.0e6 + 500);
        QVERIFY(std::abs(fine.peak - loud.peak) < 0.02);
    }

    /// range() over the summary levels equals the brute-force maximum over the blocks, for
    /// short and long (zoomed-out) ranges; quiet stretches stay quiet when zoomed out.
    void rangeMatchesBruteForceAtEveryZoom()
    {
        const WaveformData& w = *m_halvesWave;
        QVERIFY(w.levels.size() >= 3);
        const int n = w.size();
        const double usPerBlock = 1e6 / w.peaksPerSecond;
        std::mt19937 rng(7);
        for (int k = 0; k < 500; ++k) {
            const int i0 = int(rng() % unsigned(n));
            const int len = 1 + int(rng() % unsigned(n - i0));
            const WaveformColumn v = w.range(i0 * usPerBlock, (i0 + len) * usPerBlock);
            QCOMPARE(v.peak, bruteMax(w, i0, i0 + len));
        }
        // The quiet half as one column (400 blocks, a zoomed-out view): still -30 dBFS.
        const WaveformColumn quiet = w.range(2.0e6 + usPerBlock, 4.0e6);
        QVERIFY2(quiet.peak < dbToGain(-29), qPrintable(QString::number(quiet.peak)));
        // The whole file: the loud peak, and an RMS between the two halves'.
        const WaveformColumn all = w.range(0, 4.0e6);
        QVERIFY(std::abs(all.peak - dbToGain(-3)) < 0.01);
        QVERIFY(all.rms < dbToGain(-3) / std::sqrt(2.0) && all.rms > dbToGain(-30));
    }

    /// Spec 31.13: the loud half draws ~22 times higher than the quiet one; -6 dBFS = half.
    void drawnHeightIsLinearAmplitude()
    {
        View v(m_halves, m_halvesWave);
        const QImage img = v.grab();
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty())
            img.save(QDir(dir).filePath(QStringLiteral("waveforms_halves.png")));
        const double loud = v.heightAt(img, 25);
        const double quiet = v.heightAt(img, 75);
        QVERIFY2(std::abs(loud - dbToGain(-3)) < 0.06, qPrintable(QString::number(loud)));
        QVERIFY2(quiet < 0.08, qPrintable(QString::number(quiet)));
        QVERIFY(loud > 8 * quiet);

        View six(m_tone6, m_tone6Wave);
        const double h = six.heightAt(six.grab(), 50);
        QVERIFY2(std::abs(h - 0.5) < 0.06, qPrintable(QString::number(h)));
    }

    /// Spec 31.14: the waveform follows the clip volume: -6 dB halves it, keys change it along
    /// the clip, +dB clips at the top.
    void heightFollowsClipVolume()
    {
        View v(m_tone6, m_tone6Wave);
        const double before = v.heightAt(v.grab(), 50);
        v.f.project.beginEdit(QStringLiteral("volume"));
        ClipData* c = editops::clipById(v.f.data(), v.clip);
        c->audio.animated(keys::VolumeDb, 0.0).base = -6.0;
        v.f.project.endEdit();
        const double down = v.heightAt(v.grab(), 50);
        QVERIFY2(std::abs(down - before * 0.501) < 0.05,
            qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(down)));

        v.f.project.beginEdit(QStringLiteral("loud"));
        c = editops::clipById(v.f.data(), v.clip);
        c->audio.animated(keys::VolumeDb, 0.0).base = 12.0; // 4x: clipped at full height
        v.f.project.endEdit();
        const double up = v.heightAt(v.grab(), 50);
        QVERIFY2(std::abs(up - 1.0) < 0.06, qPrintable(QString::number(up)));

        // Keys: 0 dB at frame 0 → -18 dB at frame 99: the height falls along the clip.
        v.f.project.beginEdit(QStringLiteral("keys"));
        c = editops::clipById(v.f.data(), v.clip);
        AnimatedValue& vol = c->audio.animated(keys::VolumeDb, 0.0);
        vol.base = 0.0;
        vol.setKey(0, 0.0);
        vol.setKey(99, -18.0);
        v.f.project.endEdit();
        const QImage img = v.grab();
        const double h10 = v.heightAt(img, 10);
        const double h50 = v.heightAt(img, 50);
        const double h90 = v.heightAt(img, 90);
        QVERIFY2(h10 > h50 && h50 > h90,
            qPrintable(QStringLiteral("%1 %2 %3").arg(h10).arg(h50).arg(h90)));
        const double expected50 = 0.501 * dbToGain(vol.valueAtTime(50));
        QVERIFY2(std::abs(h50 - expected50) < 0.05,
            qPrintable(QStringLiteral("%1 vs %2").arg(h50).arg(expected50)));
    }

    /// Zoomed out (many blocks per pixel column) the halves keep their heights: no saturation.
    void zoomedOutDoesNotSaturate()
    {
        View v(m_halves, m_halvesWave);
        v.w.setPixelsPerFrame(0.6, timelinemetrics::kHeaderWidth); // ~67 ms per column
        v.w.scrollToFrame(0);
        const QImage img = v.grab();
        const double loud = v.heightAt(img, 25);
        const double quiet = v.heightAt(img, 75);
        QVERIFY2(std::abs(loud - dbToGain(-3)) < 0.06, qPrintable(QString::number(loud)));
        QVERIFY2(quiet < 0.08, qPrintable(QString::number(quiet)));
    }

    /// Spec 31.15: waveform files of the old format (version 1, 8-bit, no RMS) are not used;
    /// MediaCache computes and rewrites them.
    void oldCacheVersionIsRebuilt()
    {
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        MediaCache& cache = MediaCache::instance();
        cache.setCacheDirectory(cacheDir.path());
        QSignalSpy spy(&cache, &MediaCache::waveformReady);
        QVERIFY(cache.waveform(m_tone6) == nullptr);
        QVERIFY(spy.wait(120000));
        QVERIFY(cache.waveform(m_tone6));
        QString file;
        QDirIterator it(cacheDir.path() + QStringLiteral("/waveforms"), {QStringLiteral("*.cwwf")},
            QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        file = it.next();

        // Replace it by a version 1 file (qint8 min / max per 10 ms) of silence.
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QDataStream out(&f);
            out.setVersion(QDataStream::Qt_6_4);
            const QVector<qint8> zeros(400, 0);
            out << quint32(0x43575746) << quint32(1) << qint32(100) << zeros << zeros << zeros
                << zeros;
        }
        QVERIFY(loadWaveform(file) == nullptr);
        cache.clearMemoryCache();
        QVERIFY(cache.waveform(m_tone6) == nullptr);
        QVERIFY(spy.wait(120000));
        const auto rebuilt = cache.waveform(m_tone6);
        QVERIFY(rebuilt);
        QCOMPARE(rebuilt->peaksPerSecond, kWaveformPeaksPerSecond);
        QVERIFY(std::abs(rebuilt->range(1e6, 2e6).peak - 0.501) < 0.01);
        const auto reread = loadWaveform(file); // rewritten in the current format
        QVERIFY(reread);
        QCOMPARE(reread->maxL, rebuilt->maxL);
    }
};

QTEST_MAIN(TestWaveforms)
#include "test_waveforms.moc"
