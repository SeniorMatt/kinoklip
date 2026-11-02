// SPDX-License-Identifier: GPL-3.0-or-later
// Adjustment clips: an adjustment clip's effects / transform / opacity / masks act on the
// composite of the tracks below it, exactly like the same settings applied to the clips
// themselves (or to a compound clip of them); transitions, nesting and disabling behave.
#include "RenderTestUtil.h"
#include "core/CompoundOps.h"
#include "core/Generators.h"
#include "core/Subtitles.h"
#include "render/Compositor.h"
#include "render/RenderCacheKeys.h"

#include <QElapsedTimer>

using namespace kk;
using namespace kk::test;

namespace {

int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

/// Fraction of pixels whose channels differ by more than @p tolerance.
double differingFraction(const QImage& a, const QImage& b, int tolerance)
{
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            if (std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                    std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))})
                > tolerance)
                ++count;
        }
    }
    return double(count) / std::max(1, a.width() * a.height());
}

EffectInstance effect(TimelineData& tl, const char* type, const char* key, double value)
{
    EffectInstance e;
    e.id = tl.allocateId();
    e.type = QLatin1String(type);
    e.params.setBase(QLatin1String(key), value);
    return e;
}

ClipData adjustmentClip(TimelineData& tl, int64_t start, int64_t duration)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Adjustment;
    c.name = QStringLiteral("Adjustment Clip");
    c.start = start;
    c.duration = duration;
    return c;
}

/// Opaque red background on V1 and a half-transparent blue box on V2, plus a third (empty)
/// video track for the adjustment clip.
TimelineData scene()
{
    TimelineData tl = emptyTimeline(320, 180);
    tl.uid = 1;
    addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 40, 30), 0, 100));
    ClipData box = solidClip(tl, QColor(20, 60, 220), 0, 100);
    box.transform.setBase(keys::ZoomX, 0.4);
    box.transform.setBase(keys::ZoomY, 0.4);
    box.transform.setBase(keys::PosX, -50);
    box.transform.setBase(keys::Opacity, 60.0);
    addClip(tl, TrackType::Video, 1, box);
    TrackData v3;
    v3.id = tl.allocateId();
    tl.videoTracks.push_back(v3);
    return tl;
}

std::shared_ptr<const TimelineData> withNested(
    const TimelineData& parent, const QVector<const TimelineData*>& nested)
{
    auto map = std::make_shared<NestedTimelineMap>();
    for (const TimelineData* n : nested)
        map->insert(n->uid, std::make_shared<const TimelineData>(*n));
    auto copy = std::make_shared<TimelineData>(parent);
    copy->nested.map = std::move(map);
    return copy;
}

} // namespace

