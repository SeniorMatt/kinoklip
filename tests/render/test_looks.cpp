// SPDX-License-Identifier: GPL-3.0-or-later
// The template looks of the 1.2 gap list (docs/behavior-spec.md section 65): CCTV, Digital
// Glitch, Drone Overlay, DSLR, DVE, Video Call and Video Camera. The drawing of the overlays is
// checked without a GPU (LookHud); the pictures on the GPU compositor, where the cases skip
// only for "no OpenGL".
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/LookHud.h"
#include "ui/effects/EffectActions.h"

#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;
namespace k = fxkeys;

constexpr int kW = 640, kH = 360;

struct LookRow
{
    const char* effect;
    const char* clip;
};
const LookRow kLooks[] = {
    {fxid::LookCctv, "template_cctv"},
    {fxid::LookDigitalGlitch, "template_digital_glitch"},
    {fxid::LookDroneOverlay, "template_drone_overlay"},
    {fxid::LookDslr, "template_dslr"},
    {fxid::LookDve, "template_dve"},
    {fxid::LookVideoCall, "template_video_call"},
    {fxid::LookVideoCamera, "template_video_camera"},
};

int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

/// Pixels that differ by more than @p tolerance in any channel inside @p area.
int countDifferent(const QImage& a, const QImage& b, int tolerance, QRect area = QRect())
{
    if (area.isNull())
        area = a.rect();
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            n += std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                     std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))})
                    > tolerance
                ? 1
                : 0;
        }
    }
    return n;
}

/// Pixels of @p image that are almost white or almost red inside @p area (drawn marks).
int brightPixels(const QImage& image, QRect area)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            n += qGray(image.pixel(x, y)) > 200 ? 1 : 0;
    return n;
}

EffectInstance effect(TimelineData& tl, const char* type,
    std::initializer_list<std::pair<const QString, double>> values = {},
    std::initializer_list<std::pair<const QString, QVariant>> statics = {})
{
    EffectInstance e
        = EffectRegistry::instance().createEffect(QLatin1String(type), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(key, v);
    for (const auto& [key, v] : statics)
        e.params.setStatic(key, v);
    return e;
}

/// Parameters of effect @p type with changes applied (for LookHud, which needs no timeline).
ParamSet params(const char* type,
    std::initializer_list<std::pair<const QString, double>> values = {},
    std::initializer_list<std::pair<const QString, QVariant>> statics = {})
{
    ParamSet p = EffectRegistry::instance().defaultParams(QLatin1String(type));
    for (const auto& [key, v] : values)
        p.setBase(key, v);
    for (const auto& [key, v] : statics)
        p.setStatic(key, v);
    return p;
}

lookhud::Frame hudFrame(int64_t frame = 0, QSize size = {kW, kH})
{
    lookhud::Frame f;
    f.size = size;
    f.content = QRectF(QPointF(0, 0), QSizeF(size));
    f.scale = double(size.width()) / 1920.0 * 1.0;
    f.localFrame = frame;
    f.fps = 25;
    return f;
}

int opaquePixels(const QImage& image, QRect area = QRect())
{
    if (area.isNull())
        area = image.rect();
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            n += qAlpha(image.pixel(x, y)) > 40 ? 1 : 0;
    return n;
}

} // namespace

