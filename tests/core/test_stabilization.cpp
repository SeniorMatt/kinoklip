// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Stabilization maths and data (core/Stabilization.h, spec section 58): a known
// shaky camera path goes in, the corrections are checked with numbers. Jitter removal per Mode,
// Camera Lock, Strength, Zoom coverage, the lookup by source frame (trim, speed, reverse), the
// project file and undo / redo.
#include "CoreTestUtils.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Stabilization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
namespace st = kk::stabilize;

namespace {

constexpr double kAspect = 9.0 / 16.0;
const Rational kFps {25, 1};

/// One camera pose: where the first frame's picture is in this frame (normalised coordinates).
struct Pose
{
    double tx = 0;
    double ty = 0;
    double theta = 0;
    double lns = 0;
};

/// Deterministic noise in -1..1.
struct Noise
{
    uint32_t state = 12345;
    double next()
    {
        state = state * 1664525u + 1013904223u;
        return double(state >> 8) / double(1 << 23) - 1.0;
    }
};

/// The camera path as stored data of @p mode (Perspective: the similarity written as a homography).
std::shared_ptr<const st::Data> makeData(
    st::Mode mode, const std::vector<Pose>& poses, int64_t first = 0, Rational fps = kFps)
{
    std::vector<float> path;
    for (const Pose& p : poses) {
        switch (mode) {
        case st::Mode::Translation:
            path.push_back(float(p.tx));
            path.push_back(float(p.ty));
            break;
        case st::Mode::Similarity:
            path.insert(path.end(), {float(p.tx), float(p.ty), float(p.theta), float(p.lns)});
            break;
        case st::Mode::Perspective: {
            const double s = std::exp(p.lns);
            const double c = s * std::cos(p.theta), n = s * std::sin(p.theta);
            path.insert(path.end(),
                {float(c), float(-n), float(p.tx), float(n), float(c), float(p.ty), 0.0f, 0.0f});
            break;
        }
        }
    }
    return std::make_shared<const st::Data>(mode, fps, first, kAspect, std::move(path));
}

/// Where the camera ends up after the correction: M(t) * P(t), as a matrix.
st::Mat3 corrected(const st::Data& d, const st::Settings& s, int64_t frame)
{
    const int pc = st::paramCount(d.mode());
    const float* p = &d.path()[size_t(frame - d.firstFrame()) * size_t(pc)];
    st::Mat3 path;
    switch (d.mode()) {
    case st::Mode::Translation:
        path = st::Mat3::translation(p[0], p[1]);
        break;
    case st::Mode::Similarity: {
        const double sc = std::exp(double(p[3]));
        path.m = {sc * std::cos(p[2]), -sc * std::sin(p[2]), p[0], sc * std::sin(p[2]),
            sc * std::cos(p[2]), p[1], 0, 0, 1};
        break;
    }
    case st::Mode::Perspective:
        for (int i = 0; i < 8; ++i)
            path.m[size_t(i)] = p[i];
        break;
    }
    return d.correction(s, frame) * path;
}

ClipData stabilizedClip(std::shared_ptr<const st::Data> data, int64_t start = 0,
    int64_t duration = 50, int64_t sourceInFrames = 0)
{
    ClipData c = mediaClip(start, duration, sourceInFrames);
    c.stabilization.settings.enabled = true;
    c.stabilization.settings.mode = data->mode();
    c.stabilization.data = std::move(data);
    return c;
}

} // namespace

class TestStabilization : public QObject
{
    Q_OBJECT

private slots:
    void modesAndRanges()
    {
        for (const st::Mode m :
            {st::Mode::Perspective, st::Mode::Similarity, st::Mode::Translation})
            QCOMPARE(st::modeFromId(st::modeId(m)), m);
        QCOMPARE(st::paramCount(st::Mode::Perspective), 8);
        QCOMPARE(st::paramCount(st::Mode::Similarity), 4);
        QCOMPARE(st::paramCount(st::Mode::Translation), 2);
        st::Settings s;
        s.croppingRatio = 7;
        s.smooth = -3;
        s.strength = -9;
        s = st::clamped(s);
        QCOMPARE(s.croppingRatio, 1.0);
        QCOMPARE(s.smooth, 0.0);
        QCOMPARE(s.strength, -1.0);
        s.croppingRatio = 0.0;
        QCOMPARE(st::clamped(s).croppingRatio, 0.25);
        QVERIFY(ClipData {}.stabilization.isDefault());
    }