class TestAdjustment : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    void backendRows()
    {
        QTest::addColumn<bool>("useGpu");
        QTest::newRow("gpu") << true;
        QTest::newRow("cpu") << false;
    }
    Compositor* backend()
    {
        QFETCH(bool, useGpu);
        if (useGpu && !m_gpu->isGpu())
            return nullptr;
        return useGpu ? m_gpu.get() : m_cpu.get();
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    /// An identity adjustment clip changes nothing (and outside its range nothing either).
    void identityIsInvisible_data() { backendRows(); }
    void identityIsInvisible()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        const TimelineData plain = scene();
        TimelineData adjusted = plain;
        addClip(adjusted, TrackType::Video, 2, adjustmentClip(adjusted, 10, 20));
        for (const int64_t f : {0, 10, 29, 30}) {
            const int d
                = maxDifference(c->renderImage(snap(plain), f), c->renderImage(snap(adjusted), f));
            QVERIFY2(d <= 1, qPrintable(QStringLiteral("frame %1: %2").arg(f).arg(d)));
        }
    }

    /// Saturation is linear on premultiplied colour, so desaturating the composite equals
    /// desaturating every clip below: the adjustment clip must match the per-clip effects.
    void effectMatchesPerClipEffects()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData perClip = scene();
        perClip.videoTracks[0].clips[0].effects.append(
            effect(perClip, "saturation", "saturation", 20));
        perClip.videoTracks[1].clips[0].effects.append(
            effect(perClip, "saturation", "saturation", 20));
        TimelineData adjusted = scene();
        ClipData adj = adjustmentClip(adjusted, 0, 100);
        adj.effects.append(effect(adjusted, "saturation", "saturation", 20));
        addClip(adjusted, TrackType::Video, 2, adj);
        const QImage a = m_gpu->renderImage(snap(perClip), 5);
        const QImage b = m_gpu->renderImage(snap(adjusted), 5);
        QVERIFY2(maxDifference(a, b) <= 2, qPrintable(QString::number(maxDifference(a, b))));
        // ... and it really did something.
        QVERIFY(maxDifference(b, m_gpu->renderImage(snap(scene()), 5)) > 20);
    }

    /// A blur on the adjustment clip blurs the edge between the tracks below (a per-clip
    /// blur could not do that across clips): the composite is processed as a whole.
    void blurActsOnComposite()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = scene();
        const QImage sharp = m_gpu->renderImage(snap(tl), 5);
        ClipData adj = adjustmentClip(tl, 0, 100);
        adj.effects.append(effect(tl, "gaussian_blur", "radius", 12));
        addClip(tl, TrackType::Video, 2, adj);
        const QImage blurred = m_gpu->renderImage(snap(tl), 5);
        QVERIFY(highFrequencyEnergy(blurred) < highFrequencyEnergy(sharp) * 0.5);
    }

    /// Transform and opacity of an adjustment clip equal those of a compound clip made from
    /// the tracks below, when the result covers the frame (zoom in).
    void transformMatchesCompound_data() { backendRows(); }
    void transformMatchesCompound()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData flat = scene();
        flat.videoTracks.pop_back();
        TimelineData compoundParent = flat;
        QVector<quint64> ids;
        for (const TrackData& t : compoundParent.videoTracks)
            for (const ClipData& clip : t.clips)
                ids.append(clip.id);
        const auto r = compound::collapse(compoundParent, ids, 9, QStringLiteral("cc"), nullptr);
        QVERIFY(r);
        for (TrackData& t : compoundParent.videoTracks) {
            for (ClipData& clip : t.clips) {
                clip.transform.setBase(keys::ZoomX, 1.5);
                clip.transform.setBase(keys::ZoomY, 1.5);
                clip.transform.setBase(keys::Rotation, 8);
            }
        }
        TimelineData adjusted = scene();
        ClipData adj = adjustmentClip(adjusted, 0, 100);
        adj.transform.setBase(keys::ZoomX, 1.5);
        adj.transform.setBase(keys::ZoomY, 1.5);
        adj.transform.setBase(keys::Rotation, 8);
        addClip(adjusted, TrackType::Video, 2, adj);
        const QImage a = c->renderImage(withNested(compoundParent, {&r->nested}), 5);
        const QImage b = c->renderImage(snap(adjusted), 5);
        // Compare the centre (the corners show the unrotated picture below for the adjustment
        // clip, transparent for the compound clip). The nested composite is resampled once
        // more than the adjustment source, so antialiased edges may differ a little: allow a
        // few edge pixels, but the pictures must agree everywhere else.
        const QRect centre(60, 40, 200, 100);
        const double off = differingFraction(a.copy(centre), b.copy(centre), 8);
        QVERIFY2(off < 0.02, qPrintable(QString::number(off)));
    }

    /// Opacity mixes processed and unprocessed picture; a mask limits the effect.
    void opacityAndMask()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = scene();
        ClipData adj = adjustmentClip(tl, 0, 100);
        adj.effects.append(effect(tl, "brightness_contrast", "brightness", 40));
        adj.transform.setBase(keys::Opacity, 50);
        addClip(tl, TrackType::Video, 2, adj);
        const Px half = px(m_gpu->renderImage(snap(tl), 5), 300, 170); // red background
        // brightness 40 adds 51 at full opacity (see TestCompositor::colourEffects) → +25 at half.
        QVERIFY2(near(half, 225, 65, 55, 255, 3), describe(half));

        TimelineData masked = scene();
        ClipData madj = adjustmentClip(masked, 0, 100);
        madj.effects.append(effect(masked, "brightness_contrast", "brightness", 40));
        MaskData m;
        m.id = masked.allocateId();
        m.shape = MaskShape::Rectangle;
        m.params.setBase(maskkeys::Width, 100);
        m.params.setBase(maskkeys::Height, 100);
        m.params.setBase(maskkeys::PosX, 100); // right of the frame centre
        madj.masks.append(m);
        addClip(masked, TrackType::Video, 2, madj);
        const QImage img = m_gpu->renderImage(snap(masked), 5);
        QVERIFY2(near(px(img, 260, 90), 251, 91, 81, 255, 3), describe(px(img, 260, 90)));
        QVERIFY2(near(px(img, 300, 170), 200, 40, 30, 255, 2), describe(px(img, 300, 170)));
    }

    /// A fade-in (transition without neighbour) blends the effect in.
    void transitionFadesEffect()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = scene();
        ClipData adj = adjustmentClip(tl, 10, 50);
        adj.effects.append(effect(tl, "brightness_contrast", "brightness", 40));
        adj.transitionIn = TransitionData {};
        adj.transitionIn->duration = 9; // progress (f - 10 + 1) / 10
        addClip(tl, TrackType::Video, 2, adj);
        const Px mid = px(m_gpu->renderImage(snap(tl), 14), 300, 170); // progress 0.5
        QVERIFY2(near(mid, 225, 65, 55, 255, 3), describe(mid));
        const Px full = px(m_gpu->renderImage(snap(tl), 30), 300, 170);
        QVERIFY2(near(full, 251, 91, 81, 255, 3), describe(full));
    }

    /// An adjustment clip inside a nested timeline only processes that timeline's tracks.
    void nestedScope_data() { backendRows(); }
    void nestedScope()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        // Nested: a zoomed-out green solid with an adjustment clip that flips it (CPU and GPU
        // both implement transforms), placed over a red parent background.
        TimelineData nested = emptyTimeline(320, 180);
        nested.uid = 9;
        ClipData green = solidClip(nested, QColor(30, 200, 60), 0, 100);
        green.transform.setBase(keys::ZoomX, 0.5);
        green.transform.setBase(keys::ZoomY, 0.5);
        green.transform.setBase(keys::PosX, -60);
        addClip(nested, TrackType::Video, 0, green);
        ClipData adj = adjustmentClip(nested, 0, 100);
        adj.transform.setStatic(keys::FlipH, true);
        addClip(nested, TrackType::Video, 1, adj);

        TimelineData parent = emptyTimeline(320, 180);
        parent.uid = 1;
        addClip(parent, TrackType::Video, 0, solidClip(parent, QColor(200, 40, 30), 0, 100));
        addClip(parent, TrackType::Video, 1, compound::makeNestedClip(nested, {}));
        ClipData& inst = parent.videoTracks[1].clips[0];
        inst.id = parent.allocateId();
        inst.duration = 100;
        const QImage img = c->renderImage(withNested(parent, {&nested}), 5);
        // Green now sits on the right half (flipped), red shows through on both sides.
        QVERIFY2(near(px(img, 220, 90), 30, 200, 60, 255, 3), describe(px(img, 220, 90)));
        QVERIFY2(near(px(img, 10, 90), 200, 40, 30, 255, 3), describe(px(img, 10, 90)));
        QVERIFY2(near(px(img, 100, 90), 30, 200, 60, 255, 3) == false, describe(px(img, 100, 90)));
    }

    /// Disabled adjustment clips and hidden tracks do nothing.
    void disabled()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = scene();
        ClipData adj = adjustmentClip(tl, 0, 100);
        adj.effects.append(effect(tl, "saturation", "saturation", 0));
        adj.enabled = false;
        addClip(tl, TrackType::Video, 2, adj);
        const QImage plain = m_gpu->renderImage(snap(scene()), 5);
        QVERIFY(maxDifference(plain, m_gpu->renderImage(snap(tl), 5)) <= 1);
        tl.videoTracks[2].clips[0].enabled = true;
        tl.videoTracks[2].enabled = false;
        QVERIFY(maxDifference(plain, m_gpu->renderImage(snap(tl), 5)) <= 1);
    }

    /// Cost of an adjustment clip at 1080p (printed; numbers in src/render/readme.md).
    void cost()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData plain = emptyTimeline(1920, 1080);
        addClip(plain, TrackType::Video, 0, solidClip(plain, QColor(200, 40, 30), 0, 100));
        ClipData box = solidClip(plain, QColor(20, 60, 220), 0, 100);
        box.transform.setBase(keys::ZoomX, 0.5);
        box.transform.setBase(keys::ZoomY, 0.5);
        addClip(plain, TrackType::Video, 1, box);
        TrackData v3;
        v3.id = plain.allocateId();
        plain.videoTracks.push_back(v3);
        TimelineData adjusted = plain;
        ClipData adj = adjustmentClip(adjusted, 0, 100);
        adj.effects.append(effect(adjusted, "saturation", "saturation", 20));
        addClip(adjusted, TrackType::Video, 2, adj);
        TimelineData identity = plain;
        addClip(identity, TrackType::Video, 2, adjustmentClip(identity, 0, 100));
        auto measure = [&](const TimelineData& tl) {
            const auto shot = snap(tl);
            (void)m_gpu->render(shot, 0, {}, Quality::Full, OutputTexture); // warm up
            QElapsedTimer t;
            t.start();
            constexpr int kFrames = 20;
            for (int f = 0; f < kFrames; ++f)
                (void)m_gpu->render(shot, f, {}, Quality::Full, OutputTexture);
            return double(t.nsecsElapsed()) / 1e6 / kFrames;
        };
        qInfo("1080p ms/frame (texture output): plain %.2f, identity adjustment %.2f, "
              "adjustment + saturation %.2f",
            measure(plain), measure(identity), measure(adjusted));
    }

    /// Render cache: adjustment / generator changes change the chunk keys (their full clip
    /// JSON is hashed), captions do not (they are drawn live over cached chunks, which keep
    /// the subtitle tracks).
    void renderCacheKeys()
    {
        TimelineData tl = scene();
        ClipData adj = adjustmentClip(tl, 0, 100);
        adj.effects.append(effect(tl, "saturation", "saturation", 20));
        addClip(tl, TrackType::Video, 2, adj);
        const CacheKeyOptions options;
        const auto base = renderCacheChunks(tl, {}, options);
        QVERIFY(!base.empty());
        QVERIFY(base.front().reasons & CacheEffects);
        TimelineData changed = tl;
        changed.videoTracks[2].clips[0].effects[0].params.setBase(QStringLiteral("saturation"), 50);
        QVERIFY(renderCacheChunks(changed, {}, options).front().key != base.front().key);
        TimelineData captioned = tl;
        TrackData st = subtitles::makeSubtitleTrack();
        st.id = captioned.allocateId();
        ClipData c = subtitles::makeCaption(QStringLiteral("x"), 0, 10);
        c.id = captioned.allocateId();
        st.clips.push_back(c);
        captioned.subtitleTracks.push_back(st);
        const auto withCaption = renderCacheChunks(captioned, {}, options);
        QCOMPARE(withCaption.front().key, base.front().key);
        const auto view = makeRenderCacheView(std::make_shared<const TimelineData>(captioned),
            withCaption, {withCaption.front().key}, QStringLiteral("/nonexistent"));
        QCOMPARE(view->chunks.size(), size_t(1));
        QCOMPARE(view->chunks.front().timeline->subtitleTracks.size(), size_t(1));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestAdjustment>(argc, argv);
}

#include "test_adjustment.moc"
