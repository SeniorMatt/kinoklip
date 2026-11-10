// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorPanel: generic editors from descriptors, keyframe toggling, scrub undo grouping,
// cheap playhead refreshes, effects list editing, linked audio clips.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/ParamKeys.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/MultiClipEdit.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};
const TrackRef kA1 {TrackType::Audio, 0};

/// Sends a full press / drag / release sequence (QTest::mouseMove does not carry buttons).
void drag(QWidget* w, int fromX, const QVector<int>& xs, Qt::KeyboardModifiers mods = {})
{
    const int y = w->height() / 2;
    auto send
        = [w, mods](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons buttons) {
              QMouseEvent e(type, QPointF(x, w->height() / 2), w->mapToGlobal(QPointF(x, 0)),
                  button, buttons, mods);
              QApplication::sendEvent(w, &e);
          };
    Q_UNUSED(y);
    send(QEvent::MouseButtonPress, fromX, Qt::LeftButton, Qt::LeftButton);
    for (int x : xs)
        send(QEvent::MouseMove, x, Qt::NoButton, Qt::LeftButton);
    send(
        QEvent::MouseButtonRelease, xs.isEmpty() ? fromX : xs.back(), Qt::LeftButton, Qt::NoButton);
}

ParamGroupBox* groupTitled(InspectorPanel& panel, const QString& title)
{
    for (ParamGroupBox* g : panel.groups()) {
        if (g->findChild<QLabel*>(QStringLiteral("InspectorGroupTitle"))->text() == title)
            return g;
    }
    return nullptr;
}

} // namespace

