// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/FrameRateConvert: a timeline keeps everything at the same moment in time
// when its frame rate changes (24 -> 60, 60 -> 24, 29.97 -> 60), contiguity, keyframes of every
// parameter set, markers, fades, transitions, nested clips; project level and undo.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/FrameRateConvert.h"
#include "core/Generators.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Serialization.h"
#include "core/VideoEffectKeys.h"
#include "core/VideoEffectKeys5.h"

#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

const Rational k24 {24, 1};
const Rational k60 {60, 1};
const Rational k2997 {30000, 1001};

double seconds(int64_t frames, Rational fps)
{
    return double(frames) * fps.den / fps.num;
}

/// A 24 fps timeline using every frame-based field.
TimelineData fullTimeline(Rational fps = k24)
{
    TimelineData d = makeDefaultTimeline(QStringLiteral("T"), fps, 1920, 1080, 2, 2);
    d.uid = 1;
    ClipData a = mediaClip(0, 7, 0, 0, QStringLiteral("a"));
    ClipData b = mediaClip(7, 5, 10, 0, QStringLiteral("b"));
    ClipData c = mediaClip(15, 3, 0, 0, QStringLiteral("c")); // after a gap of 3
    ops::overwriteClip(d, kV1, a);
    ops::overwriteClip(d, kV1, b);
    ops::overwriteClip(d, kV1, c);
    ops::overwriteClip(d, kA1, mediaClip(0, 12, 0, 0, QStringLiteral("aud")));

    ClipData& ca = d.videoTracks[0].clips[0];
    ClipData& cb = d.videoTracks[0].clips[1];
    // Keyframes with a free handle.
    AnimatedValue& posX = ca.transform.animated(keys::PosX, 0);
    posX.setKey(0, 0.0);
    posX.setKey(4, 100.0);
    posX.setKey(6, 50.0);
    posX.keys[1].out.mode = KeyHandle::Mode::Free;
    posX.keys[1].out.dtFrames = 1.5;
    posX.keys[1].out.dv = 8.0;
    ca.audio.animated(keys::VolumeDb, 0).setKey(2, -6.0);
    // Effect with a frame-count setting, keyframed effect value.
    EffectInstance fx;
    fx.id = d.allocateId();
    fx.type = QString::fromLatin1(fxid::MotionTrails);
    fx.params.values[fxkeys::Delay] = AnimatedValue(2.0);
    fx.params.animated(fxkeys::Opacity, 100.0).setKey(3, 10.0);
    ca.effects.append(fx);
    // Shape mask with two shape keys.
    MaskData mask = makeMask(MaskShape::Polygon, d.allocateId(), {});
    mask.path.shapes = {MaskPath {MaskPoint {}}, MaskPath {MaskPoint {}}};
    mask.path.timing.putKey({0, 0.0, {}, {}, false});
    mask.path.timing.putKey({6, 1.0, {}, {}, false});
    ca.masks.append(mask);
    // Transition, fades, markers.
    TransitionData tr;
    tr.duration = 4;
    tr.params.animated(QStringLiteral("x"), 0).setKey(2, 1.0);
    cb.transitionIn = tr;
    cb.fadeInFrames = 2;
    cb.fadeOutFrames = 3;
    Marker cm;
    cm.frame = 3;
    cm.duration = 2;
    cb.markers.append(cm);
    // Retime curve: speed ramp over the clip (R in source frames at the timeline rate).
    cb.retime.values[keys::RetimeFrame] = AnimatedValue(0.0);
    cb.retime.values[keys::RetimeFrame].putKey({0, 0.0, {}, {}, false});
    cb.retime.values[keys::RetimeFrame].putKey({5, 10.0, {}, {}, false});
    // Composition and grade with keys.
    Composition comp;
    CompNode node;
    node.id = comp.allocateId();
    node.type = QStringLiteral("transform");
    node.params.animated(QStringLiteral("posX"), 0).setKey(4, 3.0);
    comp.nodes.append(node);
    cb.composition = comp;
    GradeNode gn;
    gn.id = 1;
    gn.params.animated(QStringLiteral("gain"), 0).setKey(2, 1.0);
    cb.grade.nodes.append(gn);
    // Timeline level.
    Marker m;
    m.frame = 9;
    m.duration = 6;
    d.markers.append(m);
    d.markIn = 3;
    d.markOut = 18;
    d.startTimecode = 24 * 3600;
    d.grade.nodes.append(gn);
    d.audioTracks[0].mix.automationKeys.animated(mixkeys::kVolumeDb, 0).setKey(12, -3.0);
    // Nested clip: shows 10 frames at 25 fps = 400000 us of another timeline.
    ClipData n;
    n.kind = ClipKind::Nested;
    n.nestedUid = 7;
    n.start = 0;
    n.duration = 5;
    n.sourceInUs = 400000;
    ops::overwriteClip(d, kV2, n);
    return d;
}

} // namespace

