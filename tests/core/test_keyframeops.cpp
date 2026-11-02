// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/KeyframeOps: paths, grouped listing, navigation and keyframe edits used by
// the timeline keyframe editor and the curve editor (docs/behavior-spec.md section 15).
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"

#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ko = kk::keyframeops;

namespace {
const QString kPosX = QStringLiteral("transform/posX");
const QString kOpacity = QStringLiteral("transform/opacity");
const QString kCropL = QStringLiteral("transform/cropL");
const QString kVolume = QStringLiteral("audio/volumeDb");

// A timeline with one 100-frame clip on V1 animated on posX (0, 50), opacity (50) and cropL (20).
struct Fixture
{
    TimelineData d = timeline();
    quint64 id = 0;
    Fixture()
    {
        ClipData c = mediaClip(10, 100);
        c.transform.animated(keys::PosX, 0).setKey(0, 0.0);
        c.transform.animated(keys::PosX, 0).setKey(50, 100.0);
        c.transform.animated(keys::Opacity, 100).setKey(50, 40.0);
        c.transform.animated(keys::CropL, 0).setKey(20, 8.0);
        c.transform.setBase(keys::Rotation, 5.0); // not animated: not listed
        id = editops::overwriteClip(d, kV1, c);
    }
    ClipData& clip() { return *editops::clipById(d, id); }
    const AnimatedValue& value(const QString& path) { return *ko::findValue(clip(), path); }
};
} // namespace

class TestKeyframeOps : public QObject
{
    Q_OBJECT
private slots:
    void spec15_9_pathsResolve()
    {
        Fixture f;
        QCOMPARE(ko::memberPath(QStringLiteral("transform"), keys::PosX), kPosX);
        QVERIFY(ko::findValue(f.clip(), kPosX));
        QVERIFY(!ko::findValue(f.clip(), QStringLiteral("transform/nope")));
        QVERIFY(!ko::findValue(f.clip(), QStringLiteral("bogus")));
        EffectInstance fx
            = EffectRegistry::instance().createEffect(QStringLiteral("gaussian_blur"), 77);
        fx.params.animated(QStringLiteral("radius"), 0).setKey(3, 1.0);
        f.clip().effects.append(fx);
        const QString path = ko::effectPath(77, QStringLiteral("radius"));
        QCOMPARE(path, QStringLiteral("effect/77/radius"));
        QVERIFY(ko::findValue(f.clip(), path));
        QVERIFY(!ko::findValue(f.clip(), QStringLiteral("effect/78/radius")));
    }

    // The keyframe editor lists animated parameters grouped like the inspector.
    void spec15_9_animatedParamsInInspectorOrder()
    {
        Fixture f;
        f.clip().audio.animated(keys::VolumeDb, 0).setKey(5, -6.0);
        EffectInstance fx
            = EffectRegistry::instance().createEffect(QStringLiteral("gaussian_blur"), 77);
        fx.params.animated(QStringLiteral("radius"), 0).setKey(3, 1.0);
        f.clip().effects.append(fx);
        const QVector<ko::AnimatedParam> params = ko::animatedParams(f.clip());
        QStringList paths;
        for (const auto& p : params)
            paths << p.path;
        QCOMPARE(paths,
            QStringList({kPosX, kOpacity, kCropL, kVolume, QStringLiteral("effect/77/radius")}));
        QCOMPARE(params[0].groupId, QString::fromLatin1(groups::Transform));
        QCOMPARE(params[2].groupId, QString::fromLatin1(groups::Cropping));
        QCOMPARE(params[4].groupId, QStringLiteral("effect/77"));
        QVERIFY(!params[0].label.isEmpty());
    }