    // Translation: the jitter goes, the slow pan stays (rms within 0.002 of the width, 1.3 px of
    // 640).
    void translationRemovesJitterButKeepsThePan()
    {
        Noise noise;
        std::vector<Pose> poses;
        const double pan = 0.0015; // per frame: 0.3 of the width over 200 frames
        for (int t = 0; t < 200; ++t)
            poses.push_back({pan * t + 0.012 * noise.next(), 0.012 * noise.next()});
        const auto data = makeData(st::Mode::Translation, poses);
        st::Settings s;
        s.mode = st::Mode::Translation;
        s.smooth = 1.0;
        double before = 0, after = 0, worst = 0;
        for (int t = 20; t < 180; ++t) { // away from the ends, where the filter has both sides
            const st::Mat3 m = corrected(*data, s, t);
            const double ex = m.m[2] - pan * t, ey = m.m[5];
            before += std::pow(poses[size_t(t)].tx - pan * t, 2) + std::pow(poses[size_t(t)].ty, 2);
            after += ex * ex + ey * ey;
            worst = std::max({worst, std::abs(ex), std::abs(ey)});
        }
        QVERIFY2(std::sqrt(before / 160) > 0.005, "the test path is not shaky enough");
        QVERIFY2(
            std::sqrt(after / 160) < 0.002 && std::sqrt(after / 160) < std::sqrt(before / 160) / 3,
            qPrintable(QString::number(std::sqrt(after / 160))));
        QVERIFY2(worst < 0.005, qPrintable(QString::number(worst)));
        // The ends keep the drift too (a locally linear fit, not a plain average, near the ends).
        QVERIFY(std::abs(corrected(*data, s, 0).m[2] - 0.0) < 0.01);
        QVERIFY(std::abs(corrected(*data, s, 199).m[2] - pan * 199) < 0.01);
    }

    // Smooth 0 follows the camera: nothing is corrected.
    void smoothZeroCorrectsNothing()
    {
        Noise noise;
        std::vector<Pose> poses;
        for (int t = 0; t < 60; ++t)
            poses.push_back({0.01 * noise.next(), 0.01 * noise.next()});
        const auto data = makeData(st::Mode::Translation, poses);
        st::Settings s;
        s.mode = st::Mode::Translation;
        s.smooth = 0.0;
        for (int t = 0; t < 60; ++t) {
            const st::Mat3 m = data->correction(s, t);
            QVERIFY(std::abs(m.m[2]) < 1e-9 && std::abs(m.m[5]) < 1e-9);
        }
    }

    // Camera Lock: pan and jitter both go; the corrected camera is the first frame's.
    void cameraLockRemovesEverything()
    {
        Noise noise;
        std::vector<Pose> poses;
        for (int t = 0; t < 120; ++t)
            poses.push_back({0.002 * t + 0.01 * noise.next(), -0.001 * t + 0.01 * noise.next(),
                0.02 * noise.next(), 0.03 * noise.next()});
        poses[0] = Pose {};
        for (const st::Mode mode :
            {st::Mode::Translation, st::Mode::Similarity, st::Mode::Perspective}) {
            const auto data = makeData(mode, poses);
            st::Settings s;
            s.mode = mode;
            s.cameraLock = true;
            for (int t = 0; t < 120; ++t) {
                const st::Mat3 m = corrected(*data, s, t);
                const st::Mat3 identity;
                double worst = 0;
                for (size_t i = 0; i < 9; ++i)
                    worst = std::max(worst, std::abs(m.m[i] - identity.m[i]));
                // Translation only removes the translation of the path.
                if (mode == st::Mode::Translation)
                    worst = std::max(std::abs(m.m[2]), std::abs(m.m[5]));
                QVERIFY2(worst < 1e-5,
                    qPrintable(
                        QStringLiteral("mode %1 frame %2: %3").arg(int(mode)).arg(t).arg(worst)));
            }
        }
    }

