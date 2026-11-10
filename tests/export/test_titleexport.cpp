// SPDX-License-Identifier: GPL-3.0-or-later
// Titles in exports (specs 68 and 69): an exported frame shows the same layers, emoji and
// captions as the viewer's frame of the compositor. The emoji parts need a colour emoji font
// (the system's, or the file named by KK_EMOJI_FONT).
#include "ExportTestUtil.h"
#include "core/Subtitles.h"
#include "core/TitleLayers.h"
#include "export/ExportJob.h"
#include "export/ExportSettings.h"
#include "render/Compositor.h"
#include "render/EmojiText.h"

using namespace kk;
using namespace kk::test;

namespace {

constexpr int64_t kFrame = 7;

/// Number of hue buckets with at least 40 clearly coloured pixels.
int hues(const QImage& image, const QRect& area)
{
    int buckets[12] = {};
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.hsvSaturationF() < 0.35 || c.valueF() < 0.25)
                continue;
            ++buckets[std::clamp(c.hsvHue() / 30, 0, 11)];
        }
    }
    int n = 0;
    for (int count : buckets)
        n += count >= 40 ? 1 : 0;
    return n;
}

} // namespace

class TestTitleExport : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Compositor> m_compositor;

    /// The frame kFrame of @p tl as the viewer shows it and as an export writes it.
    void viewerAndExport(const TimelineData& tl, QImage* viewer, QImage* exported)
    {
        const auto shot = std::make_shared<const TimelineData>(tl);
        *viewer = m_compositor->renderImage(shot, kFrame);
        QVERIFY(!viewer->isNull());
        ExportSettings s;
        s.container = QStringLiteral("png_seq");
        s.normalize();
        s.audio.enabled = false;
        s.range = RangeMode::Custom;
        s.rangeIn = kFrame;
        s.rangeOut = kFrame + 1;
        s.subtitles.enabled = true;
        s.subtitles.mode = SubtitleExportMode::BurnIn;
        s.outputPath = m_dir.filePath(QStringLiteral("frame_%1.png").arg(m_n++));
        ExportJob job(s, shot, std::make_shared<MediaPathMap>());
        job.setCompositor(m_compositor.get());
        const ExportResult r = job.run();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.files.isEmpty());
        *exported = QImage(r.files.first());
        QVERIFY(!exported->isNull());
    }
    int m_n = 0;

    static double meanDifference(const QImage& a, const QImage& b)
    {
        if (a.size() != b.size())
            return 255;
        double sum = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y);
                const QRgb q = b.pixel(x, y);
                sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                    + std::abs(qBlue(p) - qBlue(q));
            }
        }
        return sum / (3.0 * a.width() * a.height());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        render::emoji::registerBundledFont();
        m_compositor = std::make_unique<Compositor>();
    }
    void cleanupTestCase() { m_compositor.reset(); }

    // Spec 68.17: a title with layers exports as the viewer shows it.
    void spec68_17_layersExportLikeTheViewer()
    {
        TimelineData tl = emptyTimeline(320, 180);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(20, 20, 30), 0, 50));
        ClipData title = titleClip(tl, QStringLiteral("LAYERS"), 0, 50, 48);
        const quint64 box
            = titlelayers::addLayer(title, TitleLayerKind::Shape, QStringLiteral("rounded"));
        titlelayers::params(title, box)->setStatic(keys::Color, QColor(220, 40, 40));
        titlelayers::params(title, box)->setBase(keys::ShapeWidth, 60);
        titlelayers::params(title, box)->setBase(keys::ShapeHeight, 40);
        titlelayers::params(title, box)->animated(keys::TextPosX, 0.5).setKey(0, 0.2);
        titlelayers::params(title, box)->animated(keys::TextPosX, 0.5).setKey(20, 0.8);
        titlelayers::moveLayer(title, box, -1);
        addClip(tl, TrackType::Video, 1, title);
        QImage viewer;
        QImage exported;
        viewerAndExport(tl, &viewer, &exported);
        QVERIFY2(meanDifference(viewer, exported) < 1.0,
            qPrintable(QString::number(meanDifference(viewer, exported))));
        // The keyframed box sits where frame 7 puts it: 0.2 + 0.6 * 7 / 20 = 0.41 of the width.
        int redX = 0;
        int redN = 0;
        for (int y = 0; y < exported.height(); ++y)
            for (int x = 0; x < exported.width(); ++x)
                if (qRed(exported.pixel(x, y)) > 200 && qGreen(exported.pixel(x, y)) < 80) {
                    redX += x;
                    ++redN;
                }
        QVERIFY(redN > 1000); // 60 x 40 less the rounded corners
        QVERIFY2(std::abs(redX / redN - 131) <= 6, qPrintable(QString::number(redX / redN)));
    }

    // Spec 69.6: colour emoji of a title and of a caption are in the exported frame, as in the
    // viewer's.
    void spec69_6_emojiExportLikeTheViewer()
    {
        if (!render::emoji::hasColourFont())
            QSKIP("no colour emoji font (set KK_EMOJI_FONT to a Noto Color Emoji file)");
        TimelineData tl = emptyTimeline(480, 270);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(20, 20, 30), 0, 50));
        ClipData title = titleClip(tl, QStringLiteral("\U0001F34E\U0001F34F\U0001F30A"), 0, 50, 70);
        title.title.setBase(keys::TextPosY, 0.7);
        addClip(tl, TrackType::Video, 1, title);
        TrackData track = subtitles::makeSubtitleTrack();
        track.id = tl.allocateId();
        track.subtitleStyle.setBase(keys::Size, 50);
        ClipData caption
            = subtitles::makeCaption(QStringLiteral("\U0001F525\U0001F340\U0001F4A7"), 0, 50);
        caption.id = tl.allocateId();
        track.clips.push_back(caption);
        tl.subtitleTracks.push_back(track);
        QImage viewer;
        QImage exported;
        viewerAndExport(tl, &viewer, &exported);
        QVERIFY2(meanDifference(viewer, exported) < 1.0,
            qPrintable(QString::number(meanDifference(viewer, exported))));
        // Title in the upper part, caption in the lower third: both in colour.
        QVERIFY2(hues(exported, QRect(0, 0, 480, 150)) > 2,
            qPrintable(QString::number(hues(exported, QRect(0, 0, 480, 150)))));
        QVERIFY2(hues(exported, QRect(0, 180, 480, 90)) > 2,
            qPrintable(QString::number(hues(exported, QRect(0, 180, 480, 90)))));
    }
};

QTEST_MAIN(TestTitleExport)
#include "test_titleexport.moc"
