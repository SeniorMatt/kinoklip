// SPDX-License-Identifier: GPL-3.0-or-later
// test_formats — every import format of plan.md §5.8: probe + decode one frame / audio block of
// a real fixture (scripts/make_test_media.sh, formats/), plus SupportedFormats, ImageLoader
// (orientation, cache) and ImageSequence (detection, gaps, frame mapping).
#include "MediaTestUtil.h"
#include "media/AudioDecoder.h"
#include "media/ImageLoader.h"
#include "media/ImageSequence.h"
#include "media/MediaInfo.h"
#include "media/SupportedFormats.h"
#include "media/VideoDecoder.h"

#include <QFileInfo>
#include <QTest>

#include <cmath>
#include <vector>

using namespace kk;
using namespace kk::media;

namespace {

enum class Kind {
    Video,
    Audio,
    Still,
    Animation,
};

/// Arrangement of the quadrant still (make_test_media.sh 12c) after orientation handling.
enum class Quadrants {
    None, ///< not a quadrant picture
    Upright, ///< TL red, TR lime, BL blue, BR white
    Clockwise90, ///< EXIF orientation 6 / DNG flip: TL blue, TR red, BL white, BR lime
    CounterClockwise90, ///< HEIF irot 1: TL lime, TR white, BL red, BR blue
};

/// Library a format needs; missing → QSKIP.
enum class Needs {
    Nothing,
    Heif,
    Raw,
};

constexpr int64_t kClipUs = 2'000'000; ///< length of every video/audio fixture
/// Containers round durations to their packet/frame grid (FLV/WMV add ~80 ms, DTS frames...).
constexpr int64_t kDurationToleranceUs = 150'000;
/// Lossy codecs shift the pure quadrant colours a little.
constexpr int kColourTolerance = 70;

QString formatFixture(const QString& name)
{
    return test::fixtureDir() + QStringLiteral("/formats/") + name;
}

/// Mean and standard deviation of the luma of `image` (sampled on a grid).
void lumaStats(const QImage& image, double* mean, double* stddev)
{
    double sum = 0;
    double sumSq = 0;
    int n = 0;
    for (int y = 0; y < image.height(); y += 4) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += 4) {
            const double v = qGray(row[x]);
            sum += v;
            sumSq += v * v;
            ++n;
        }
    }
    *mean = n ? sum / n : 0;
    *stddev = n ? std::sqrt(std::max(0.0, sumSq / n - *mean * *mean)) : 0;
}

/// RMS of 1 s of decoded audio starting at 0.5 s.
double audioRms(const QString& path, QString* error)
{
    AudioDecoder decoder;
    if (!decoder.open(path, error)) {
        return -1;
    }
    constexpr int kFrames = 48000;
    std::vector<float> buffer(size_t(kFrames) * 2);
    const int got = decoder.read(24000, buffer.data(), kFrames);
    if (got <= 0) {
        *error = QStringLiteral("read() returned %1").arg(got);
        return -1;
    }
    double sumSq = 0;
    for (float s : buffer) {
        sumSq += double(s) * s;
    }
    return std::sqrt(sumSq / double(buffer.size()));
}

bool near(QRgb actual, QRgb expected)
{
    return std::abs(qRed(actual) - qRed(expected)) <= kColourTolerance
        && std::abs(qGreen(actual) - qGreen(expected)) <= kColourTolerance
        && std::abs(qBlue(actual) - qBlue(expected)) <= kColourTolerance;
}