    void spec15_9_masterRowAndNavigation()
    {
        Fixture f;
        QCOMPARE(ko::allKeyFrames(f.clip()), QVector<int64_t>({0, 20, 50}));
        QCOMPARE(ko::adjacentKeyFrame(f.clip(), 0, true), std::optional<int64_t>(20));
        QCOMPARE(ko::adjacentKeyFrame(f.clip(), 20, false), std::optional<int64_t>(0));
        QCOMPARE(ko::adjacentKeyFrame(f.clip(), 50, true), std::nullopt);
        QCOMPARE(ko::keysAt(f.clip(), 50).size(), 2);
        QCOMPARE(ko::allKeys(f.clip()).size(), 4);
        QCOMPARE(ko::keysInRange(f.clip(), 10, 60).size(), 3);
        QCOMPARE(ko::keysInRange(f.clip(), 10, 60, {kPosX}).size(), 1);
    }

    // Moving keys together; collisions replace the unmoved key.
    void spec15_9_moveKeys()
    {
        Fixture f;
        const QVector<KeyframeRef> refs {{f.id, kPosX, 0}, {f.id, kPosX, 50}, {f.id, kOpacity, 50}};
        const QVector<KeyframeRef> moved = ko::moveKeys(f.d, refs, 10);
        QCOMPARE(moved.size(), 3);
        QCOMPARE(moved[0].frame, 10);
        QCOMPARE(f.value(kPosX).keys[1].frame, 60);
        QCOMPARE(f.value(kOpacity).keys[0].frame, 60);
        // Keys moving onto each other's old frame never overwrite each other.
        ko::moveKeys(f.d, {{f.id, kPosX, 10}, {f.id, kPosX, 60}}, 50);
        QCOMPARE(f.value(kPosX).keys.size(), 2);
        QCOMPARE(f.value(kPosX).keys[0].frame, 60);
        QCOMPARE(f.value(kPosX).keys[0].value, 0.0);
        // A key moved onto an unselected key replaces it.
        ko::moveKeys(f.d, {{f.id, kPosX, 60}}, 50);
        QCOMPARE(f.value(kPosX).keys.size(), 1);
        QCOMPARE(f.value(kPosX).keys[0].value, 0.0);
    }

    void spec15_9_moveDeltaClampedToClip()
    {
        Fixture f;
        QCOMPARE(ko::clampMoveDelta(f.d, {{f.id, kPosX, 50}}, 80), 49); // last frame is 99
        QCOMPARE(ko::clampMoveDelta(f.d, {{f.id, kPosX, 50}}, -80), -50);
        // Together: the most constrained key wins.
        QCOMPARE(ko::clampMoveDelta(f.d, {{f.id, kPosX, 0}, {f.id, kPosX, 50}}, -5), 0);
        // A key already outside (head trimmed) may stay there but not move further out.
        QCOMPARE(ko::clampMoveDelta(f.d, {{f.id, kPosX, -10}}, -5), 0);
        QCOMPARE(ko::clampMoveDelta(f.d, {{f.id, kPosX, -10}}, 4), 4);
    }

    void spec15_9_deleteAndInterp()
    {
        Fixture f;
        QCOMPARE(ko::setInterp(f.d, {{f.id, kPosX, 0}, {f.id, kPosX, 7}}, Interp::EaseInOut), 1);
        QCOMPARE(keyInterp(f.value(kPosX).keys[0]), Interp::EaseInOut);
        QCOMPARE(ko::deleteKeys(f.d, {{f.id, kOpacity, 50}, {f.id, kCropL, 99}}), 1);
        QVERIFY(!f.value(kOpacity).isAnimated());
        QCOMPARE(f.value(kOpacity).base, 40.0); // last key's value is kept
    }