    // Similarity: rotation and zoom jitter are smoothed away as well.
    void similarityHandlesRotationAndZoomJitter()
    {
        Noise noise;
        std::vector<Pose> poses;
        for (int t = 0; t < 200; ++t)
            poses.push_back({0.004 * noise.next(), 0.004 * noise.next(), 0.03 * noise.next(),
                0.04 * noise.next()});
        const auto data = makeData(st::Mode::Similarity, poses);
        st::Settings s;
        s.mode = st::Mode::Similarity;
        s.smooth = 1.0;
        double rotBefore = 0, rotAfter = 0, zoomBefore = 0, zoomAfter = 0;
        for (int t = 30; t < 170; ++t) {
            const st::Mat3 m = corrected(*data, s, t);
            rotBefore += std::pow(poses[size_t(t)].theta, 2);
            zoomBefore += std::pow(poses[size_t(t)].lns, 2);
            rotAfter += std::pow(std::atan2(m.m[3], m.m[0]), 2);
            zoomAfter += std::pow(std::log(std::hypot(m.m[0], m.m[3])), 2);
        }
        QVERIFY(std::sqrt(rotBefore / 140) > 0.01);
        QVERIFY(std::sqrt(zoomBefore / 140) > 0.015);
        QVERIFY2(std::sqrt(rotAfter / 140) < 0.004,
            qPrintable(QString::number(std::sqrt(rotAfter / 140))));
        QVERIFY2(std::sqrt(zoomAfter / 140) < 0.005,
            qPrintable(QString::number(std::sqrt(zoomAfter / 140))));
    }

    // Strength: 0 = nothing, 0.5 = half the move, -1 = the opposite move, 1 = full.
    void strengthScalesTheCorrection()
    {
        const auto data = makeData(st::Mode::Translation, {{0, 0}, {0.05, 0.02}, {0, 0}});
        st::Settings s;
        s.mode = st::Mode::Translation;
        s.cameraLock = true;
        s.strength = 1.0;
        const double full = data->correction(s, 1).m[2];
        QCOMPARE(full, -double(0.05f));
        s.strength = 0.5;
        QVERIFY(std::abs(data->correction(s, 1).m[2] - full / 2) < 1e-9);
        s.strength = -1.0;
        QVERIFY(std::abs(data->correction(s, 1).m[2] + full) < 1e-9);
        s.strength = 0.0;
        QCOMPARE(data->correction(s, 1).m[2], 0.0);

        ClipData clip = stabilizedClip(data, 0, 3);
        clip.stabilization.settings.cameraLock = true;
        clip.stabilization.settings.strength = 0.0;
        QVERIFY(!st::isActive(clip.stabilization));
        QVERIFY(st::transform(clip, 1, QSizeF(640, 360), kFps).isIdentity());
        clip.stabilization.settings.strength = 1.0;
        QVERIFY(st::isActive(clip.stabilization));
        QVERIFY(!st::transform(clip, 1, QSizeF(640, 360), kFps).isIdentity());
    }

    // The enable rules: off, no data, a Mode that differs from the data.
    void activeRules()
    {
        const auto data = makeData(st::Mode::Translation, {{0, 0}, {0.01, 0}});
        ClipData clip = stabilizedClip(data, 0, 2);
        QVERIFY(st::isActive(clip.stabilization));
        clip.stabilization.settings.enabled = false;
        QVERIFY(!st::isActive(clip.stabilization));
        clip.stabilization.settings.enabled = true;
        clip.stabilization.settings.mode = st::Mode::Similarity; // changed after the analysis
        QVERIFY(!st::isActive(clip.stabilization));
        QVERIFY(st::needsAnalysis(clip.stabilization));
        QVERIFY(st::transform(clip, 1, QSizeF(640, 360), kFps).isIdentity());
        ClipData bare = mediaClip(0, 2);
        bare.stabilization.settings.enabled = true;
        QVERIFY(!st::isActive(bare.stabilization)); // switched on, never analysed
        QVERIFY(!st::needsAnalysis(bare.stabilization));
    }