/// Checks the four quadrant colours; returns an explanation on mismatch.
QString checkQuadrants(const QImage& image, Quadrants layout)
{
    const QRgb red = qRgb(255, 0, 0), lime = qRgb(0, 255, 0), blue = qRgb(0, 0, 255),
               white = qRgb(255, 255, 255);
    QRgb expected[4] = {}; // TL, TR, BL, BR
    switch (layout) {
    case Quadrants::None:
        return {};
    case Quadrants::Upright:
        expected[0] = red, expected[1] = lime, expected[2] = blue, expected[3] = white;
        break;
    case Quadrants::Clockwise90:
        expected[0] = blue, expected[1] = red, expected[2] = white, expected[3] = lime;
        break;
    case Quadrants::CounterClockwise90:
        expected[0] = lime, expected[1] = white, expected[2] = red, expected[3] = blue;
        break;
    }
    const int w = image.width(), h = image.height();
    const QPoint points[4]
        = {{w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4}};
    for (int i = 0; i < 4; ++i) {
        const QRgb actual = image.pixel(points[i]);
        if (!near(actual, expected[i])) {
            return QStringLiteral("quadrant %1 at (%2,%3) is #%4, expected #%5")
                .arg(i)
                .arg(points[i].x())
                .arg(points[i].y())
                .arg(actual & 0xffffff, 6, 16, QLatin1Char('0'))
                .arg(expected[i] & 0xffffff, 6, 16, QLatin1Char('0'));
        }
    }
    return {};
}

} // namespace

Q_DECLARE_METATYPE(Kind)
Q_DECLARE_METATYPE(Quadrants)
Q_DECLARE_METATYPE(Needs)