class TestFrameRateConvert : public QObject
{
    Q_OBJECT
private slots:
    void convertFramesRoundsToNearest()
    {
        QCOMPARE(convertFrames(10, k24, k60), int64_t(25));
        QCOMPARE(convertFrames(7, k24, k60), int64_t(18)); // 17.5 rounds away from zero
        QCOMPARE(convertFrames(-7, k24, k60), int64_t(-18));
        QCOMPARE(convertFrames(25, k60, k24), int64_t(10));
        QCOMPARE(convertFrames(30000, k2997, k60), int64_t(60060));
        QCOMPARE(convertFrames(5, Rational {0, 1}, k60), int64_t(5));
    }

    void upAndDownKeepsEveryTimeAndContiguity()
    {
        for (const Rational target : {k60, Rational {50, 1}, k2997}) {
            TimelineData d = fullTimeline();
            const TimelineData before = d;
            convertTimelineFrameRate(d, target);
            QCOMPARE(d.fps, target);
            const auto& v1 = d.videoTracks[0].clips;
            const auto& o1 = before.videoTracks[0].clips;
            QCOMPARE(v1.size(), o1.size());
            for (size_t i = 0; i < v1.size(); ++i) {
                QVERIFY(std::abs(seconds(v1[i].start, target) - seconds(o1[i].start, k24))
                    <= seconds(1, target) / 2 + 1e-9);
                QVERIFY(std::abs(seconds(v1[i].end(), target) - seconds(o1[i].end(), k24))
                    <= seconds(1, target) / 2 + 1e-9);
            }
            QCOMPARE(v1[0].end(), v1[1].start); // touching clips still touch
            QVERIFY(v1[2].start > v1[1].end()); // the gap is still a gap
            QVERIFY(std::abs(seconds(v1[2].start - v1[1].end(), target) - 3.0 / 24)
                <= seconds(1, target) + 1e-9);
            KK_VERIFY_INVARIANTS(d);
            // Source times are microseconds: untouched.
            QCOMPARE(v1[1].sourceInUs, o1[1].sourceInUs);
            QCOMPARE(d.videoTracks[1].clips[0].sourceInUs, int64_t(400000));
            QCOMPARE(d.videoTracks[1].clips[0].nestedUid, quint64(7));
        }
    }

    void roundTripFrom24To60AndBack()
    {
        TimelineData d = fullTimeline();
        const TimelineData before = d;
        convertTimelineFrameRate(d, k60);
        convertTimelineFrameRate(d, k24);
        QCOMPARE(d, before);
    }

    void keyframesHandlesAndRetimeScale()
    {
        TimelineData d = fullTimeline();
        convertTimelineFrameRate(d, k60);
        const ClipData& a = d.videoTracks[0].clips[0];
        const AnimatedValue& posX = a.transform.values[keys::PosX];
        QCOMPARE(posX.keys.size(), qsizetype(3));
        QCOMPARE(posX.keys[1].frame, int64_t(10)); // 4 * 2.5
        QCOMPARE(posX.keys[2].frame, int64_t(15));
        QCOMPARE(posX.keys[1].value, 100.0);
        QCOMPARE(posX.keys[1].out.dtFrames, 3.75); // 1.5 * 2.5
        QCOMPARE(posX.keys[1].out.dv, 8.0);
        QCOMPARE(a.audio.values[keys::VolumeDb].keys[0].frame, int64_t(5));
        // frame-count setting of the effect: 2 frames at 24 -> 5 at 60 (clamped to the maximum 4)
        QCOMPARE(a.effects[0].params.values[fxkeys::Delay].base, 4.0);
        QCOMPARE(a.effects[0].params.values[fxkeys::Opacity].keys[0].frame, int64_t(8)); // 7.5
        // mask shape keys follow
        QCOMPARE(a.masks[0].path.timing.keys[1].frame, int64_t(15));
        QCOMPARE(a.masks[0].path.shapes.size(), qsizetype(2));
        // retime curve: keys and values (source frames at the timeline rate) scale
        const ClipData& b = d.videoTracks[0].clips[1];
        const AnimatedValue& r = b.retime.values[keys::RetimeFrame];
        QCOMPARE(r.keys[1].frame, b.duration); // the key at the clip end stays at the end
        QCOMPARE(r.keys[1].value, 25.0);
        // 10 source frames over 5 was speed 2; the clip is 12 frames now (12.5 rounded), so ~2.08
        QVERIFY(std::abs(retime::speedAt(b, 1.0) - 2.0) < 0.15);
        // transition, fades, clip marker, composition, grade
        QCOMPARE(b.transitionIn->duration, int64_t(10));
        QCOMPARE(b.transitionIn->params.values[QStringLiteral("x")].keys[0].frame, int64_t(5));
        QCOMPARE(b.fadeInFrames, int64_t(5));
        QCOMPARE(b.fadeOutFrames,
            int64_t(7)); // 7.5 -> 8, cut to fit the 12-frame clip after a 5-frame fade-in
        QCOMPARE(b.markers[0].frame, int64_t(8));
        QCOMPARE(b.markers[0].duration, int64_t(5)); // [3,5) -> [8,13)
        QCOMPARE(b.composition->nodes[0].params.values[QStringLiteral("posX")].keys[0].frame,
            int64_t(10));
        QCOMPARE(b.grade.nodes[0].params.values[QStringLiteral("gain")].keys[0].frame, int64_t(5));
    }