    // Zoom: just enough to cover the picture in the worst frame, at most 1 + Cropping Ratio.
    void zoomCoversTheWorstFrame()
    {
        std::vector<Pose> poses;
        for (int t = 0; t < 60; ++t)
            poses.push_back(
                {0.04 * std::sin(t * 0.5), 0.03 * std::cos(t * 0.37), 0.03 * std::sin(t * 0.21)});
        poses[0] = Pose {};
        for (const st::Mode mode :
            {st::Mode::Translation, st::Mode::Similarity, st::Mode::Perspective}) {
            const auto data = makeData(mode, poses);
            st::Settings s;
            s.mode = mode;
            s.cameraLock = true;
            s.zoom = false;
            QCOMPARE(data->zoomFactor(s), 1.0);
            s.zoom = true;
            const double z = data->zoomFactor(s);
            QVERIFY2(z > 1.02 && z < 2.0, qPrintable(QString::number(z)));
            // Every corner of every frame, shrunk by z, lies inside the corrected picture...
            double worstMargin = 1e9;
            for (int t = 0; t < 60; ++t) {
                bool ok = true;
                const st::Mat3 back = data->correction(s, t).inverse(&ok);
                QVERIFY(ok);
                for (const double sx : {-0.5, 0.5}) {
                    for (const double sy : {-0.5 * kAspect, 0.5 * kAspect}) {
                        double x = 0, y = 0;
                        back.apply(sx / z, sy / z, &x, &y);
                        worstMargin = std::min(
                            {worstMargin, 0.5 - std::abs(x), 0.5 * kAspect - std::abs(y)});
                    }
                }
            }
            QVERIFY2(worstMargin > -1e-6, qPrintable(QString::number(worstMargin)));
            // ... and it is just enough: some corner touches the edge.
            QVERIFY2(worstMargin < 1e-3, qPrintable(QString::number(worstMargin)));
            s.croppingRatio = 0.25;
            QVERIFY(data->zoomFactor(s) <= 1.25 + 1e-9);
            s.croppingRatio = 1.0;
            s.strength = 0.0;
            QCOMPARE(data->zoomFactor(s), 1.0);
        }
    }

    // Source frames, not timeline frames, pick the correction: trim, slip, speed and reverse.
    void thePathIsKeyedBySourceFrame()
    {
        std::vector<Pose> poses;
        for (int t = 0; t < 100; ++t)
            poses.push_back({0.001 * t, 0});
        const auto data = makeData(st::Mode::Translation, poses, 0);
        const QSizeF frame(640, 360);
        // Camera lock: the correction of source frame k is -k/1000 of the picture width (px 640).
        auto shift = [&](const ClipData& c, int64_t local) {
            return st::transform(c, local, frame, kFps).map(QPointF(0, 0)).x();
        };
        auto expected = [](int k) { return -0.001 * k * 640.0; };
        ClipData c = stabilizedClip(data, 100, 50, 10); // starts at source frame 10
        c.stabilization.settings.cameraLock = true;
        QVERIFY(std::abs(shift(c, 0) - expected(10)) < 1e-4);
        QVERIFY(std::abs(shift(c, 7) - expected(17)) < 1e-4);
        // Trim the head by 5 frames: the clip starts later in the source, same frames keep theirs.
        ClipData trimmed = c;
        trimmed.start += 5;
        trimmed.duration -= 5;
        trimmed.sourceInUs += 5 * kUsPerFrame25;
        QVERIFY(std::abs(shift(trimmed, 0) - shift(c, 5)) < 1e-9);
        QVERIFY(std::abs(shift(trimmed, 12) - shift(c, 17)) < 1e-9);
        // Double speed: local frame 3 shows source frame 16.
        ClipData fast = c;
        fast.speed = 2.0;
        QVERIFY(std::abs(shift(fast, 3) - expected(16)) < 1e-4);
        // Reverse: starts at its end and runs backwards.
        ClipData back = c;
        back.speed = -1.0;
        back.sourceInUs = 40 * kUsPerFrame25;
        QVERIFY(std::abs(shift(back, 0) - expected(40)) < 1e-4);
        QVERIFY(std::abs(shift(back, 5) - expected(35)) < 1e-4);
        // Outside the analysed frames: nothing is corrected.
        ClipData beyond = stabilizedClip(data, 0, 50, 98);
        beyond.stabilization.settings.cameraLock = true;
        QVERIFY(std::abs(shift(beyond, 0) - expected(98)) < 1e-4);
        QCOMPARE(shift(beyond, 10), 0.0);
        // Fps of a 30 fps source: frame numbers follow the source rate.
        QCOMPARE(st::sourceFrameAt(1'000'000, Rational {30000, 1001}), int64_t(29));
        QCOMPARE(st::sourceFrameAt(framesToUs(7, Rational {30000, 1001}), Rational {30000, 1001}),
            int64_t(7));
    }