class TestFormats : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
    }

    // ---- The format matrix --------------------------------------------------------------

    void probeAndDecode_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<Kind>("kind");
        QTest::addColumn<int>("width"); // display size
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("audio"); // video files: also has an audio stream
        QTest::addColumn<Quadrants>("quadrants");
        QTest::addColumn<Needs>("needs");

        const auto video = [](const char* file, int w = 320, int h = 240, bool audio = true) {
            QTest::newRow(file) << QString::fromLatin1(file) << Kind::Video << w << h << audio
                                << Quadrants::None << Needs::Nothing;
        };
        video("mp4_h264.mp4");
        video("mp4_hevc.mp4");
        video("mp4_av1.mp4");
        video("m4v_h264.m4v", 320, 240, false);
        video("mov_prores.mov");
        video("mov_dnxhr.mov");
        video("mov_cineform.mov");
        video("mkv_vp9.mkv");
        video("mkv_ffv1.mkv");
        video("webm_vp9.webm");
        video("webm_vp8.webm");
        video("webm_av1.webm");
        video("avi_mjpeg.avi");
        video("avi_mpeg4.avi");
        video("avi_raw.avi", 320, 240, false);
        video("mts_h264.mts");
        video("mxf_mpeg2.mxf");
        video("mxf_dnxhd.mxf");
        video("flv_h264.flv");
        video("wmv_wmv2.wmv");
        video("3gp_h263.3gp", 352, 288);
        video("ogv_theora.ogv");
        video("mpg_mpeg2.mpg");
        video("vob_mpeg2.vob");
        video("dv_pal.dv", 768, 576); // 720x576 with 16:15 PAL pixels
        video("y4m_raw.y4m", 320, 240, false);
        video("anim.gif", 160, 120, false);

        for (const char* file : {"audio.wav", "audio.aiff", "audio.flac", "audio.mp3", "audio.m4a",
                 "audio_alac.m4a", "audio.aac", "audio.ogg", "audio.opus", "audio.wma", "audio.ac3",
                 "audio.eac3", "audio.dts", "audio.caf", "audio.w64", "audio.mka"}) {
            QTest::newRow(file) << QString::fromLatin1(file) << Kind::Audio << 0 << 0 << true
                                << Quadrants::None << Needs::Nothing;
        }

        const auto still = [](const char* file, Quadrants q = Quadrants::Upright,
                               Needs needs = Needs::Nothing, int w = 256, int h = 192) {
            QTest::newRow(file) << QString::fromLatin1(file) << Kind::Still << w << h << false << q
                                << needs;
        };
        for (const char* file : {"still.png", "still.jpg", "still.webp", "still.bmp", "still.gif",
                 "still.tiff", "still.tga", "still.ico", "still.svg", "still.jxl", "still.exr",
                 "still.dpx", "still.psd", "still.qoi", "still.pcx", "still.sgi", "still.j2k"}) {
            still(file);
        }
        still("still.avif", Quadrants::Upright, Needs::Heif);
        still("still.heic", Quadrants::CounterClockwise90, Needs::Heif, 192, 256);
        still("exif_rot90.jpg", Quadrants::Clockwise90, Needs::Nothing, 192, 256);
        // Half-size RAW decode: 256x192 sensor → 128x96, then the orientation-6 flip.
        still("raw_rggb.dng", Quadrants::Clockwise90, Needs::Raw, 96, 128);

        QTest::newRow("anim.webp") << QStringLiteral("anim.webp") << Kind::Animation << 160 << 120
                                   << false << Quadrants::None << Needs::Nothing;
    }

    void probeAndDecode()
    {
        QFETCH(QString, file);
        QFETCH(Kind, kind);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(bool, audio);
        QFETCH(Quadrants, quadrants);
        QFETCH(Needs, needs);

        const QString path = formatFixture(file);
        if (!QFileInfo::exists(path)) {
            QSKIP(qPrintable(QStringLiteral("fixture %1 not generated (encoder or tool missing, "
                                            "see make_test_media.sh output)")
                                 .arg(file)));
        }
        if (needs == Needs::Heif && !ImageLoader::hasHeifSupport()) {
            QSKIP("built without libheif");
        }
        if (needs == Needs::Raw && !ImageLoader::hasRawSupport()) {
            QSKIP("built without LibRaw");
        }
        QVERIFY2(SupportedFormats::isLikelyMedia(path), "extension missing from SupportedFormats");

        const MediaInfo info = probe(path);
        QVERIFY2(info.valid, qPrintable(info.error));

        if (kind == Kind::Audio) {
            QVERIFY(info.hasAudio);
            QVERIFY(!info.hasVideo);
            QVERIFY2(std::abs(info.durationUs - kClipUs) <= kDurationToleranceUs,
                qPrintable(QString::number(info.durationUs)));
            QCOMPARE(SupportedFormats::categoryFor(path), MediaCategory::Audio);
        } else {
            QVERIFY(info.hasVideo);
            QCOMPARE(info.displayWidth, width);
            QCOMPARE(info.displayHeight, height);
            QCOMPARE(info.isStillImage, kind == Kind::Still);
            if (kind == Kind::Still) {
                QCOMPARE(info.durationUs, 0);
            } else {
                QVERIFY(info.frameRate.isValid());
                QVERIFY2(std::abs(info.durationUs - kClipUs) <= kDurationToleranceUs,
                    qPrintable(QString::number(info.durationUs)));
            }
            QCOMPARE(info.hasAudio, audio);

            VideoDecoder decoder;
            QString error;
            QVERIFY2(decoder.open(path, &error), qPrintable(error));
            QCOMPARE(decoder.info().isStillImage, kind == Kind::Still);
            const QImage image = decoder.frameAt(1'000'000);
            QVERIFY(!image.isNull());
            QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);
            QCOMPARE(image.size(), QSize(width, height));
            double mean = 0, stddev = 0;
            lumaStats(image, &mean, &stddev);
            QVERIFY2(mean > 30 && stddev > 20,
                qPrintable(
                    QStringLiteral("black/flat frame: mean %1 stddev %2").arg(mean).arg(stddev)));
            const QString quadrantError = checkQuadrants(image, quadrants);
            QVERIFY2(quadrantError.isEmpty(), qPrintable(quadrantError));
            // The raw-plane path works for every format too.
            const auto frame = decoder.decodeFrame(1'000'000);
            QVERIFY(frame);
            QVERIFY(frame->width > 0 && frame->height > 0);
            if (kind == Kind::Animation || file.endsWith(QLatin1String(".gif"))) {
                if (kind != Kind::Still) {
                    // The animation must actually move: frame 0 and a frame at 1.5 s differ.
                    const QImage first = decoder.frameAt(0);
                    const QImage later = decoder.frameAt(1'500'000);
                    QVERIFY(first != later);
                }
            }
        }
        if (info.hasAudio) {
            QString error;
            const double rms = audioRms(path, &error);
            QVERIFY2(rms > 0.05,
                qPrintable(QStringLiteral("silent audio (rms %1) %2").arg(rms).arg(error)));
        }
    }

    // ---- SupportedFormats ---------------------------------------------------------------

    void categories()
    {
        QCOMPARE(categoryFor(QStringLiteral("/a/b/Clip.MOV")), MediaCategory::Video);
        QCOMPARE(categoryFor(QStringLiteral("x.mts")), MediaCategory::Video);
        QCOMPARE(categoryFor(QStringLiteral("song.flac")), MediaCategory::Audio);
        QCOMPARE(categoryFor(QStringLiteral("song.opus")), MediaCategory::Audio);
        QCOMPARE(categoryFor(QStringLiteral("photo.HEIC")), MediaCategory::Image);
        QCOMPARE(categoryFor(QStringLiteral("render.exr")), MediaCategory::Image);
        QCOMPARE(categoryFor(QStringLiteral("IMG_0001.CR3")), MediaCategory::RawImage);
        QCOMPARE(categoryFor(QStringLiteral("shot.dng")), MediaCategory::RawImage);
        QCOMPARE(
            categoryFor(QStringLiteral("/r/shot_%04d.exr#fps=24")), MediaCategory::ImageSequence);
        QCOMPARE(categoryFor(QStringLiteral("notes.txt")), MediaCategory::Unknown);
        QCOMPARE(categoryFor(QStringLiteral("noextension")), MediaCategory::Unknown);
        QVERIFY(isLikelyMedia(QStringLiteral("a.mkv")));
        QVERIFY(!isLikelyMedia(QStringLiteral("a.kkproj")));
        // No extension appears in two categories.
        const QStringList& all = SupportedFormats::allExtensions();
        QCOMPARE(QSet<QString>(all.begin(), all.end()).size(), all.size());
        for (const QString& extension : all) {
            QCOMPARE(extension, extension.toLower());
        }
    }

    void dialogFilters()
    {
        const QString filter = SupportedFormats::dialogFilter();
        const QStringList entries = filter.split(QStringLiteral(";;"));
        QCOMPARE(entries.size(), 5);
        QVERIFY(entries[0].startsWith(QLatin1String("All media (*.mp4 ")));
        QVERIFY(entries[1].startsWith(QLatin1String("Video (")));
        QVERIFY(entries[2].startsWith(QLatin1String("Audio (")));
        QVERIFY(entries[3].startsWith(QLatin1String("Images (")));
        QCOMPARE(entries[4], QStringLiteral("All files (*)"));
        QVERIFY(entries[2].contains(QLatin1String("*.wav")));
        QVERIFY(!entries[2].contains(QLatin1String("*.mp4")));
        QVERIFY(entries[3].contains(QLatin1String("*.heic")));
        QVERIFY(entries[3].contains(QLatin1String("*.cr2"))); // Images include camera RAW
        QVERIFY(entries[0].contains(QLatin1String("*.nef")));
        QVERIFY(entries[0].endsWith(QLatin1Char(')')));
    }

    // ---- ImageLoader --------------------------------------------------------------------

    void stillCache()
    {
        const QString path = formatFixture(QStringLiteral("still.png"));
        ImageLoader::clearCache();
        QCOMPARE(ImageLoader::cachedImageCount(), 0);
        const LoadedImage first = ImageLoader::loadCached(path);
        QVERIFY(!first.image.isNull());
        QCOMPARE(first.backend, ImageBackend::Qt);
        QCOMPARE(ImageLoader::cachedImageCount(), 1);
        QCOMPARE(ImageLoader::cachedBytes(), first.image.sizeInBytes());
        const LoadedImage second = ImageLoader::loadCached(path);
        QCOMPARE(second.image.cacheKey(), first.image.cacheKey()); // shared pixels, no decode
        // Two decoders of the same still share the cached pixels.
        VideoDecoder a, b;
        QVERIFY(a.open(path));
        QVERIFY(b.open(path));
        QCOMPARE(a.frameAt(0).cacheKey(), b.frameAt(0).cacheKey());
        QCOMPARE(ImageLoader::cachedImageCount(), 1);
        // Budget: evicts down to the limit; oversized images are not cached at all.
        ImageLoader::setCacheLimitBytes(1);
        QCOMPARE(ImageLoader::cachedImageCount(), 0);
        QVERIFY(!ImageLoader::loadCached(path).image.isNull());
        QCOMPARE(ImageLoader::cachedImageCount(), 0);
        ImageLoader::setCacheLimitBytes(256LL * 1024 * 1024);
    }

    void backendsAndHeaders()
    {
        struct Row
        {
            const char* file;
            ImageBackend backend;
        };
        const Row rows[] = {{"still.png", ImageBackend::Qt}, {"still.svg", ImageBackend::Qt},
            {"still.exr", ImageBackend::FFmpeg}, {"still.psd", ImageBackend::FFmpeg},
            {"still.jxl", ImageBackend::FFmpeg}, {"still.tga", ImageBackend::FFmpeg}};
        for (const Row& row : rows) {
            const QString path = formatFixture(QString::fromLatin1(row.file));
            if (!QFileInfo::exists(path)) {
                continue;
            }
            const ImageHeader header = ImageLoader::readHeader(path);
            QVERIFY2(header.valid, row.file);
            QCOMPARE(header.backend, row.backend);
            QCOMPARE(ImageLoader::load(path).backend, row.backend);
        }
        if (ImageLoader::hasHeifSupport()
            && QFileInfo::exists(formatFixture(QStringLiteral("still.avif")))) {
            const ImageHeader header
                = ImageLoader::readHeader(formatFixture(QStringLiteral("still.avif")));
            QCOMPARE(header.backend, ImageBackend::Heif);
            QCOMPARE(header.format, QStringLiteral("avif"));
        }
        // Single-frame GIF is a still, animated GIF/WebP are not.
        QVERIFY(!ImageLoader::readHeader(formatFixture(QStringLiteral("still.gif"))).animated);
        QVERIFY(ImageLoader::readHeader(formatFixture(QStringLiteral("anim.gif"))).animated);
        QVERIFY(ImageLoader::readHeader(formatFixture(QStringLiteral("anim.webp"))).animated);
        // Garbage with an image extension fails cleanly through every backend.
        const QString garbage = test::fixture("corrupt.mp4");
        QVERIFY(ImageLoader::load(garbage).image.isNull());
        QVERIFY(!ImageLoader::readHeader(QStringLiteral("/nonexistent/x.png")).valid);
    }

    void rawFullSize()
    {
        if (!ImageLoader::hasRawSupport()) {
            QSKIP("built without LibRaw");
        }
        const QString path = formatFixture(QStringLiteral("raw_rggb.dng"));
        if (!QFileInfo::exists(path)) {
            QSKIP("DNG fixture not generated (python3 missing?)");
        }
        ImageLoader::setRawFullSize(true);
        const LoadedImage full = ImageLoader::load(path);
        const ImageHeader header = ImageLoader::readHeader(path);
        ImageLoader::setRawFullSize(false);
        QCOMPARE(full.image.size(), QSize(192, 256));
        QCOMPARE(header.size, QSize(192, 256));
        QCOMPARE(checkQuadrants(full.image, Quadrants::Clockwise90), QString());
    }

    // ---- Image sequences ----------------------------------------------------------------

    void sequenceDetection()
    {
        const QString dir = formatFixture(QStringLiteral("seq_png"));
        const SequenceInfo png = detectSequence(dir + QStringLiteral("/frame_0003.png"));
        QVERIFY(png.found);
        QCOMPARE(png.pattern, dir + QStringLiteral("/frame_%04d.png"));
        QCOMPARE(png.firstNumber, 1);
        QCOMPARE(png.lastNumber, 10);
        QCOMPARE(png.fileCount, 9);
        QCOMPARE(png.missing, QVector<int> {5});
        QCOMPARE(png.frameCount(), 10);

        const QString dotDir = formatFixture(QStringLiteral("seq_dot"));
        const SequenceInfo dot = detectSequence(dotDir + QStringLiteral("/shot.0102.jpg"));
        QVERIFY(dot.found);
        QCOMPARE(dot.pattern, dotDir + QStringLiteral("/shot.%04d.jpg"));
        QCOMPARE(dot.firstNumber, 100);
        QCOMPARE(dot.lastNumber, 104);
        QVERIFY(dot.missing.isEmpty());

        const QString bareDir = formatFixture(QStringLiteral("seq_unpadded"));
        const SequenceInfo bare = detectSequence(bareDir + QStringLiteral("/7.png"));
        QVERIFY(bare.found);
        QCOMPARE(bare.pattern, bareDir + QStringLiteral("/%d.png"));
        QCOMPARE(bare.firstNumber, 1);
        QCOMPARE(bare.lastNumber, 10);
        const SequenceInfo bareFromTen = detectSequence(bareDir + QStringLiteral("/10.png"));
        QCOMPARE(bareFromTen.pattern, bare.pattern); // 2 digits, but siblings are unpadded

        QVERIFY(!detectSequence(formatFixture(QStringLiteral("still.png"))).found); // no number
        QVERIFY(!detectSequence(formatFixture(QStringLiteral("mp4_h264.mp4"))).found); // 1 file
        QCOMPARE(ImageSequence::displayName(png.pattern), QStringLiteral("frame_[0001-0010].png"));
    }

    void sequencePaths()
    {
        QString pattern;
        Rational rate;
        QVERIFY(ImageSequence::parsePath(
            QStringLiteral("/a/b_%04d.exr#fps=24000/1001"), &pattern, &rate));
        QCOMPARE(pattern, QStringLiteral("/a/b_%04d.exr"));
        QCOMPARE(rate, (Rational {24000, 1001}));
        QVERIFY(ImageSequence::parsePath(QStringLiteral("/a/%d.png"), &pattern, &rate));
        QCOMPARE(rate, ImageSequence::kDefaultFrameRate);
        QVERIFY(ImageSequence::isSequencePath(QStringLiteral("/a/100%%_%03d.png")));
        QCOMPARE(ImageSequence::fileForNumber(QStringLiteral("/a/100%%_%03d.png"), 7),
            QStringLiteral("/a/100%_007.png"));
        QVERIFY(!ImageSequence::isSequencePath(QStringLiteral("/a/100%.png")));
        QVERIFY(!ImageSequence::isSequencePath(QStringLiteral("/a/plain.png")));
        QVERIFY(!ImageSequence::isSequencePath(QStringLiteral("/a%04d/plain.png"))); // dir part
        QVERIFY(!ImageSequence::isSequencePath(QStringLiteral("/a/x_%04d_%04d.png")));
        QCOMPARE(ImageSequence::makePath(QStringLiteral("/a/%d.png"), {24, 1}),
            QStringLiteral("/a/%d.png#fps=24"));
        QCOMPARE(ImageSequence::makePath(QStringLiteral("/a/%d.png")), QStringLiteral("/a/%d.png"));
        QCOMPARE(ImageSequence::makePath(QStringLiteral("/a/%d.png"), {30000, 1001}),
            QStringLiteral("/a/%d.png#fps=30000/1001"));
        QCOMPARE(ImageSequence::fileForNumber(QStringLiteral("/a/f%d.png"), 12345),
            QStringLiteral("/a/f12345.png"));
        QCOMPARE(ImageSequence::fileForNumber(QStringLiteral("/a/f%04d.png"), 12345),
            QStringLiteral("/a/f12345.png"));
    }

    void sequenceProbeAndDecode()
    {
        const QString pattern = formatFixture(QStringLiteral("seq_png/frame_%04d.png"));
        const QString path = ImageSequence::makePath(pattern, {10, 1});
        const MediaInfo info = probe(path);
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY(info.hasVideo);
        QVERIFY(info.isImageSequence);
        QVERIFY(!info.isStillImage);
        QCOMPARE(info.frameRate, (Rational {10, 1}));
        QCOMPARE(info.durationUs, 1'000'000); // numbers 1..10 → 10 frames
        QCOMPARE(info.displayWidth, 160);
        QCOMPARE(info.displayHeight, 120);
        QCOMPARE(info.sequenceFirstNumber, 1);
        QCOMPARE(info.sequenceLastNumber, 10);
        QCOMPARE(info.sequenceMissingFrames, 1);
        QCOMPARE(info.videoCodec, QStringLiteral("png"));

        // Default rate: 25 fps.
        QCOMPARE(probe(pattern).frameRate, ImageSequence::kDefaultFrameRate);
        QCOMPARE(probe(pattern).durationUs, 400'000);

        VideoDecoder decoder;
        QString error;
        QVERIFY2(decoder.open(path, &error), qPrintable(error));
        QVERIFY(decoder.info().isImageSequence);
        // File number n carries barcode n - 1; clip frame k shows number k + 1, except the
        // gap (number 5 = clip frame 4), which holds number 4 (barcode 3).
        for (int k = 0; k < 10; ++k) {
            const int64_t t = k * 100'000 + 50'000;
            const QImage image = decoder.frameAt(t);
            QCOMPARE(test::readFrameIndex(image), k == 4 ? 3 : k);
            QCOMPARE(decoder.lastFrameTimeUs(), k * 100'000);
        }
        // Backwards, past the end (clamped to the last frame), and the raw-plane path.
        QCOMPARE(test::readFrameIndex(decoder.frameAt(150'000)), 1);
        QCOMPARE(test::readFrameIndex(decoder.frameAt(5'000'000)), 9);
        const auto frame = decoder.decodeFrame(250'000);
        QVERIFY(frame);
        QCOMPARE(frame->format, PixelFormat::Bgra);
        QCOMPARE(frame->ptsUs, 200'000);
        QCOMPARE(frame->durationUs, 100'000);
        QCOMPARE(decoder.frameAt(250'000, QSize(80, 0)).size(), QSize(80, 60));

        // A sequence whose files are gone fails cleanly.
        QVERIFY(!probe(formatFixture(QStringLiteral("seq_png/none_%04d.png"))).valid);
        QVERIFY(!decoder.open(formatFixture(QStringLiteral("seq_png/none_%04d.png"))));

        // NTSC rate: exact frame boundaries (frame k starts at ceil(k * 1001 / 30000 s)).
        const QString ntsc = ImageSequence::makePath(
            formatFixture(QStringLiteral("seq_dot/shot.%04d.jpg")), {30000, 1001});
        QVERIFY(decoder.open(ntsc));
        QCOMPARE(decoder.info().durationUs, 166'834); // ceil(5 * 1001 / 30000 s)
        QCOMPARE(test::readFrameIndex(decoder.frameAt(33'366)), 0);
        QCOMPARE(test::readFrameIndex(decoder.frameAt(33'367)), 1);
    }

    void animatedWebpTiming()
    {
        const QString path = formatFixture(QStringLiteral("anim.webp"));
        const MediaInfo info = probe(path);
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.frameRate, (Rational {10, 1}));
        QCOMPARE(info.durationUs, 2'000'000);
        VideoDecoder decoder;
        QVERIFY(decoder.open(path));
        // Forward, backward (reader restart) and repeated requests agree.
        const QImage f5 = decoder.frameAt(550'000);
        QCOMPARE(decoder.lastFrameTimeUs(), 500'000);
        const QImage f19 = decoder.frameAt(1'999'999);
        QCOMPARE(decoder.lastFrameTimeUs(), 1'900'000);
        QVERIFY(f5 != f19);
        QCOMPARE(decoder.frameAt(500'000), f5);
    }
};

QTEST_GUILESS_MAIN(TestFormats)
#include "test_formats.moc"