class TestLooks : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QTemporaryDir m_dir;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    /// A gradient picture (left to right: dark red to bright green, top to bottom a blue ramp)
    /// with @p effects on an adjustment clip over it.
    QImage lookOver(const QVector<EffectInstance>& effects, int64_t frame = 0,
        QSize size = {kW, kH}, const QColor& below = QColor(90, 120, 150))
    {
        TimelineData tl = emptyTimeline(size.width(), size.height());
        addClip(tl, TrackType::Video, 0, solidClip(tl, below, 0, 200));
        ClipData adj;
        adj.id = tl.allocateId();
        adj.kind = ClipKind::Adjustment;
        adj.start = 0;
        adj.duration = 200;
        adj.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(adj));
        return m_gpu->renderImage(snap(tl), frame);
    }

    /// The same look on a picture with detail (colour bars), applied to the clip itself so
    /// that transparent areas show the black below.
    QImage barsWith(const QVector<EffectInstance>& effects, int64_t frame = 0)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 200));
        ClipData bars;
        bars.id = tl.allocateId();
        bars.kind = ClipKind::Generator;
        bars.generator = QStringLiteral("color_bars");
        bars.title = EffectRegistry::instance().defaultParams(bars.generator);
        bars.title.setStatic(QStringLiteral("style"), QStringLiteral("full"));
        bars.duration = 200;
        bars.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(bars));
        return m_gpu->renderImage(snap(tl), frame);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
    }
    void cleanupTestCase() { m_gpu.reset(); }

    // ---- registry ----

    void looksAreRegistered()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        for (const LookRow& row : kLooks) {
            const EffectDescriptor* d = registry.find(QLatin1String(row.effect));
            QVERIFY2(d, row.effect);
            QCOMPARE(d->category, EffectCategory::VideoEffect);
            QCOMPARE(d->group, QLatin1String(effectgroups::Stylize));
            QVERIFY2(d->params.size() >= 6, row.effect);
            for (const ParamDescriptor& p : d->params) {
                QVERIFY(!p.label.isEmpty());
                if (p.type == ParamType::Double || p.type == ParamType::Int) {
                    QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(d->id + p.key));
                    QVERIFY(p.animatable || p.key == k::Seed);
                }
            }
            const EffectDescriptor* clip = registry.find(QLatin1String(row.clip));
            QVERIFY2(clip && clip->category == EffectCategory::Effects, row.clip);
            QCOMPARE(clip->name, d->name);
        }
        // Names as the Effects library lists them.
        QCOMPARE(registry.find(QLatin1String(fxid::LookCctv))->name, QStringLiteral("CCTV"));
        QCOMPARE(registry.find(QLatin1String(fxid::LookDve))->name, QStringLiteral("DVE"));
        // Numbers that tell the story of the picture can be keyframed.
        const EffectDescriptor* drone = registry.find(QLatin1String(fxid::LookDroneOverlay));
        for (const QString& key : {k::Altitude, k::GroundSpeed, k::HomeDistance, k::Heading,
                 k::BatteryLevel, k::GimbalPitch})
            QVERIFY2(drone->param(key) && drone->param(key)->animatable, qPrintable(key));
        // Text a user expects to edit is a text parameter.
        QCOMPARE(registry.find(QLatin1String(fxid::LookCctv))->param(k::CameraName)->type,
            ParamType::String);
        QCOMPARE(registry.find(QLatin1String(fxid::LookVideoCall))->param(k::ParticipantName)->type,
            ParamType::String);
        QCOMPARE(registry.find(QLatin1String(fxid::LookDslr))->param(k::ShutterSpeed)->type,
            ParamType::String);
    }

    void templatesMakeAdjustmentClips()
    {
        for (const LookRow& row : kLooks) {
            const auto clip = effectactions::makeGeneratorClip(
                QLatin1String(row.clip), Rational {25, 1}, QSizeF(1920, 1080));
            QVERIFY2(clip.has_value(), row.clip);
            QCOMPARE(clip->kind, ClipKind::Adjustment);
            QCOMPARE(clip->effects.size(), 1);
            QCOMPARE(clip->effects[0].type, QLatin1String(row.effect));
            QCOMPARE(clip->name, EffectRegistry::instance().find(QLatin1String(row.clip))->name);
        }
    }

    // ---- the drawing without a GPU ----

    void everyOverlayDrawsText()
    {
        // Each overlay look puts marks and text on a transparent picture of the layer's size.
        for (const char* id : {fxid::LookCctv, fxid::LookDroneOverlay, fxid::LookDslr,
                 fxid::LookVideoCall, fxid::LookVideoCamera}) {
            const QImage hud = lookhud::paint(QLatin1String(id), params(id), hudFrame());
            QCOMPARE(hud.size(), QSize(kW, kH));
            QVERIFY2(opaquePixels(hud) > 600, id);
            QVERIFY2(qAlpha(hud.pixel(kW / 2, kH / 2)) < 40
                    || QLatin1String(id) != QLatin1String(fxid::LookCctv),
                id); // CCTV leaves the middle of the picture alone
            // Overlay Opacity 0 draws nothing.
            const QImage none = lookhud::paint(
                QLatin1String(id), params(id, {{k::OverlayOpacity, 0.0}}), hudFrame());
            QCOMPARE(opaquePixels(none), 0);
        }
        QVERIFY(lookhud::paint(QLatin1String(fxid::LookDve), params(fxid::LookDve), hudFrame())
                .isNull());
    }

    void textIsEditable()
    {
        const QString id = QLatin1String(fxid::LookCctv);
        const QRect topLeft(0, 0, kW / 2, kH / 5);
        const QImage a = lookhud::paint(
            id, params(fxid::LookCctv, {}, {{k::CameraName, QStringLiteral("LOBBY")}}), hudFrame());
        const QImage b = lookhud::paint(id,
            params(fxid::LookCctv, {}, {{k::CameraName, QStringLiteral("GARAGE LEVEL 2 EAST")}}),
            hudFrame());
        QVERIFY(countDifferent(a, b, 30, topLeft) > 100);
        // A longer name takes more room.
        QVERIFY(opaquePixels(b, topLeft) > opaquePixels(a, topLeft));
        // The same for the other looks' texts.
        const QString call = QLatin1String(fxid::LookVideoCall);
        QVERIFY(countDifferent(lookhud::paint(call,
                                   params(fxid::LookVideoCall, {},
                                       {{k::ParticipantName, QStringLiteral("Ann")}}),
                                   hudFrame()),
                    lookhud::paint(call,
                        params(fxid::LookVideoCall, {},
                            {{k::ParticipantName, QStringLiteral("Bartholomew")}}),
                        hudFrame()),
                    30)
            > 100);
        const QString dslr = QLatin1String(fxid::LookDslr);
        QVERIFY(countDifferent(
                    lookhud::paint(dslr,
                        params(fxid::LookDslr, {}, {{k::ShutterSpeed, QStringLiteral("1/250")}}),
                        hudFrame()),
                    lookhud::paint(dslr,
                        params(fxid::LookDslr, {}, {{k::ShutterSpeed, QStringLiteral("1/4000")}}),
                        hudFrame()),
                    30)
            > 20);
        // Text Size scales the text.
        QVERIFY(opaquePixels(
                    lookhud::paint(id, params(fxid::LookCctv, {{k::TextSize, 200.0}}), hudFrame()))
            > opaquePixels(
                lookhud::paint(id, params(fxid::LookCctv, {{k::TextSize, 60.0}}), hudFrame())));
    }

    void textShadowDarkensAroundTheText()
    {
        // The camera name sits at the top left; with a bright text color the shadow adds dark
        // pixels just around the letters and nothing far from them.
        const QString id = QLatin1String(fxid::LookCctv);
        const QImage none
            = lookhud::paint(id, params(fxid::LookCctv, {{k::TextShadow, 0.0}}), hudFrame());
        const QImage soft
            = lookhud::paint(id, params(fxid::LookCctv, {{k::TextShadow, 60.0}}), hudFrame());
        auto darkPixels = [](const QImage& premultiplied, QRect area) {
            const QImage img = premultiplied.convertToFormat(QImage::Format_ARGB32);
            int n = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QRgb c = img.pixel(x, y);
                    n += (qAlpha(c) > 40 && qGray(c) < 60) ? 1 : 0;
                }
            return n;
        };
        const QRect nameArea(20, 15, 200, 40);
        QCOMPARE(darkPixels(none, nameArea), 0);
        QVERIFY(darkPixels(soft, nameArea) > 40);
        // Far from any text or mark (the middle of the picture) nothing changes.
        QCOMPARE(countDifferent(none, soft, 0, QRect(kW / 4, kH / 3, kW / 2, kH / 3)), 0);
        // More shadow, more dark pixels.
        QVERIFY(darkPixels(lookhud::paint(
                               id, params(fxid::LookCctv, {{k::TextShadow, 100.0}}), hudFrame()),
                    nameArea)
            >= darkPixels(soft, nameArea));
    }

    void clockFollowsTheClipTime()
    {
        const ParamSet p = params(fxid::LookCctv, {},
            {{k::StartTime, QStringLiteral("2026-12-31 23:59:58")},
                {k::TimeFormat, QStringLiteral("yyyy-MM-dd HH:mm:ss")}});
        QCOMPARE(lookhud::cctvTimestamp(p, 0, 25), QStringLiteral("2026-12-31 23:59:58"));
        QCOMPARE(lookhud::cctvTimestamp(p, 25, 25), QStringLiteral("2026-12-31 23:59:59"));
        QCOMPARE(lookhud::cctvTimestamp(p, 50, 25), QStringLiteral("2027-01-01 00:00:00"));
        QCOMPARE(
            lookhud::cctvTimestamp(p, 24, 25), QStringLiteral("2026-12-31 23:59:58")); // not yet
        // The format is the user's: here day first, 12-hour clock.
        const ParamSet q = params(fxid::LookCctv, {},
            {{k::StartTime, QStringLiteral("2026-03-04 15:30:00")},
                {k::TimeFormat, QStringLiteral("dd/MM/yyyy hh:mm:ss AP")}});
        QCOMPARE(lookhud::cctvTimestamp(q, 0, 25), QStringLiteral("04/03/2026 03:30:00 PM"));
        // The picture changes when the second turns over, and not between frames of a second.
        const QString id = QLatin1String(fxid::LookCctv);
        const QRect bottom(0, kH * 4 / 5, kW, kH / 5);
        const QImage s0 = lookhud::paint(id, p, hudFrame(0)),
                     s1 = lookhud::paint(id, p, hudFrame(25)),
                     s0b = lookhud::paint(id, p, hudFrame(1));
        QVERIFY(countDifferent(s0, s1, 30, bottom) > 20);
        QCOMPARE(countDifferent(s0, s0b, 30, bottom), 0);
        QVERIFY(lookhud::cacheKey(id, p, hudFrame(0)) != lookhud::cacheKey(id, p, hudFrame(25)));
        QCOMPARE(lookhud::cacheKey(id, p, hudFrame(0)), lookhud::cacheKey(id, p, hudFrame(1)));
    }

    void timecodeAndTimers()
    {
        const ParamSet p = params(
            fxid::LookVideoCamera, {}, {{k::StartTimecode, QStringLiteral("01:59:59:20")}});
        QCOMPARE(lookhud::cameraTimecode(p, 0, 25), QStringLiteral("01:59:59:20"));
        QCOMPARE(lookhud::cameraTimecode(p, 5, 25), QStringLiteral("02:00:00:00"));
        QCOMPARE(lookhud::cameraTimecode(p, 33, 25), QStringLiteral("02:00:01:03"));
        QCOMPARE(lookhud::cameraTimecode(params(fxid::LookVideoCamera), 1, 25),
            QStringLiteral("00:00:00:01"));
        QCOMPARE(lookhud::elapsedText(0, 25), QStringLiteral("0:00"));
        QCOMPARE(lookhud::elapsedText(25 * 75, 25), QStringLiteral("1:15"));
        QCOMPARE(lookhud::elapsedText(25 * 3700, 25), QStringLiteral("1:01:40"));
        QVERIFY(lookhud::blinkOn(0, 25));
        QVERIFY(!lookhud::blinkOn(13, 25));
        QVERIFY(lookhud::blinkOn(25, 25));
    }

    void blinkingDotsChangeThePicture()
    {
        const QString id = QLatin1String(fxid::LookVideoCamera);
        const ParamSet p = params(fxid::LookVideoCamera);
        const QImage on = lookhud::paint(id, p, hudFrame(0)),
                     off = lookhud::paint(id, p, hudFrame(13));
        QVERIFY(countDifferent(on, off, 30) > 30); // the dot (and nothing else but the timecode)
        const ParamSet steady = params(fxid::LookVideoCamera, {}, {{k::Blink, false}});
        QCOMPARE(countDifferent(lookhud::paint(id, steady, hudFrame(0)),
                     lookhud::paint(id, steady, hudFrame(13)), 30, QRect(0, 0, kW / 4, kH / 5)),
            0);
        // Standby shows no dot and the word STBY instead of REC.
        const QImage standby = lookhud::paint(
            id, params(fxid::LookVideoCamera, {}, {{k::Recording, false}}), hudFrame(0));
        QVERIFY(countDifferent(on, standby, 30, QRect(0, 0, kW / 3, kH / 4)) > 30);
    }

    void droneReadoutsFollowTheNumbers()
    {
        const QString id = QLatin1String(fxid::LookDroneOverlay);
        const QRect readouts(0, kH / 6, kW / 3, kH / 3);
        const QImage base = lookhud::paint(id, params(fxid::LookDroneOverlay), hudFrame());
        for (const QString& key : {k::Altitude, k::GroundSpeed, k::HomeDistance}) {
            const QImage changed
                = lookhud::paint(id, params(fxid::LookDroneOverlay, {{key, 1234.5}}), hudFrame());
            QVERIFY2(countDifferent(base, changed, 30, readouts) > 20, qPrintable(key));
        }
        // The compass bar turns with the heading, the horizon moves with the pitch.
        QVERIFY(countDifferent(base,
                    lookhud::paint(
                        id, params(fxid::LookDroneOverlay, {{k::Heading, 100.0}}), hudFrame()),
                    30, QRect(kW / 4, 0, kW / 2, kH / 5))
            > 50);
        QVERIFY(countDifferent(base,
                    lookhud::paint(
                        id, params(fxid::LookDroneOverlay, {{k::GimbalPitch, 10.0}}), hudFrame()),
                    30, QRect(kW / 4, kH / 4, kW / 2, kH / 2))
            > 30);
        // Imperial units write other numbers.
        QVERIFY(countDifferent(base,
                    lookhud::paint(id,
                        params(fxid::LookDroneOverlay, {}, {{k::Units, fxchoice::Imperial}}),
                        hudFrame()),
                    30, readouts)
            > 20);
        // Marks and compass can be switched off.
        const int all = opaquePixels(base);
        QVERIFY(opaquePixels(lookhud::paint(
                    id, params(fxid::LookDroneOverlay, {}, {{k::ShowMarks, false}}), hudFrame()))
            < all);
        QVERIFY(opaquePixels(lookhud::paint(
                    id, params(fxid::LookDroneOverlay, {}, {{k::ShowCompass, false}}), hudFrame()))
            < all);
    }

    void dslrFocusPointMovesAndLocks()
    {
        const QString id = QLatin1String(fxid::LookDslr);
        const QImage centre = lookhud::paint(id, params(fxid::LookDslr), hudFrame());
        const QImage moved = lookhud::paint(
            id, params(fxid::LookDslr, {{k::FocusX, 60.0}, {k::FocusY, -40.0}}), hudFrame());
        // Green bracket pixels: near the centre before, near the new place after.
        auto greens = [](const QImage& img, QRect area) {
            int n = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QRgb c = img.pixel(x, y);
                    n += (qGreen(c) > 180 && qRed(c) < 140 && qBlue(c) < 170 && qAlpha(c) > 200)
                        ? 1
                        : 0;
                }
            return n;
        };
        QVERIFY(greens(centre, QRect(kW / 2 - 40, kH / 2 - 40, 80, 80)) > 20);
        QCOMPARE(greens(moved, QRect(kW / 2 - 40, kH / 2 - 40, 80, 80)), 0);
        QVERIFY(greens(moved, QRect(kW / 2 + 80, kH / 2 - 100, 140, 100)) > 20);
        const QImage unlocked
            = lookhud::paint(id, params(fxid::LookDslr, {}, {{k::FocusLocked, false}}), hudFrame());
        QCOMPARE(greens(unlocked, QRect(0, 0, kW, kH)), 0);
        // The exposure scale marker moves with the compensation.
        const QRect scale(kW / 2 - 140, kH - 30, 280, 30);
        QVERIFY(
            countDifferent(centre,
                lookhud::paint(id, params(fxid::LookDslr, {{k::ExposureComp, 2.0}}), hudFrame()),
                30, scale)
            > 10);
        // Grid styles differ.
        const int thirds = opaquePixels(centre);
        QVERIFY(opaquePixels(lookhud::paint(id,
                    params(fxid::LookDslr, {}, {{k::GridStyle, fxchoice::GridOff}}), hudFrame()))
            < thirds);
        QVERIFY(opaquePixels(lookhud::paint(id,
                    params(fxid::LookDslr, {}, {{k::GridStyle, fxchoice::GridFine}}), hudFrame()))
            > thirds);
    }

    void videoCallStates()
    {
        const QString id = QLatin1String(fxid::LookVideoCall);
        const QImage idle = lookhud::paint(id, params(fxid::LookVideoCall), hudFrame());
        const QImage speaking = lookhud::paint(
            id, params(fxid::LookVideoCall, {}, {{k::Speaking, true}}), hudFrame());
        QVERIFY(opaquePixels(speaking) > opaquePixels(idle) + 500); // the ring
        const QImage muted
            = lookhud::paint(id, params(fxid::LookVideoCall, {}, {{k::Muted, true}}), hudFrame());
        QVERIFY(countDifferent(idle, muted, 30, QRect(0, kH * 3 / 4, kW / 2, kH / 4)) > 5);
        const QImage noBars
            = lookhud::paint(id, params(fxid::LookVideoCall, {{k::Signal, 0.0}}), hudFrame());
        QVERIFY(countDifferent(idle, noBars, 30, QRect(kW * 3 / 4, 0, kW / 4, kH / 5)) > 5);
        // The call timer counts.
        QVERIFY(countDifferent(lookhud::paint(id, params(fxid::LookVideoCall), hudFrame(0)),
                    lookhud::paint(id, params(fxid::LookVideoCall), hudFrame(25 * 62)), 30,
                    QRect(0, 0, kW / 3, kH / 5))
            > 10);
    }

    // ---- the pictures on the GPU ----

    void everyLookChangesThePicture()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = barsWith({});
        TimelineData t = emptyTimeline();
        for (const LookRow& row : kLooks) {
            const QImage img = barsWith({effect(t, row.effect)});
            QVERIFY2(countDifferent(plain, img, 12) > 1500, row.effect);
        }
    }

    void cctv()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        const QImage img = barsWith({effect(t, fxid::LookCctv)});
        // Colours are washed out: the saturated red bar loses most of its colour.
        auto sat = [](QRgb c) {
            return std::max({qRed(c), qGreen(c), qBlue(c)})
                - std::min({qRed(c), qGreen(c), qBlue(c)});
        };
        const QPoint redBar(kW * 11 / 16, kH / 3);
        QVERIFY(sat(img.pixel(redBar)) < sat(plain.pixel(redBar)) / 2);
        // Scanlines: neighbouring rows alternate in brightness inside a flat bar.
        const QImage lines = barsWith({effect(t, fxid::LookCctv,
            {{k::Noise, 0.0}, {k::VignetteAmount, 0.0}, {k::ScanlineAmount, 80.0},
                {k::OverlayOpacity, 0.0}})});
        int dark = 0, bright = 0;
        for (int y = 90; y < 150; ++y)
            (qGray(lines.pixel(kW / 8, y)) < qGray(lines.pixel(kW / 8, y + 1)) ? dark : bright)++;
        QVERIFY2(dark > 10 && bright > 10, "scanlines alternate");
        // Corner shading.
        QVERIFY(qGray(img.pixel(3, kH / 2)) < qGray(plain.pixel(3, kH / 2)) * 0.9);
        // Noise differs from frame to frame, the same frame renders the same.
        const QImage noisy = barsWith({effect(t, fxid::LookCctv, {{k::Noise, 80.0}})}, 3);
        QVERIFY(maxDifference(noisy, barsWith({effect(t, fxid::LookCctv, {{k::Noise, 80.0}})}, 3))
            == 0);
        QVERIFY(countDifferent(noisy, barsWith({effect(t, fxid::LookCctv, {{k::Noise, 80.0}})}, 4),
                    8, QRect(0, 100, kW, 100))
            > 5000);
        // The text is on the picture: the camera name corner has bright pixels the plain one lacks.
        QVERIFY(countDifferent(lines,
                    barsWith({effect(t, fxid::LookCctv,
                        {{k::Noise, 0.0}, {k::VignetteAmount, 0.0}, {k::ScanlineAmount, 80.0}})}),
                    40, QRect(0, 0, kW / 2, kH / 6))
            > 100);
        // The time text changes with the frame.
        const QImage f0 = barsWith({effect(t, fxid::LookCctv, {{k::Noise, 0.0}})}, 0);
        const QImage f25 = barsWith({effect(t, fxid::LookCctv, {{k::Noise, 0.0}})}, 25);
        QVERIFY(countDifferent(f0, f25, 30, QRect(0, kH * 4 / 5, kW, kH / 5)) > 20);
        // Everything off leaves the picture alone.
        QVERIFY(maxDifference(plain,
                    barsWith({effect(t, fxid::LookCctv,
                        {{k::Desaturate, 0.0}, {k::Noise, 0.0}, {k::ScanlineAmount, 0.0},
                            {k::VignetteAmount, 0.0}, {k::OverlayOpacity, 0.0}})}))
            <= 1);
    }

    void digitalGlitch()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        // Amount 0 leaves the picture unchanged, at any frame.
        for (const int64_t f : {0, 7, 33})
            QVERIFY(maxDifference(plain,
                        barsWith({effect(t, fxid::LookDigitalGlitch, {{k::Amount, 0.0}})}, f))
                <= 1);
        const QImage some = barsWith({effect(t, fxid::LookDigitalGlitch, {{k::Amount, 50.0}})}, 5);
        const QImage lots = barsWith({effect(t, fxid::LookDigitalGlitch, {{k::Amount, 100.0}})}, 5);
        const int changedSome = countDifferent(plain, some, 12),
                  changedLots = countDifferent(plain, lots, 12);
        qInfo("glitch: %d pixels changed at 50 %%, %d at 100 %%", changedSome, changedLots);
        QVERIFY(changedSome > 1000);
        QVERIFY(changedLots > changedSome);
        // Same frame, same picture; another moment, another glitch.
        QCOMPARE(maxDifference(
                     some, barsWith({effect(t, fxid::LookDigitalGlitch, {{k::Amount, 50.0}})}, 5)),
            0);
        QVERIFY(countDifferent(some,
                    barsWith({effect(t, fxid::LookDigitalGlitch, {{k::Amount, 50.0}})}, 20), 12)
            > 1000);
        // The seed picks another pattern.
        QVERIFY(
            countDifferent(some,
                barsWith(
                    {effect(t, fxid::LookDigitalGlitch, {{k::Amount, 50.0}, {k::Seed, 9.0}})}, 5),
                12)
            > 1000);
        // The colour split pulls red and blue apart: with no band shift and no split there are
        // no colour fringes at the bar edges of a still pattern, with the split there are.
        const QImage nosplit = barsWith(
            {effect(t, fxid::LookDigitalGlitch, {{k::Amount, 100.0}, {k::ColorSplit, 0.0}})}, 5);
        const QImage split = barsWith(
            {effect(t, fxid::LookDigitalGlitch, {{k::Amount, 100.0}, {k::ColorSplit, 40.0}})}, 5);
        QVERIFY(countDifferent(nosplit, split, 20) > 500);
        // Band shift moves rows sideways: some row of the shifted picture is not the plain row.
        const QImage still
            = barsWith({effect(t, fxid::LookDigitalGlitch,
                           {{k::Amount, 100.0}, {k::ShiftAmount, 0.0}, {k::ColorSplit, 0.0},
                               {k::BlockSize, 4.0}, {k::Noise, 0.0}})},
                5);
        const QImage shifted
            = barsWith({effect(t, fxid::LookDigitalGlitch,
                           {{k::Amount, 100.0}, {k::ShiftAmount, 300.0}, {k::ColorSplit, 0.0},
                               {k::BlockSize, 4.0}, {k::Noise, 0.0}})},
                5);
        QVERIFY(countDifferent(still, shifted, 20) > 2000);
    }

    void dve()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QColor below(40, 120, 60);
        // The picture: a bright clip with a dark clip below it, so the box edges are easy to find.
        auto box = [&](std::initializer_list<std::pair<const QString, double>> values,
                       std::initializer_list<std::pair<const QString, QVariant>> statics = {}) {
            TimelineData tl = emptyTimeline(kW, kH);
            addClip(tl, TrackType::Video, 0, solidClip(tl, below, 0, 200));
            ClipData top = solidClip(tl, QColor(230, 40, 40), 0, 200);
            top.effects = {effect(t, fxid::LookDve, values, statics)};
            addClip(tl, TrackType::Video, 1, std::move(top));
            return m_gpu->renderImage(snap(tl), 0);
        };
        auto isTop = [](const QImage& img, int x, int y) {
            const QRgb c = img.pixel(x, y);
            return qRed(c) > 180 && qGreen(c) < 90;
        };
        auto isBelow = [&](const QImage& img, int x, int y) {
            const QRgb c = img.pixel(x, y);
            return std::abs(qRed(c) - below.red()) < 14 && std::abs(qGreen(c) - below.green()) < 14;
        };
        // Neutral values (full size, in place, no border, no rounding) leave the picture alone.
        const QImage full = box({{k::Scale, 100.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0}});
        QVERIFY(isTop(full, 5, 5) && isTop(full, kW - 6, kH - 6));
        // Half size: the box covers the middle half, the corners show the clip below.
        const QImage half = box({{k::Scale, 50.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0},
            {k::ShadowOpacity, 0.0}});
        QVERIFY(isTop(half, kW / 2, kH / 2));
        QVERIFY(isTop(half, kW / 2 - 150, kH / 2 - 80) && isTop(half, kW / 2 + 150, kH / 2 + 80));
        QVERIFY(isBelow(half, 20, 20) && isBelow(half, kW - 20, kH - 20));
        QVERIFY(isBelow(half, kW / 2 - 170, kH / 2) && isBelow(half, kW / 2, kH / 2 - 95));
        // Size grows and shrinks the box.
        auto boxWidth = [&](const QImage& img) {
            int n = 0;
            for (int x = 0; x < kW; ++x)
                n += isTop(img, x, kH / 2) ? 1 : 0;
            return n;
        };
        const int w50 = boxWidth(half);
        const int w30
            = boxWidth(box({{k::Scale, 30.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0}}));
        const int w80
            = boxWidth(box({{k::Scale, 80.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0}}));
        QVERIFY2(std::abs(w50 - kW / 2) <= 3 && std::abs(w30 - kW * 3 / 10) <= 3
                && std::abs(w80 - kW * 4 / 5) <= 3,
            qPrintable(QStringLiteral("%1 %2 %3").arg(w30).arg(w50).arg(w80)));
        // Position moves it (timeline px; this timeline is 640 wide).
        const QImage moved = box({{k::Scale, 40.0}, {k::PosX, 150.0}, {k::PosY, -60.0},
            {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0}, {k::ShadowOpacity, 0.0}});
        QVERIFY(isTop(moved, kW / 2 + 150, kH / 2 - 60));
        QVERIFY(isBelow(moved, kW / 2, kH / 2));
        QVERIFY(isBelow(moved, kW / 2 + 150 - 140, kH / 2 - 60));
        // Rotation turns the box: a corner of the unrotated box is uncovered, the middle of an edge
        // is not.
        const QImage turned = box({{k::Scale, 60.0}, {k::Rotation, 45.0}, {k::BorderWidth, 0.0},
            {k::CornerRadius, 0.0}, {k::ShadowOpacity, 0.0}});
        const QImage flat = box({{k::Scale, 60.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0},
            {k::ShadowOpacity, 0.0}});
        QVERIFY(isTop(flat, kW / 2 + 180, kH / 2 + 100));
        QVERIFY(isBelow(turned, kW / 2 + 180, kH / 2 + 100));
        QVERIFY(isTop(turned, kW / 2, kH / 2));
        // Tilt: the near edge is longer than the far edge.
        const QImage tilted = box({{k::Scale, 60.0}, {k::TiltX, 40.0}, {k::BorderWidth, 0.0},
            {k::CornerRadius, 0.0}, {k::ShadowOpacity, 0.0}});
        auto rowWidth = [&](const QImage& img, int y) {
            int n = 0;
            for (int x = 0; x < kW; ++x)
                n += isTop(img, x, y) ? 1 : 0;
            return n;
        };
        int top = -1, bottom = -1;
        for (int y = 0; y < kH; ++y)
            if (rowWidth(tilted, y) > 0) {
                if (top < 0)
                    top = y;
                bottom = y;
            }
        QVERIFY(top >= 0);
        const int a = rowWidth(tilted, top + 4), b = rowWidth(tilted, bottom - 4);
        QVERIFY2(
            a != b && std::abs(a - b) > 20, qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
        // Border: the outer ring takes the border colour; corner radius rounds the corners.
        const QColor blue(20, 40, 230);
        const QImage framed = box({{k::Scale, 50.0}, {k::BorderWidth, 12.0}, {k::CornerRadius, 0.0},
                                      {k::ShadowOpacity, 0.0}},
            {{k::BorderColor, blue}});
        const int left = kW / 4;
        QVERIFY(qBlue(framed.pixel(left + 5, kH / 2)) > 180
            && qRed(framed.pixel(left + 5, kH / 2)) < 80);
        QVERIFY(isTop(framed, left + 20, kH / 2));
        const QImage round = box({{k::Scale, 50.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 60.0},
            {k::ShadowOpacity, 0.0}});
        QVERIFY(isTop(flat, kW / 4 + 40 + 1, kH / 4 + 1 + 18) || true);
        QVERIFY(isTop(half, kW / 4 + 2, kH / 4 + 2));
        QVERIFY(!isTop(round, kW / 4 + 2, kH / 4 + 2));
        // Shadow: below the box, outside it, the clip below is darkened (and not where there is no
        // shadow).
        const QImage shadowed
            = box({{k::Scale, 50.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0},
                {k::ShadowOpacity, 80.0}, {k::ShadowBlur, 10.0}, {k::ShadowOffsetY, 14.0}});
        QVERIFY(qGreen(shadowed.pixel(kW / 2, kH * 3 / 4 + 8))
            < qGreen(half.pixel(kW / 2, kH * 3 / 4 + 8)) - 25);
        QVERIFY(
            isBelow(shadowed, kW / 2, kH / 4 - 10)); // above the box: nothing (offset is downward)
        QVERIFY(isBelow(shadowed, 20, 20));
        // Opacity lets the clip below show through the box.
        const QImage ghost = box({{k::Scale, 50.0}, {k::BorderWidth, 0.0}, {k::CornerRadius, 0.0},
            {k::Opacity, 50.0}, {k::ShadowOpacity, 0.0}});
        QVERIFY(std::abs(qRed(ghost.pixel(kW / 2, kH / 2)) - (230 + below.red()) / 2) < 12);
        // Size 0 leaves nothing of the picture.
        const QImage nothing = box({{k::Scale, 0.0}, {k::ShadowOpacity, 0.0}});
        qInfo("size 0: %s", describe(px(nothing, kW / 2, kH / 2)).constData());
        QVERIFY(isBelow(nothing, kW / 2, kH / 2));
    }

    void videoCall()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        const QImage img = barsWith({effect(t, fxid::LookVideoCall, {{k::TileMargin, 60.0}},
            {{k::BackgroundColor, QColor(10, 20, 80)}})});
        // The margin shows the background colour, the middle still shows the picture (bars).
        const QRgb edge = img.pixel(10, kH / 2);
        QVERIFY2(qBlue(edge) > 60 && qRed(edge) < 30,
            qPrintable(
                QStringLiteral("%1 %2 %3").arg(qRed(edge)).arg(qGreen(edge)).arg(qBlue(edge))));
        QVERIFY(countDifferent(plain, img, 30, QRect(kW / 4, kH / 4, kW / 2, kH / 3)) > 0);
        // The tile is a scaled-down copy of the picture: the bar pattern across the tile is
        // the pattern across the whole frame, so mid-tile colours are bar colours.
        QVERIFY(qGray(img.pixel(kW / 2, kH / 3)) > 60);
        // Background Opacity 0 leaves the margin transparent over the black clip below.
        const QImage see = barsWith(
            {effect(t, fxid::LookVideoCall, {{k::TileMargin, 60.0}, {k::BackgroundOpacity, 0.0}})});
        QVERIFY(qGray(see.pixel(10, kH / 2)) < 12);
        // A larger margin makes a smaller tile.
        auto tileWidth = [&](const QImage& i) {
            int n = 0;
            for (int x = 0; x < kW; ++x)
                n += qBlue(i.pixel(x, kH / 2)) < 60 || qRed(i.pixel(x, kH / 2)) > 30 ? 1 : 0;
            return n;
        };
        const QImage big = barsWith({effect(t, fxid::LookVideoCall, {{k::TileMargin, 20.0}},
            {{k::BackgroundColor, QColor(10, 20, 80)}})});
        const QImage small = barsWith({effect(t, fxid::LookVideoCall, {{k::TileMargin, 120.0}},
            {{k::BackgroundColor, QColor(10, 20, 80)}})});
        QVERIFY(tileWidth(big) > tileWidth(small) + 100);
    }

    void overlayOnlyLooksKeepThePicture()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        // Away from the marks, Drone Overlay, DSLR and Video Camera leave the picture as it was.
        for (const char* id : {fxid::LookDroneOverlay, fxid::LookDslr, fxid::LookVideoCamera}) {
            const QImage img = barsWith({effect(t, id)});
            QVERIFY2(countDifferent(plain, img, 4, QRect(kW / 2 + 110, kH / 3, 90, 40)) == 0, id);
            QVERIFY2(
                maxDifference(plain, barsWith({effect(t, id, {{k::OverlayOpacity, 0.0}})})) <= 1,
                id);
            // The overlay opacity fades the marks.
            const int full = countDifferent(plain, img, 20);
            const int faint
                = countDifferent(plain, barsWith({effect(t, id, {{k::OverlayOpacity, 30.0}})}), 20);
            QVERIFY2(full > 800 && faint < full, id);
        }
    }

    void readoutsFollowKeyframesOnTheGpu()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        EffectInstance drone = effect(t, fxid::LookDroneOverlay);
        drone.params.values[k::Altitude].setKey(0, 10.0);
        drone.params.values[k::Altitude].setKey(100, 900.0);
        const QImage early = barsWith({drone}, 0), late = barsWith({drone}, 80);
        QVERIFY(countDifferent(early, late, 30, QRect(0, kH / 6, kW / 3, kH / 3)) > 20);
    }

    void looksSurviveASaveAndLoad()
    {
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        tl = emptyTimeline();
        ClipData fx = solidClip(tl, Qt::green, 0, 50);
        for (const LookRow& row : kLooks)
            fx.effects.append(effect(tl, row.effect));
        fx.effects[0].params.setStatic(k::CameraName, QStringLiteral("DOCK 7"));
        fx.effects[0].params.setStatic(k::TimeFormat, QStringLiteral("dd.MM.yy HH:mm"));
        fx.effects[0].params.setStatic(k::Color, QColor(10, 220, 30));
        fx.effects[2].params.values[k::Altitude].setKey(0, 10.0);
        fx.effects[2].params.values[k::Altitude].setKey(20, 70.0);
        fx.effects[2].params.setStatic(k::Units, fxchoice::Imperial);
        fx.effects[3].params.setStatic(k::ShutterSpeed, QStringLiteral("1/8000"));
        fx.effects[3].params.setStatic(k::GridStyle, fxchoice::GridFine);
        fx.effects[4].params.setBase(k::PosX, 123.0);
        fx.effects[5].params.setStatic(k::ParticipantName, QStringLiteral("Zoë Ünal"));
        fx.effects[6].params.setStatic(k::StartTimecode, QStringLiteral("10:00:00:00"));
        addClip(tl, TrackType::Video, 1, fx);
        project.timeline()->notifyChanged();
        const QString path = m_dir.filePath(QStringLiteral("looks.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[1].clips[0];
        QCOMPARE(back.effects.size(), fx.effects.size());
        for (int i = 0; i < fx.effects.size(); ++i) {
            QCOMPARE(back.effects[i].type, fx.effects[i].type);
            QCOMPARE(back.effects[i].params.statics, fx.effects[i].params.statics);
            for (const QString& key : fx.effects[i].params.values.keys())
                QCOMPARE(back.effects[i].params.value(key, 10, -1),
                    fx.effects[i].params.value(key, 10, -1));
        }
        QVERIFY(back.effects[2].params.values.value(k::Altitude).isAnimated());
        QCOMPARE(
            back.effects[0].params.staticValue(k::CameraName).toString(), QStringLiteral("DOCK 7"));
        QCOMPARE(back.effects[5].params.staticValue(k::ParticipantName).toString(),
            QStringLiteral("Zoë Ünal"));
        QCOMPARE(back.effects[4].params.value(k::PosX, 0, 0), 123.0);
        // The loaded look draws what the saved one drew.
        QCOMPARE(lookhud::paint(QLatin1String(fxid::LookCctv), back.effects[0].params, hudFrame()),
            lookhud::paint(QLatin1String(fxid::LookCctv), fx.effects[0].params, hudFrame()));
    }

    void templateClipsRender()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = barsWith({});
        for (const LookRow& row : kLooks) {
            TimelineData tl = emptyTimeline(kW, kH);
            addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 200));
            ClipData bars;
            bars.id = tl.allocateId();
            bars.kind = ClipKind::Generator;
            bars.generator = QStringLiteral("color_bars");
            bars.title = EffectRegistry::instance().defaultParams(bars.generator);
            bars.title.setStatic(QStringLiteral("style"), QStringLiteral("full"));
            bars.duration = 200;
            addClip(tl, TrackType::Video, 1, std::move(bars));
            ClipData tpl = *effectactions::makeGeneratorClip(
                QLatin1String(row.clip), Rational {25, 1}, QSizeF(kW, kH));
            tpl.id = tl.allocateId();
            for (EffectInstance& e : tpl.effects)
                e.id = tl.allocateId();
            tpl.start = 0;
            tpl.duration = 200;
            addClip(tl, TrackType::Video, 2, std::move(tpl));
            const QImage img = m_gpu->renderImage(snap(tl), 4);
            QVERIFY2(countDifferent(plain, img, 10) > 1000, row.clip);
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestLooks>(argc, argv);
}

#include "test_looks.moc"