    // The correction sits in the picture's own coordinates: a 16:9 picture shifted by a tenth of
    // its width moves by a tenth of the fitted picture's width, in a taller frame too.
    void theCorrectionFollowsThePictureNotTheFrame()
    {
        const auto data = makeData(st::Mode::Translation, {{0, 0}, {0.1, 0.0}});
        ClipData c = stabilizedClip(data, 0, 2);
        c.stabilization.settings.cameraLock = true;
        QCOMPARE(st::transform(c, 1, QSizeF(1920, 1080), kFps).map(QPointF(0, 0)).x(),
            -float(0.1f) * 1920.0);
        // A square frame: the 16:9 picture is 1000 px wide there.
        QVERIFY(
            std::abs(st::transform(c, 1, QSizeF(1000, 1000), kFps).map(QPointF(0, 0)).x() + 100.0)
            < 1e-4);
    }

    void projectFileRoundTripAndOldFiles()
    {
        Noise noise;
        std::vector<Pose> poses;
        for (int t = 0; t < 80; ++t)
            poses.push_back({0.01 * noise.next(), 0.01 * noise.next(), 0.01 * noise.next(),
                0.01 * noise.next()});
        for (const st::Mode mode :
            {st::Mode::Translation, st::Mode::Similarity, st::Mode::Perspective}) {
            ClipData c = stabilizedClip(makeData(mode, poses, 12, Rational {30000, 1001}));
            c.stabilization.settings.cameraLock = true;
            c.stabilization.settings.zoom = true;
            c.stabilization.settings.croppingRatio = 0.6;
            c.stabilization.settings.smooth = 0.75;
            c.stabilization.settings.strength = -0.5;
            const QJsonObject json = serialization::clipToJson(c);
            QVERIFY(json.contains(QStringLiteral("stabilization")));
            const ClipData back = serialization::clipFromJson(
                QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
            QVERIFY(back.stabilization == c.stabilization);
            QVERIFY(back.stabilization.data);
            QCOMPARE(back.stabilization.data->firstFrame(), int64_t(12));
            QCOMPARE(back.stabilization.data->fps(), (Rational {30000, 1001}));
            QCOMPARE(back.stabilization.data->frames(), int64_t(80));
            QVERIFY(back.stabilization.data->path() == c.stabilization.data->path());
            // Compact: well under the raw size of the numbers as text.
            QVERIFY(QJsonDocument(json).toJson(QJsonDocument::Compact).size()
                < int(80 * st::paramCount(mode) * 4 * 1.4) + 600);
        }
        // A clip that never had stabilization writes nothing and reads back as default.
        const QJsonObject plain = serialization::clipToJson(mediaClip(0, 10));
        QVERIFY(!plain.contains(QStringLiteral("stabilization")));
        QVERIFY(serialization::clipFromJson(plain).stabilization.isDefault());
        // Damaged data is dropped, the settings stay.
        QJsonObject o
            = serialization::clipToJson(stabilizedClip(makeData(st::Mode::Translation, poses)));
        QJsonObject stab = o.value(QStringLiteral("stabilization")).toObject();
        QJsonObject data = stab.value(QStringLiteral("data")).toObject();
        data[QStringLiteral("path")] = QStringLiteral("AAAA");
        stab[QStringLiteral("data")] = data;
        o[QStringLiteral("stabilization")] = stab;
        const ClipData damaged = serialization::clipFromJson(o);
        QVERIFY(!damaged.stabilization.data);
        QVERIFY(damaged.stabilization.settings.enabled);
    }

    // In a project: stabilizing is one undo step, undo / redo bring the data back, save / load keep
    // it, and an old project (no key) loads and compares as before.
    void undoRedoSaveLoad()
    {
        QTemporaryDir dir;
        Project p;
        p.beginEdit(QStringLiteral("Add Clip"));
        editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 30));
        p.endEdit();
        const quint64 id = p.timeline()->data().track(kV1)->clips[0].id;
        const TimelineData before = p.timeline()->data();
        const auto data = makeData(st::Mode::Similarity, {{0, 0}, {0.01, 0, 0.01, 0}, {0, 0.01}});