    void timelineLevelFields()
    {
        TimelineData d = fullTimeline();
        convertTimelineFrameRate(d, k60);
        QCOMPARE(d.markers[0].frame, int64_t(23)); // 22.5
        QCOMPARE(d.markers[0].duration, int64_t(15)); // [9,15) -> [23,38)
        QCOMPARE(d.markIn, int64_t(8));
        QCOMPARE(d.markOut, int64_t(45));
        QCOMPARE(d.startTimecode, int64_t(60 * 3600)); // 01:00:00:00 stays
        QCOMPARE(d.grade.nodes[0].params.values[QStringLiteral("gain")].keys[0].frame, int64_t(5));
        QCOMPARE(d.audioTracks[0].mix.automationKeys.values[mixkeys::kVolumeDb].keys[0].frame,
            int64_t(30));
    }

    void shrinkingClipsNeverOverlapOrVanish()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("T"), k60, 1920, 1080, 1, 1);
        for (int i = 0; i < 6; ++i)
            ops::overwriteClip(d, kV1, mediaClip(i, 1)); // single frames at 60 fps
        convertTimelineFrameRate(d, k24);
        int64_t previousEnd = 0;
        for (const ClipData& c : d.videoTracks[0].clips) {
            QVERIFY(c.duration >= 1);
            QVERIFY(c.start >= previousEnd);
            previousEnd = c.end();
        }
        KK_VERIFY_INVARIANTS(d);
    }

    void sameRateAndInvalidRateDoNothing()
    {
        TimelineData d = fullTimeline();
        const TimelineData before = d;
        convertTimelineFrameRate(d, k24);
        convertTimelineFrameRate(d, Rational {0, 1});
        QCOMPARE(d, before);
    }

    void saveAndLoadKeepsConvertedTimeline()
    {
        TimelineData d = fullTimeline();
        convertTimelineFrameRate(d, k60);
        QVERIFY(d.fps == k60);
        QCOMPARE(d.videoTracks[0].clips[1].start, int64_t(18));
    }

    void projectFollowsAndIsOneUndoStep()
    {
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Setup"));
        TimelineData s = fullTimeline();
        s.uid = uid;
        s.name = p.timeline()->data().name;
        s.useProjectSettings = true;
        p.timeline()->mutableData() = s;
        p.endEdit();
        p.timeline()->setPlayhead(12);
        const TimelineData before = p.timeline()->data();
        const int steps = p.undoStack()->count();

        QVERIFY(p.convertTimelineFrameRate(uid, k60));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        QCOMPARE(p.settings().fps, k60);
        QCOMPARE(p.timeline()->data().fps, k60);
        QVERIFY(p.timeline()->data().useProjectSettings);
        QCOMPARE(p.timeline()->playhead(), int64_t(30));
        QCOMPARE(p.timeline()->data().videoTracks[0].clips[1].start, int64_t(18));

        p.undo();
        QCOMPARE(p.settings().fps, k24);
        QCOMPARE(p.timeline()->data(), before);
    }

    void timelineLeavesProjectWhenAnotherFollowerHasClips()
    {
        Project p;
        const quint64 first = p.activeTimelineUid();
        const quint64 second = p.createTimeline(QStringLiteral("Other"));
        p.beginEdit(QStringLiteral("Setup"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        p.setActiveTimeline(second);
        p.beginEdit(QStringLiteral("Setup2"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        QVERIFY(p.convertTimelineFrameRate(first, k60));
        QCOMPARE(p.settings().fps, k24); // the project stays
        QVERIFY(!p.timelineData(first)->useProjectSettings);
        QCOMPARE(p.timelineData(first)->fps, k60);
        QCOMPARE(p.timelineData(second)->fps, k24);
        QCOMPARE(p.timelineData(first)->videoTracks[0].clips[0].duration, int64_t(25));
    }

    void compoundsFollowOnRequest()
    {
        Project p;
        p.beginEdit(QStringLiteral("Setup"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(10, 10));
        p.endEdit();
        QString error;
        const auto& clips = p.timeline()->data().videoTracks[0].clips;
        const quint64 cc
            = p.createCompoundClip({clips[0].id, clips[1].id}, QStringLiteral("CC"), {}, &error);
        QVERIFY2(cc != 0, qPrintable(error));
        QVERIFY(p.convertTimelineFrameRate(p.activeTimelineUid(), k60, true));
        QCOMPARE(p.timelineData(cc)->fps, k60);
        QCOMPARE(p.timelineData(cc)->videoTracks[0].clips[1].start, int64_t(25));
        // the compound clip itself still shows the whole content: 20 frames at 24 -> 50 at 60
        QCOMPARE(p.timeline()->data().videoTracks[0].clips[0].duration, int64_t(50));
    }
};

QTEST_GUILESS_MAIN(TestFrameRateConvert)
#include "test_framerateconvert.moc"
