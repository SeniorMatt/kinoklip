// SPDX-License-Identifier: GPL-3.0-or-later
// Background Removal (spec 78): the effect's parameters and registration, the unavailable path
// (the picture stays as it is), and the GPU pass with a stand-in model that finds the left half of
// the picture: the three outputs, the threshold, and the render that waits for the matte.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/VideoEffectKeys.h"
#include "matting/MatteService.h"
#include "render/Compositor.h"
#include "render/GlEffects.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

using namespace kk;
using namespace kk::test;
using namespace kk::matting;
namespace k = kk::fxkeys;

namespace {

constexpr int kW = 320, kH = 180;

/// A matte that is 255 on the left half of the picture and 0 on the right half.
class HalfBackend final : public MatteBackend
{
public:
    QImage process(const QImage& rgb, MatteQuality, bool) override
    {
        QImage m(rgb.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                m.scanLine(y)[x] = x < m.width() / 2 ? 255 : 0;
        return m;
    }
};

EffectInstance effect(TimelineData& tl, const QString& output, double threshold = 50)
{
    EffectInstance e = EffectRegistry::instance().createEffect(
        QLatin1String(fxid::BackgroundRemoval), tl.allocateId());
    e.params.setStatic(k::BgOutput, output);
    e.params.setBase(k::BgThreshold, threshold);
    return e;
}

} // namespace

class TestBackgroundRemoval : public QObject
{
    Q_OBJECT
    std::unique_ptr<Compositor> m_gpu;
    QTemporaryDir m_dir;
    QString m_fixtureError;
    std::shared_ptr<MediaPathMap> m_map = std::make_shared<MediaPathMap>();

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    QImage render(const QString& output, double threshold = 50, bool wait = true)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("v"), 0, 40);
        clip.effects = {effect(tl, output, threshold)};
        addClip(tl, TrackType::Video, 0, std::move(clip));
        RenderRequest rq {snap(tl), nullptr, 5, {}, Quality::Full, OutputImage};
        rq.waitForAi = wait;
        return m_gpu->renderAsync(std::move(rq)).get()->image;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        setModelsDirectoryForTests(m_dir.path() + QStringLiteral("/models"));
        MatteService::instance().setCacheRoot(m_dir.path() + QStringLiteral("/cache"));
        m_gpu = std::make_unique<Compositor>();
        auto map = m_map;
        QString why;
        if (ensureFixtures(&why))
            map->insert(QStringLiteral("v"), fixture("h264_25fps.mp4"));
        else
            m_fixtureError = why;
        m_gpu->setMediaPaths(map);
    }
    void cleanupTestCase()
    {
        MatteService::instance().setBackendFactory({});
        MatteService::instance().setFrameSource({});
        m_gpu.reset();
        setModelsDirectoryForTests({});
    }

    void effectIsRegisteredWithItsControls()
    {
        const EffectDescriptor* d
            = EffectRegistry::instance().find(QLatin1String(fxid::BackgroundRemoval));
        QVERIFY(d);
        QCOMPARE(d->name, QStringLiteral("Background Removal"));
        QCOMPARE(d->category, EffectCategory::VideoEffect);
        QVERIFY(d->description.startsWith(QStringLiteral("AI")));
        QStringList keys;
        for (const ParamDescriptor& p : d->params)
            keys << p.key;
        QCOMPARE(keys,
            (QStringList {k::BgModel, k::BgOutput, k::BgThreshold, k::BgSoftness, k::BgShrinkGrow, k::BgFeather,
                k::BgQuality}));
        const ParamSet defaults = EffectRegistry::instance().defaultParams(d->id);
        QCOMPARE(defaults.staticValue(k::BgOutput).toString(), fxchoice::BgForeground);
        QCOMPARE(defaults.staticValue(k::BgQuality).toString(), fxchoice::BgPreview);
        QCOMPARE(defaults.staticValue(k::BgModel).toString(), fxchoice::BgModelAuto);
        QCOMPARE(defaults.value(k::BgThreshold, 0, -1), 50.0);
        QCOMPARE(defaults.value(k::BgShrinkGrow, 0, -1), 0.0);
        const ParamDescriptor* out = d->param(k::BgOutput);
        QCOMPARE(out->choices,
            (QStringList {fxchoice::BgForeground, fxchoice::BgMatte, fxchoice::BgBackground}));
        bool gpu = false;
        for (const render::GlEffectFactory& f : render::builtinGlEffects())
            gpu = gpu || QLatin1String(f.id) == QLatin1String(fxid::BackgroundRemoval);
        QVERIFY(gpu);
    }

    void withoutAModelThePictureStaysAsItIs()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        QVERIFY2(m_fixtureError.isEmpty(), qPrintable(m_fixtureError));
        MatteService::instance().setBackendFactory({});
        MatteService::instance().setFrameSource({});
        QVERIFY(!MatteService::instance().status().ready);
        const QImage img = render(fxchoice::BgForeground);
        QVERIFY(!img.isNull());
        QCOMPARE(img.pixelColor(kW * 3 / 4, kH / 2).alpha(), 255);
        QCOMPARE(img.pixelColor(kW / 4, kH / 2).alpha(), 255);
    }

    void foregroundKeepsTheMatteAndDropsTheRest()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        auto& s = MatteService::instance();
        s.setBackendFactory(
            [](const QString&, QString*) { return std::make_unique<HalfBackend>(); });
        s.setFrameSource([](const QString&, int64_t, int) {
            QImage i(160, 90, QImage::Format_RGB32);
            i.fill(Qt::gray);
            return i;
        });
        const QImage fg = render(fxchoice::BgForeground);
        QCOMPARE(fg.pixelColor(kW / 4, kH / 2).alpha(), 255);
        QCOMPARE(fg.pixelColor(kW * 3 / 4, kH / 2).alpha(), 0);

        const QImage bg = render(fxchoice::BgBackground);
        QCOMPARE(bg.pixelColor(kW / 4, kH / 2).alpha(), 0);
        QCOMPARE(bg.pixelColor(kW * 3 / 4, kH / 2).alpha(), 255);

        const QImage matte = render(fxchoice::BgMatte);
        QVERIFY(qGray(matte.pixel(kW / 4, kH / 2)) > 240);
        QVERIFY(qGray(matte.pixel(kW * 3 / 4, kH / 2)) < 15);
    }

    void aHighThresholdStillKeepsASolidMatte()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage fg = render(fxchoice::BgForeground, 70);
        QCOMPARE(fg.pixelColor(kW / 4, kH / 2).alpha(), 255);
        // At 100 % even a solid matte is only half kept (the soft step is centred on the
        // threshold).
        const QImage none = render(fxchoice::BgForeground, 100);
        QVERIFY(none.pixelColor(kW / 4, kH / 2).alpha() < 255);
    }

    void realModelEndToEnd()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QString model = qEnvironmentVariable("KK_RVM_MODEL");
        if (model.isEmpty() || !matting::runtimeCompiledIn())
            QSKIP("no model or runtime: set KK_RVM_MODEL to run the real model");
        // The real model, installed in a private models folder, and a 1080p clip of a head and
        // shoulders on a green wall, decoded by the real decoder.
        QVERIFY(QDir().mkpath(m_dir.path() + QStringLiteral("/real-models")));
        setModelsDirectoryForTests(m_dir.path() + QStringLiteral("/real-models"));
        QVERIFY(QFile::copy(model, modelPath(defaultModel())));
        QVERIFY(isInstalled(defaultModel()));
        const QString clip = m_dir.filePath(QStringLiteral("person.mkv"));
        // KK_BG_PHOTO: a picture of a person (or anything) to look at by eye instead of the boxes.
        const QString photo = qEnvironmentVariable("KK_BG_PHOTO");
        const QStringList source = photo.isEmpty()
            ? QStringList {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=c=0x2a6a3a:s=1920x1080:r=25,"
                                 "drawbox=x=700:y=520:w=520:h=560:color=0x203c9c:t=fill,"
                                 "drawbox=x=840:y=170:w=240:h=330:color=0xd2a082:t=fill")}
            : QStringList {QStringLiteral("-loop"), QStringLiteral("1"), QStringLiteral("-r"),
                  QStringLiteral("25"), QStringLiteral("-i"), photo, QStringLiteral("-vf"),
                  QStringLiteral(
                      "scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080,"
                      "format=yuv420p")};
        QProcess ffmpeg;
        ffmpeg.start(QStringLiteral("ffmpeg"),
            QStringList {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")}
                + source
                + QStringList {QStringLiteral("-frames:v"), QStringLiteral("30"),
                    QStringLiteral("-c:v"), QStringLiteral("ffv1"), clip});
        QVERIFY(ffmpeg.waitForFinished(120000) && ffmpeg.exitCode() == 0);
        m_map->insert(QStringLiteral("p"), clip);
        m_gpu->setMediaPaths(m_map);

        auto& s = MatteService::instance();
        s.setCacheRoot(m_dir.path() + QStringLiteral("/cache-real"));
        s.setBackendFactory({});
        s.setFrameSource({});
        QVERIFY2(s.status().ready, qPrintable(s.status().reason));
        TimelineData tl = emptyTimeline(1920, 1080);
        ClipData c = mediaClip(tl, QStringLiteral("p"), 0, 30);
        c.effects = {effect(tl, fxchoice::BgMatte)};
        addClip(tl, TrackType::Video, 0, std::move(c));
        QElapsedTimer timer;
        timer.start();
        QImage img;
        for (int f = 0; f < 5; ++f) {
            RenderRequest rq {snap(tl), nullptr, f, {}, Quality::Full, OutputImage};
            rq.waitForAi = true;
            img = m_gpu->renderAsync(std::move(rq)).get()->image;
        }
        qInfo("1080p, preview quality, decode + model + draw: %.0f ms per frame",
            double(timer.elapsed()) / 5);
        QVERIFY(!img.isNull());
        // The wall is not the person.
        if (photo.isEmpty())
            QVERIFY(qGray(img.pixel(100, 100)) < 128);
        const QString out = qEnvironmentVariable("KK_BG_OUT");
        if (!out.isEmpty())
            img.save(out + QStringLiteral("/bg-matte.png"));
        setModelsDirectoryForTests({});
    }

    void withoutWaitingAnUnfinishedMatteLeavesThePicture()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        auto& s = MatteService::instance();
        s.setCacheRoot(m_dir.path() + QStringLiteral("/cache2")); // a cold cache
        s.setBackendFactory([](const QString&, QString*) {
            struct Slow final : MatteBackend
            {
                QImage process(const QImage& rgb, MatteQuality, bool) override
                {
                    QThread::msleep(400);
                    QImage m(rgb.size(), QImage::Format_Grayscale8);
                    m.fill(0);
                    return m;
                }
            };
            return std::make_unique<Slow>();
        });
        const QImage first = render(fxchoice::BgForeground, 50, false);
        QCOMPARE(first.pixelColor(kW / 4, kH / 2).alpha(), 255); // unprocessed, no stall
        // Later the matte (all background) exists and the frame is cut out.
        QTRY_VERIFY_WITH_TIMEOUT(
            render(fxchoice::BgForeground, 50, false).pixelColor(kW / 4, kH / 2).alpha() == 0,
            8000);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestBackgroundRemoval>(argc, argv);
}

#include "test_backgroundremoval.moc"