        p.beginEdit(QStringLiteral("Stabilize"));
        ClipData* clip = editops::clipById(p.timeline()->mutableData(), id);
        clip->stabilization.settings.enabled = true;
        clip->stabilization.settings.mode = st::Mode::Similarity;
        clip->stabilization.data = data;
        p.endEdit();
        const int steps = p.undoStack()->count();
        QCOMPARE(steps, 2);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Stabilize"));
        const TimelineData after = p.timeline()->data();

        p.undo();
        QCOMPARE(p.timeline()->data(), before);
        QVERIFY(p.timeline()->data().track(kV1)->clips[0].stabilization.isDefault());
        p.redo();
        QCOMPARE(p.timeline()->data(), after);
        QVERIFY(p.timeline()->data().track(kV1)->clips[0].stabilization.data == data);

        const QString file = dir.filePath(QStringLiteral("s.kkproj"));
        QVERIFY(p.save(file));
        Project q;
        QString error;
        QVERIFY2(q.load(file, &error), qPrintable(error));
        const ClipData& loaded = q.timeline()->data().track(kV1)->clips[0];
        QVERIFY(loaded.stabilization == after.track(kV1)->clips[0].stabilization);
        QVERIFY(loaded.stabilization.data && *loaded.stabilization.data == *data);

        // Switching it off keeps the measurement; the old file form (no key) loads as default.
        const QString oldFile = dir.filePath(QStringLiteral("old.kkproj"));
        Project o;
        o.beginEdit(QStringLiteral("Add Clip"));
        editops::overwriteClip(o.timeline()->mutableData(), kV1, mediaClip(0, 30));
        o.endEdit();
        QVERIFY(o.save(oldFile));
        QFile f(oldFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!f.readAll().contains("stabilization"));
        Project r;
        QVERIFY(r.load(oldFile, &error));
        QVERIFY(r.timeline()->data().track(kV1)->clips[0].stabilization.isDefault());
    }

    // Editing the clip keeps the data (split halves share it, so each keeps the right frames).
    void splitKeepsTheDataOfBothHalves()
    {
        TimelineData tl = timeline();
        const auto data = makeData(st::Mode::Translation, {{0, 0}, {0.01, 0}, {0.02, 0}});
        ClipData c = stabilizedClip(data, 0, 40);
        editops::overwriteClip(tl, kV1, c);
        const quint64 id = tl.track(kV1)->clips[0].id;
        QVERIFY(!editops::splitClips(tl, {id}, 20).isEmpty());
        QCOMPARE(tl.track(kV1)->clips.size(), size_t(2));
        for (const ClipData& half : tl.track(kV1)->clips)
            QVERIFY(half.stabilization.data == data);
        QCOMPARE(tl.track(kV1)->clips[1].sourceInUs, 20 * kUsPerFrame25);
    }
};

QTEST_GUILESS_MAIN(TestStabilization)
#include "test_stabilization.moc"