    void spec15_12_flattenAndHandles()
    {
        Fixture f;
        ko::setInterp(f.d, {{f.id, kPosX, 50}}, Interp::Smooth);
        QVERIFY(ko::setHandle(f.d, {f.id, kPosX, 50}, ko::HandleSide::In, 10.0, 20.0, true));
        const Keyframe& k = f.value(kPosX).keys[1];
        QCOMPARE(k.in.mode, KeyHandle::Mode::Free);
        QCOMPARE(k.out.mode, KeyHandle::Mode::Free); // kept collinear
        QCOMPARE(k.out.dv / k.out.dtFrames, 2.0);
        QVERIFY(ko::setHandle(f.d, {f.id, kPosX, 50}, ko::HandleSide::Out, 5.0, -5.0, false));
        QCOMPARE(f.value(kPosX).keys[1].in.dv, 20.0); // broken tangent: in untouched
        QCOMPARE(ko::flattenHandles(f.d, {{f.id, kPosX, 50}, {f.id, kPosX, 0}}), 2);
        QCOMPARE(f.value(kPosX).keys[1].in.dv, 0.0);
        QCOMPARE(f.value(kPosX).keys[0].out.mode, KeyHandle::Mode::Ease);
        QVERIFY(!ko::setHandle(f.d, {f.id, kPosX, 49}, ko::HandleSide::In, 1, 1, false));
    }

    // Spec 15.7 (decision): keys follow the source content on head trims; keys trimmed out of
    // the clip are kept (not deleted), so extending the clip again brings them back; a slip
    // keeps keys at their clip-relative positions (the animation stays in sync with the edit).
    void spec15_7_keysSurviveTrimAndSlip()
    {
        Fixture f;
        f.clip().sourceDurationUs = 0; // unbounded source for the test
        QCOMPARE(editops::trimHead(f.d, f.id, 30, false), 30);
        QCOMPARE(f.value(kPosX).keys[0].frame, -30); // outside, kept
        QCOMPARE(f.value(kPosX).keys[1].frame, 20);
        QCOMPARE(editops::trimHead(f.d, f.id, -30, false), -30);
        QCOMPARE(f.value(kPosX).keys[0].frame, 0);
        QCOMPARE(editops::trimTail(f.d, f.id, -60, false), -60); // duration 40: key 50 outside
        QCOMPARE(f.value(kPosX).keys[1].frame, 50);
        f.clip().sourceInUs = 100 * 40000;
        editops::slipClip(f.d, f.id, -10);
        QCOMPARE(f.value(kPosX).keys[1].frame, 50);
    }

    void spec15_9_offsetValues()
    {
        Fixture f;
        QCOMPARE(ko::offsetKeyValues(f.d, {{f.id, kPosX, 50}, {f.id, kOpacity, 50}}, -10.0), 2);
        QCOMPARE(f.value(kPosX).keys[1].value, 90.0);
        QCOMPARE(f.value(kOpacity).keys[0].value, 30.0);
    }

    void spec15_9_copyPaste()
    {
        Fixture f;
        ko::setInterp(f.d, {{f.id, kPosX, 50}}, Interp::Hold);
        const ko::KeyClipboard board
            = ko::copyKeys(f.d, {{f.id, kPosX, 50}, {f.id, kCropL, 20}, {f.id, kPosX, 99}});
        QCOMPARE(board.entries.size(), 2);
        // Into another clip that has no animation yet: parameters are created.
        ClipData other = mediaClip(200, 50);
        const quint64 otherId = editops::overwriteClip(f.d, kV1, other);
        const QVector<KeyframeRef> pasted = ko::pasteKeys(f.d, otherId, board, 5);
        QCOMPARE(pasted.size(), 2);
        const ClipData& o = *editops::clipById(f.d, otherId);
        QCOMPARE(ko::findValue(o, kCropL)->keys[0].frame, 5);
        QCOMPARE(ko::findValue(o, kPosX)->keys[0].frame, 35);
        QVERIFY(ko::findValue(o, kPosX)->keys[0].hold);
        QCOMPARE(ko::findValue(o, kPosX)->keys[0].value, 100.0);
    }
};

QTEST_GUILESS_MAIN(TestKeyframeOps)
#include "test_keyframeops.moc"