class TestInspector : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    void emptyState()
    {
        PanelFixture f;
        InspectorPanel panel(f.ctx);
        QCOMPARE(panel.clipId(), quint64(0));
        QVERIFY(panel.groups().isEmpty());
        QCOMPARE(panel.tabs()->count(), 5);
    }

    void buildsEditorsFromDescriptor()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 10, 100);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        f.selection.selectClip(id);
        QCOMPARE(panel.clipId(), id);

        const EffectDescriptor* transform
            = EffectRegistry::instance().find(QLatin1String(groups::Transform));
        ParamGroupBox* group = groupTitled(panel, transform->name);
        QVERIFY(group);
        QCOMPARE(group->rows().size(), transform->params.size());
        for (int i = 0; i < transform->params.size(); ++i)
            QCOMPARE(group->rows()[i]->ref().key, transform->params[i].key);
        // Scalars get a value field and a keyframe diamond; bools do not.
        QVERIFY(group->findChild<ValueField*>(QStringLiteral("value:") + keys::PosX));
        QVERIFY(group->findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::PosX));
        QVERIFY(!group->findChild<ValueField*>(QStringLiteral("value:") + keys::FlipH));
        // Opacity is in the Composite group (spec 24.19).
        ParamGroupBox* composite = groupTitled(
            panel, EffectRegistry::instance().find(QLatin1String(groups::Composite))->name);
        QVERIFY(composite);
        QVERIFY(composite->findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity));
        QVERIFY(composite->findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::Opacity));
        QVERIFY(!group->findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity));
        QVERIFY(groupTitled(
            panel, EffectRegistry::instance().find(QLatin1String(groups::Cropping))->name));

        // Adding an effect rebuilds and shows a group generated from its descriptor.
        const int rebuilds = panel.rebuildCount();
        QCOMPARE(effectactions::applyEffect(f.project, {id}, QStringLiteral("gaussian_blur")), 1);
        QCOMPARE(panel.rebuildCount(), rebuilds + 1);
        ParamGroupBox* blur = groupTitled(panel, tr("Gaussian Blur"));
        QVERIFY(blur);
        QVERIFY(blur->findChild<ValueField*>(QStringLiteral("value:radius")));
        saveScreenshot(panel, QStringLiteral("inspector_video"));
        panel.tabs()->setCurrentIndex(InspectorPanel::EffectsTab);
        saveScreenshot(panel, QStringLiteral("inspector_effects"));
    }

    void playheadMoveRefreshesWithoutRebuild()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 10, 100);
        // Animate opacity: 0 at local 0, 100 at local 50.
        f.project.beginEdit(QStringLiteral("keys"));
        ClipData* c = editops::clipById(f.project.timeline()->mutableData(), id);
        c->transform.animated(keys::Opacity, 100).setKey(0, 0);
        c->transform.animated(keys::Opacity, 100).setKey(50, 100);
        f.project.endEdit();

        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        auto* field = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity);
        auto* diamond
            = panel.findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::Opacity);
        QVERIFY(field && diamond);
        const int rebuilds = panel.rebuildCount();
        f.project.timeline()->setPlayhead(10 + 25);
        QCOMPARE(field->value(), 50.0);
        QCOMPARE(diamond->state(), KeyframeButton::State::Animated);
        f.project.timeline()->setPlayhead(10 + 50);
        QCOMPARE(field->value(), 100.0);
        QCOMPARE(diamond->state(), KeyframeButton::State::OnKey);
        QCOMPARE(panel.rebuildCount(), rebuilds);
    }

    // Spec 15.13: the section diamond keys every animatable parameter of the section.
    void spec15_13_sectionKeyframeDiamond()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 10, 100);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        f.project.timeline()->setPlayhead(30); // clip-local frame 20
        auto* section = panel.findChild<KeyframeButton*>(
            QStringLiteral("keyframe-section:") + QLatin1String(groups::Cropping));
        QVERIFY(section);
        QCOMPARE(section->state(), KeyframeButton::State::NotAnimated);
        const int commands = f.project.undoStack()->count();
        section->click();
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        const ClipData& c = *editops::clipById(f.project.timeline()->data(), id);
        for (const QString& key : {keys::CropL, keys::CropR, keys::CropT, keys::CropB})
            QVERIFY(c.transform.values.value(key).hasKeyAt(20));
        QVERIFY(!c.transform.values.value(keys::PosX).isAnimated()); // other section untouched
        QCOMPARE(section->state(), KeyframeButton::State::OnKey);
        f.project.timeline()->setPlayhead(50);
        QCOMPARE(section->state(), KeyframeButton::State::Animated);
        f.project.timeline()->setPlayhead(30);
        section->click(); // all on a key: removes them
        const ClipData& after = *editops::clipById(f.project.timeline()->data(), id);
        QVERIFY(!after.transform.values.value(keys::CropL).isAnimated());
        f.project.timeline()->setPlayhead(500); // outside the clip
        QVERIFY(!section->isEnabled());
    }

    void keyframeButtonTogglesAtLocalFrame()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 10, 100);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        f.project.timeline()->setPlayhead(30); // clip-local frame 20
        auto* diamond = panel.findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::PosX);
        QVERIFY(diamond);
        QCOMPARE(diamond->state(), KeyframeButton::State::NotAnimated);

        const int commands = f.project.undoStack()->count();
        diamond->click();
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        const AnimatedValue& posX = f.clip(id)->transform.values.value(keys::PosX);
        QCOMPARE(posX.keys.size(), 1);
        QCOMPARE(posX.keys.front().frame, int64_t(20));
        QCOMPARE(diamond->state(), KeyframeButton::State::OnKey);

        diamond->click(); // remove again
        QCOMPARE(f.project.undoStack()->count(), commands + 2);
        QVERIFY(!f.clip(id)->transform.values.value(keys::PosX).isAnimated());
        QCOMPARE(diamond->state(), KeyframeButton::State::NotAnimated);

        f.project.undo();
        QVERIFY(f.clip(id)->transform.values.value(keys::PosX).hasKeyAt(20));
        QCOMPARE(diamond->state(), KeyframeButton::State::OnKey);

        // Playhead outside the clip: the diamond is disabled.
        f.project.timeline()->setPlayhead(500);
        QVERIFY(!diamond->isEnabled());
    }

    void linkedZoomKeysBothAxes()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 50);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        f.project.timeline()->setPlayhead(5);
        panel.findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::ZoomX)->click();
        QVERIFY(f.clip(id)->transform.values.value(keys::ZoomX).hasKeyAt(5));
        QVERIFY(f.clip(id)->transform.values.value(keys::ZoomY).hasKeyAt(5));
    }

    void scrubIsSingleUndoStep()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 50);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        panel.show();
        f.selection.selectClip(id);
        auto* field = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::PosX);
        QVERIFY(field);
        const int commands = f.project.undoStack()->count();
        drag(field, 10, {12, 14, 20, 30, 40}); // 3 px threshold, then +26 px from x=14
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QVERIFY(!f.project.isEditing());
        QCOMPARE(f.clip(id)->transform.value(keys::PosX, 0), 26.0);
        QCOMPARE(field->value(), 26.0);

        // Fine scrubbing with Shift: step / 10.
        drag(field, 10, {14, 24}, Qt::ShiftModifier);
        QCOMPARE(f.project.undoStack()->count(), commands + 2);
        QCOMPARE(f.clip(id)->transform.value(keys::PosX, 0), 27.0);

        f.project.undo();
        QCOMPARE(f.clip(id)->transform.value(keys::PosX, 0), 26.0);
        QCOMPARE(field->value(), 26.0);

        // A plain click (no drag) creates no undo step.
        drag(field, 10, {});
        QCOMPARE(f.project.undoStack()->count(), commands + 2);
    }

    void scrubOnAnimatedParamSetsKey()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 100, 50);
        f.project.beginEdit(QStringLiteral("key"));
        editops::clipById(f.project.timeline()->mutableData(), id)
            ->transform.animated(keys::Rotation, 0)
            .setKey(0, 0);
        f.project.endEdit();
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        panel.show();
        f.selection.selectClip(id);
        f.project.timeline()->setPlayhead(110);
        auto* field = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Rotation);
        drag(field, 10, {14, 24}); // step 0.1 -> +1.0
        const AnimatedValue& rot = f.clip(id)->transform.values.value(keys::Rotation);
        QCOMPARE(rot.keys.size(), 2);
        QVERIFY(rot.hasKeyAt(10));
        QVERIFY(qFuzzyCompare(rot.valueAt(10), 1.0));
    }

    void effectsReorderEnableRemove()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 50);
        effectactions::applyEffect(f.project, {id}, QStringLiteral("gaussian_blur"));
        effectactions::applyEffect(f.project, {id}, QStringLiteral("mosaic"));
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        ParamGroupBox* mosaic = groupTitled(panel, tr("Mosaic"));
        QVERIFY(mosaic);
        // Header buttons: up, down, remove (after the reset chevron etc.); find by tool tip.
        auto button = [](ParamGroupBox* g, const QString& tip) -> QToolButton* {
            for (QToolButton* b : g->findChildren<QToolButton*>()) {
                if (b->toolTip() == tip)
                    return b;
            }
            return nullptr;
        };
        button(mosaic, QStringLiteral("Move up"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(id)->effects.front().type, QStringLiteral("mosaic"));

        ParamGroupBox* blur = groupTitled(panel, tr("Gaussian Blur"));
        QCheckBox* enable = blur->findChild<QCheckBox*>();
        QVERIFY(enable->isChecked());
        enable->click();
        QVERIFY(!f.clip(id)->effects.back().enabled);

        button(blur, QStringLiteral("Remove effect"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(id)->effects.size(), 1);
        f.project.undo();
        QCOMPARE(f.clip(id)->effects.size(), 2);
    }

    void audioTabFollowsLinkedClip()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        f.project.beginEdit(QStringLiteral("link"));
        editops::linkClips(f.project.timeline()->mutableData(), {v, a});
        f.project.endEdit();
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(v);
        QCOMPARE(panel.audioClipId(), a);
        auto* volume = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::VolumeDb);
        QVERIFY(volume);
        f.selection.selectClip(a);
        QCOMPARE(panel.audioClipId(), a);
        QCOMPARE(panel.clipId(), v); // the pair is one item (spec 24.1)
    }

    // Spec 67: the Audio tab has Pitch and an Equalizer section; edits are one undo step each.
    void spec67_audioTabHasPitchAndEqualizer()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        panel.show();
        f.selection.selectClip(a);
        auto* pitch = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::PitchSemitones);
        auto* cents = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::PitchCents);
        auto* gain = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::EqBandGain[1]);
        QVERIFY(pitch && cents && gain);
        QVERIFY(panel.findChild<ValueField*>(QStringLiteral("value:") + keys::EqBandFreq[3]));
        const int steps = f.project.undoStack()->count();
        emit pitch->valueCommitted(5.0);
        QCOMPARE(f.clip(a)->audio.value(keys::PitchSemitones, 0), 5.0);
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        emit gain->valueCommitted(6.0);
        QCOMPARE(f.clip(a)->audio.value(keys::EqBandGain[1], 0), 6.0);
        f.project.undo();
        QCOMPARE(f.clip(a)->audio.value(keys::EqBandGain[1], 0), 0.0);
        f.project.undo();
        QCOMPARE(f.clip(a)->audio.value(keys::PitchSemitones, 0), 0.0);
        // A speed change greys the pitch out (it would be ignored), with a tooltip.
        QVERIFY(pitch->isEnabled() && cents->isEnabled());
        f.project.beginEdit(QStringLiteral("speed"));
        f.project.timeline()->mutableData().audioTracks[0].clips[0].speed = 2.0;
        f.project.endEdit();
        QCoreApplication::processEvents();
        QVERIFY(!pitch->isEnabled() && !cents->isEnabled());
        QVERIFY(pitch->toolTip().contains(QLatin1String("normal speed")));
        f.project.undo();
        QCoreApplication::processEvents();
        QVERIFY(pitch->isEnabled() && cents->isEnabled());
        // Not keyframable: no keyframe button for the pitch.
        QVERIFY(!f.clip(a)->audio.isAnimated());
    }

    void spec24_1_linkedPairIsOneItem()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        f.project.beginEdit(QStringLiteral("link"));
        editops::linkClips(f.project.timeline()->mutableData(), {v, a});
        f.project.endEdit();
        InspectorPanel panel(f.ctx);
        auto* info = panel.findChild<QLabel*>(QStringLiteral("InspectorClipInfo"));
        // Linked selection selects both; the audio clip may well be the last one.
        f.selection.setClips({v, a});
        QCOMPARE(panel.clipId(), v);
        QCOMPARE(panel.audioClipId(), a);
        QVERIFY(!info->text().contains(QLatin1String("selected")));
        // The Video tab edits the video clip (owner's alpha.2 report: it said "Audio clips have
        // no video properties").
        QVERIFY(panel.findChild<ValueField*>(QStringLiteral("value:") + keys::ZoomX));
        // Another, unrelated clip as well: two items.
        const quint64 other = f.addClip(kV1, 60, 20);
        f.selection.addClips({other});
        QCOMPARE(panel.clipId(), other);
        // Both are video clips of the same kind: an edit reaches both (multi-clip editing).
        QCOMPARE(info->text(), QStringLiteral("2 clips selected, edits apply to all"));
        // A title among them is another kind: the edit reaches only the media clips.
        const quint64 title = f.addClip(kV1, 90, 20, ClipKind::Solid);
        f.selection.addClips({title});
        QCOMPARE(info->text(), QStringLiteral("3 clips selected, showing the last one"));
    }

    /// Multi-clip editing (spec 24.1): with several clips of the same kind selected, a value
    /// typed in the Inspector goes to all of them in one undo step; different values show as
    /// "Mixed"; a drag is one step too; clips of another kind or on a locked track are left
    /// alone; effect parameters reach the matching effect of the other clips.
    void spec24_1_multiClipEditsApplyToAll()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kV1, 0, 50);
        const quint64 b = f.addClip(kV1, 60, 50);
        const quint64 solid = f.addClip(kV1, 120, 20, ClipKind::Solid);
        InspectorPanel panel(f.ctx);
        f.selection.setClips({a, solid, b}); // b shown (last clicked)
        QCOMPARE(panel.clipId(), b);
        auto* zoom = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::ZoomX);
        QVERIFY(zoom);
        QVERIFY(!zoom->isMixed());
        const int before = f.project.undoStack()->count();
        emit zoom->valueCommitted(1.5);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        const TimelineData& d = f.project.timeline()->data();
        auto zoomOf
            = [&](quint64 id) { return editops::clipById(d, id)->transform.value(keys::ZoomX, 0); };
        QCOMPARE(zoomOf(a), 1.5);
        QCOMPARE(zoomOf(b), 1.5);
        QCOMPARE(zoomOf(solid), 1.0); // another kind
        // Different values: "Mixed" until the next edit.
        f.project.beginEdit(QStringLiteral("setup"));
        editops::clipById(f.project.timeline()->mutableData(), a)
            ->transform.animated(keys::ZoomX, 1.0)
            .setValue(0, 2.0);
        f.project.endEdit();
        QCoreApplication::processEvents();
        QVERIFY(zoom->isMixed());
        // A drag: one undo step for all.
        const int beforeDrag = f.project.undoStack()->count();
        emit zoom->scrubStarted();
        emit zoom->valueScrubbed(1.2);
        emit zoom->valueScrubbed(1.3);
        emit zoom->scrubFinished();
        QCOMPARE(f.project.undoStack()->count(), beforeDrag + 1);
        QCOMPARE(zoomOf(a), 1.3);
        QCOMPARE(zoomOf(b), 1.3);
        QVERIFY(!zoom->isMixed());
        f.project.undoStack()->undo();
        QCOMPARE(zoomOf(a), 2.0);
        QCOMPARE(zoomOf(b), 1.5);
        // A locked track is left alone.
        const quint64 locked = f.addClip(TrackRef {TrackType::Video, 1}, 0, 50);
        f.project.beginEdit(QStringLiteral("lock"));
        f.project.timeline()->mutableData().videoTracks[1].locked = true;
        f.project.endEdit();
        f.selection.setClips({locked, a, b});
        emit zoom->valueCommitted(0.5);
        QCOMPARE(zoomOf(a), 0.5);
        QCOMPARE(zoomOf(locked), 1.0);
    }

    /// Effect parameters: the matching instance (same type, same position among that type)
    /// of every selected clip changes with the shown clip's.
    void spec24_1_multiClipEffectParams()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kV1, 0, 50);
        const quint64 b = f.addClip(kV1, 60, 50);
        effectactions::applyEffect(f.project, {a}, QStringLiteral("gaussian_blur"));
        effectactions::applyEffect(f.project, {b}, QStringLiteral("gaussian_blur"));
        const ClipData before = *editops::clipById(f.project.timeline()->data(), a);
        ClipData after = before;
        QVERIFY(!after.effects.isEmpty());
        after.effects[0].params.animated(QStringLiteral("radius"), 0).setValue(0, 7.0);
        after.effects[0].enabled = false;
        ClipData peer = *editops::clipById(f.project.timeline()->data(), b);
        QVERIFY(multiclip::propagate(before, after, peer));
        QCOMPARE(peer.effects[0].params.value(QStringLiteral("radius"), 0, 0), 7.0);
        QVERIFY(!peer.effects[0].enabled);
        const ParamRef ref {a, ParamScope::Effect, after.effects[0].id, QStringLiteral("radius")};
        const auto pref = multiclip::peerRef(after, peer, ref);
        QVERIFY(pref);
        QCOMPARE(pref->effectId, peer.effects[0].id);
        QCOMPARE(pref->clipId, b);
        // Nothing changed: nothing to copy.
        QVERIFY(!multiclip::propagate(after, after, peer));
    }

    void spec24_2_tabFollowsClickedPart()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        f.project.beginEdit(QStringLiteral("link"));
        editops::linkClips(f.project.timeline()->mutableData(), {v, a});
        f.project.endEdit();
        InspectorPanel panel(f.ctx);
        f.selection.setClips({v, a}); // audio part clicked (last = current)
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::AudioTab));
        f.selection.setClips({a, v}); // video part clicked
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::VideoTab));
        // Other tabs are left alone.
        panel.tabs()->setCurrentIndex(InspectorPanel::EffectsTab);
        f.selection.setClips({v, a});
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::EffectsTab));
    }

    void spec13_10_selectedTransitionShowsTransitionTab()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 100);
        effectactions::applyTransition(f.project, id, QStringLiteral("wipe"), true);
        InspectorPanel panel(f.ctx);
        f.selection.selectTransition({id, true});
        QCOMPARE(panel.clipId(), id);
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::TransitionTab));
    }

    void titleClipTextIsOneUndoStepPerBurst()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 0);
        QVERIFY(id);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 900);
        f.selection.selectClip(id);
        panel.tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        auto* text = panel.findChild<QPlainTextEdit*>();
        QVERIFY(text);
        const int commands = f.project.undoStack()->count();
        text->setPlainText(QStringLiteral("Hello"));
        text->appendPlainText(QStringLiteral("World"));
        QVERIFY(f.project.isEditing());
        QTest::qWait(1000); // idle timeout closes the step
        QVERIFY(!f.project.isEditing());
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QCOMPARE(
            f.clip(id)->title.staticValue(keys::Text).toString(), QStringLiteral("Hello\nWorld"));
        saveScreenshot(panel, QStringLiteral("inspector_title"));
    }

    void colorParamUsesPicker()
    {
        PanelFixture f;
        const quint64 id
            = effectactions::addGeneratorClip(f.project, QStringLiteral("solid_color"), 0);
        InspectorPanel panel(f.ctx);
        panel.setColorPicker([](const QColor&, const QString&) { return QColor(Qt::red); });
        f.selection.selectClip(id);
        ParamGroupBox* solid = groupTitled(panel, tr("Solid Color"));
        QVERIFY(solid);
        QToolButton* swatch = nullptr;
        for (QToolButton* b : solid->findChildren<QToolButton*>()) {
            if (b->toolTip() == QStringLiteral("Click to choose a color"))
                swatch = b;
        }
        QVERIFY(swatch);
        swatch->click();
        QCOMPARE(f.clip(id)->solidColor, QColor(Qt::red));
    }

    void transitionTab()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 100);
        effectactions::applyTransition(f.project, id, QStringLiteral("wipe"), true);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        auto* duration = panel.findChild<ValueField*>(QStringLiteral("value:@duration"));
        QVERIFY(duration);
        QCOMPARE(duration->value(), 25.0);
        panel.resize(420, 700);
        panel.tabs()->setCurrentIndex(InspectorPanel::TransitionTab);
        saveScreenshot(panel, QStringLiteral("inspector_transition"));
    }

    void addKeyframeShortcutUsesActiveParam()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 100);
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        panel.show();
        f.selection.selectClip(id);
        f.project.timeline()->setPlayhead(7);
        auto* field = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity);
        drag(field, 10, {14, 10}); // touch the parameter
        f.actions.action(QStringLiteral("clip.add_keyframe"))->trigger();
        QVERIFY(f.clip(id)->transform.values.value(keys::Opacity).hasKeyAt(7));

        f.project.timeline()->setPlayhead(50);
        f.actions.action(QStringLiteral("clip.prev_keyframe"))->trigger();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(7));
    }
};

QTEST_MAIN(TestInspector)
#include "test_inspector.moc"
