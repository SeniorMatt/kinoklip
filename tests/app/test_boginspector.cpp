// SPDX-License-Identifier: GPL-3.0-or-later
// "Bog" nitpicks, Inspector / keyframes / panels part (docs/behavior-spec.md section 49):
// keyframe removal keeps the shown value, the zoom link only changes with its button, easing
// commands, panel shortcuts focusing the search field, keyboard-only ripple delete and effect
// application, and the Region Blur adjustment clip.
#include "../timeline/ParityTestUtils.h"
#include "app/MainWindow.h"
#include "app/pages/EditPage.h"
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Grade.h"
#include "core/KeyframeOps.h"
#include "core/Mask.h"
#include "core/ParamKeys.h"
#include "core/Preferences.h"
#include "ui/common/PanelFrame.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/MultiClipEdit.h"
#include "ui/inspector/ParamAccess.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/viewer/TransformGesture.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;
namespace ko = kk::keyframeops;
namespace kc = kk::keyframecommands;
namespace tg = kk::transformgesture;

namespace {

const QString kPosX = QStringLiteral("transform/posX");
const QString kOpacity = QStringLiteral("transform/opacity");

/// Descriptor of a parameter stored in ClipData::transform (Opacity is listed under Composite).
const ParamDescriptor& transformParam(const QString& key)
{
    for (const char* id : {groups::Transform, groups::Composite}) {
        const EffectDescriptor* group = EffectRegistry::instance().find(QLatin1String(id));
        if (const ParamDescriptor* param = group ? group->param(key) : nullptr)
            return *param;
    }
    qFatal("no descriptor for parameter %s", qPrintable(key));
}

/// A 100-frame clip at frame 50: posX keys 0 (0 px) / 50 (100 px), opacity key 50 (40 %).
struct KeyFixture : Ctl
{
    quint64 id = 0;
    KeyFixture()
    {
        ClipData c = solidClip(50, 100);
        c.transform.animated(keys::PosX, 0).setKey(0, 0.0);
        c.transform.animated(keys::PosX, 0).setKey(50, 100.0);
        c.transform.animated(keys::Opacity, 100).setKey(50, 40.0);
        id = put(kV1, c);
        f.project.undoStack()->clear();
    }
    const ClipData& clip() const { return *ops::clipById(f.view(), id); }
    const AnimatedValue& value(const QString& path) const { return *ko::findValue(clip(), path); }
};

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

} // namespace

class TestBogInspector : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_boginspector"); }
    void init() { QSettings().clear(); }

    // ---- 49.1 Removing keyframes keeps the value ----

    // Deleting every key of a parameter keeps the value it showed at the playhead, whatever
    // order the keys were selected in; one key left keeps interpolating as before.
    void spec49_1_deletingAllKeysKeepsTheShownValue()
    {
        for (const bool reversed : {false, true}) {
            KeyFixture t;
            t.tl()->setPlayhead(50 + 30); // posX shows 60 px
            QVector<KeyframeRef> refs {{t.id, kPosX, 0}, {t.id, kPosX, 50}};
            if (reversed)
                std::reverse(refs.begin(), refs.end());
            t.f.selection.setKeyframes(refs);
            QVERIFY(kc::deleteSelected(t.f.ctx));
            QVERIFY(!t.value(kPosX).isAnimated());
            QVERIFY(near(t.value(kPosX).base, 60.0));
            QCOMPARE(t.undoCount(), 1);
            t.f.project.undoStack()->undo();
            QCOMPARE(t.value(kPosX).keys.size(), 2);
        }
    }

    // The Inspector's diamond on the last key (and the plain AnimatedValue rule behind it)
    // leaves the parameter static at the key's value, never at the default.
    void spec49_1_inspectorDiamondOnTheLastKeyKeepsTheValue()
    {
        KeyFixture t;
        ClipData c = t.clip();
        const ParamRef ref {t.id, ParamScope::Transform, 0, keys::Opacity};
        QCOMPARE(paramaccess::toggleKey(c, ref, transformParam(keys::Opacity), 50), false);
        const AnimatedValue& opacity = *ko::findValue(c, kOpacity);
        QVERIFY(!opacity.isAnimated());
        QVERIFY(near(opacity.base, 40.0)); // not the default 100 %
        // Off a key, the diamond adds one at the shown value instead.
        QCOMPARE(paramaccess::toggleKey(c, ref, transformParam(keys::Opacity), 10), true);
        QVERIFY(near(ko::findValue(c, kOpacity)->valueAt(10), 40.0));
    }

    // ---- 49.2 The zoom link changes only with its button ----

    // A Shift scale of a linked zoom frees that drag only: the stored link is never written,
    // so a multi-clip propagation cannot turn another clip's link on or off.
    void spec49_2_dragsNeverStoreTheLink()
    {
        ClipData shown = solidClip(0, 100);
        shown.id = 1;
        ClipData peer = shown;
        peer.id = 2;
        peer.transform.setStatic(keys::ZoomLinked, false);
        for (const bool shift : {false, true}) {
            const TransformPicture pic = tg::picture(shown, 0, QSizeF(320, 180), nullptr);
            tg::TransformDrag drag;
            drag.begin(shown, 0, pic, {TransformHandle::Kind::Scale, 1, 1}, {320, 180});
            ClipData after = shown;
            drag.update(after.transform, 0, {400, 180}, shift);
            QVERIFY(!after.transform.statics.contains(keys::ZoomLinked));
            ClipData target = peer;
            QVERIFY(multiclip::propagate(shown, after, target));
            QCOMPARE(target.transform.staticValue(keys::ZoomLinked, true).toBool(), false);
        }
    }

    // A linked Inspector edit scales both axes by the same factor: equal axes stay equal, a
    // ratio left by a free Shift scale is kept (no snapping of Y to X); unlinked edits one axis.
    void spec49_2_linkedEditKeepsTheRatio()
    {
        ClipData c = solidClip(0, 100);
        const ParamRef zoomX {c.id, ParamScope::Transform, 0, keys::ZoomX};
        const ParamDescriptor& desc = transformParam(keys::ZoomX);
        paramaccess::setScalar(c, zoomX, desc, 0, 2.0);
        QVERIFY(near(c.transform.value(keys::ZoomY, 0), 2.0));
        c.transform.setBase(keys::ZoomX, 1.5);
        c.transform.setBase(keys::ZoomY, 1.0);
        paramaccess::setScalar(c, zoomX, desc, 0, 3.0);
        QVERIFY(near(c.transform.value(keys::ZoomX, 0), 3.0));
        QVERIFY(near(c.transform.value(keys::ZoomY, 0), 2.0));
        QCOMPARE(c.transform.staticValue(keys::ZoomLinked, true).toBool(), true);
        c.transform.setStatic(keys::ZoomLinked, false);
        paramaccess::setScalar(c, zoomX, desc, 0, 1.0);
        QVERIFY(near(c.transform.value(keys::ZoomY, 0), 2.0));
    }

    // ---- 49.3 Keyframes of different effects together ----

    // The marks on a clip (and its keyframe toggles) count masks, mask shapes and the grade,
    // not only transform, audio, title and effects; the retime curve has its own bar.
    void spec49_3_clipMarksCoverEveryParameter()
    {
        ClipData c = solidClip(0, 100);
        QVERIFY(ko::clipMarkFrames(c).isEmpty());
        c.retime.animated(QStringLiteral("retimeFrame"), 0).setKey(7, 1.0);
        QVERIFY(ko::clipMarkFrames(c).isEmpty());
        c.transform.animated(keys::PosX, 0).setKey(10, 1.0);
        MaskData mask = makeMask(MaskShape::Rectangle, 5, QSizeF(100, 100));
        mask.params.animated(maskkeys::Width, 100).setKey(20, 50.0);
        c.masks.append(mask);
        GradeNode node;
        node.id = 6;
        node.params.animated(gradekeys::allKeys().first(), 0).setKey(30, 1.0);
        c.grade.nodes.append(node);
        QCOMPARE(ko::clipMarkFrames(c), (QVector<int64_t> {10, 20, 30}));
    }

    // ---- 49.4 Easing commands ----

    // Registered without a key in every built-in keymap (none of the editors has a default).
    void spec49_4_easingCommandsHaveNoDefaultKey()
    {
        TimelineFixture f;
        for (const kc::EasingAction& e : kc::easingActions()) {
            QVERIFY2(f.actions.contains(QLatin1String(e.id)), e.id);
            QVERIFY2(f.actions.action(QLatin1String(e.id))->shortcuts().isEmpty(), e.id);
        }
        for (const char* preset : {"resolve-windows", "premiere-pro", "final-cut-pro",
                 "avid-media-composer", "kdenlive", "shotcut", "vegas-pro"}) {
            QFile file(QStringLiteral(":/keymaps/%1.json").arg(QLatin1String(preset)));
            QVERIFY2(file.open(QIODevice::ReadOnly), preset);
            const QJsonObject keys
                = QJsonDocument::fromJson(file.readAll()).object().value("shortcuts").toObject();
            for (const kc::EasingAction& e : kc::easingActions()) {
                QVERIFY2(keys.contains(QLatin1String(e.id)), e.id);
                QVERIFY2(keys.value(QLatin1String(e.id)).toArray().isEmpty(), e.id);
            }
        }
    }

    // The commands ease the selected keys, else every key at the playhead of the selected
    // clips; one undo step each, however many editors bound them.
    void spec49_4_easingCommandsActOnSelectionOrPlayhead()
    {
        KeyFixture t;
        kc::bindEasingActions(t.f.ctx);
        kc::bindEasingActions(t.f.ctx); // a second editor: still one edit per command
        t.f.selection.setKeyframes({{t.id, kPosX, 0}});
        t.action("clip.keyframe_ease_in")->trigger();
        QCOMPARE(keyInterp(t.value(kPosX).keys[0]), Interp::EaseIn);
        QCOMPARE(keyInterp(t.value(kPosX).keys[1]), Interp::Linear);
        QCOMPARE(t.undoCount(), 1);
        // No keys selected: the keys at the playhead (posX and opacity at clip frame 50).
        t.f.selection.setKeyframes({});
        t.f.selection.setClips({t.id});
        t.tl()->setPlayhead(50 + 50);
        t.action("clip.keyframe_ease_in_out")->trigger();
        QCOMPARE(keyInterp(t.value(kPosX).keys[1]), Interp::EaseInOut);
        QCOMPARE(keyInterp(t.value(kOpacity).keys[0]), Interp::EaseInOut);
        QCOMPARE(keyInterp(t.value(kPosX).keys[0]), Interp::EaseIn); // other frame untouched
        QCOMPARE(t.undoCount(), 2);
        t.action("clip.keyframe_linear")->trigger();
        QCOMPARE(keyInterp(t.value(kOpacity).keys[0]), Interp::Linear);
        t.action("clip.keyframe_ease_out")->trigger();
        QCOMPARE(keyInterp(t.value(kOpacity).keys[0]), Interp::EaseOut);
        // Off a key with nothing selected: nothing to do, no undo step.
        t.tl()->setPlayhead(50 + 20);
        const int before = t.undoCount();
        t.action("clip.keyframe_ease_in")->trigger();
        QCOMPARE(t.undoCount(), before);
    }

    // ---- 49.5 Panel shortcuts and the search field ----

    // Off by default (Resolve): the panel opens, the focus stays. With the option, opening it
    // by its shortcut focuses its search field; opening it by the interface bar does not.
    void spec49_5_panelShortcutFocusesTheSearchField()
    {
        TimelineFixture f;
        MainWindowOptions options;
        options.autosave = false;
        auto window = std::make_unique<MainWindow>(f.ctx, options);
        QCursor::setPos(-5000, -5000);
        window->resize(1600, 1000);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));
        EditPage* page = window->editPage();
        QAction* toggle = f.actions.action(QStringLiteral("view.toggle_effects"));
        toggle->setShortcut(QKeySequence(Qt::Key_F13));
        QLineEdit* search = page->effectsLibrary()->searchField();
        QVERIFY(search);
        QVERIFY(page->mediaPool()->searchField());
        QVERIFY(page->editIndex()->searchField());

        auto reopen = [&](bool byKey) {
            if (page->isPanelVisible(PanelId::Effects)) {
                toggle->trigger(); // hide
                QVERIFY(!page->isPanelVisible(PanelId::Effects));
            }
            window->setFocus();
            QVERIFY(!search->hasFocus());
            if (byKey)
                QTest::keyClick(window.get(), Qt::Key_F13);
            else
                toggle->trigger();
            QVERIFY(page->isPanelVisible(PanelId::Effects));
            QCoreApplication::processEvents();
        };
        QVERIFY(!prefs::focusSearchOnPanelKey());
        reopen(true);
        QVERIFY(!search->hasFocus());
        prefs::setFocusSearchOnPanelKey(true);
        reopen(true);
        QTRY_VERIFY(search->hasFocus());
        reopen(false);
        QVERIFY(!search->hasFocus());
    }

    // ---- 49.6 Keyboard-only flows ----

    // Ripple delete with nothing selected does nothing (Resolve); with the option it removes
    // the clips under the playhead on the auto-select tracks and closes up, one undo step.
    void spec49_6_rippleDeleteClipAtPlayheadOption()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(10, 10));
        t.put(kV1, solidClip(20, 10));
        t.put(kA1, solidClip(0, 10));
        t.put(kA1, solidClip(10, 10));
        t.tl()->setPlayhead(15);
        QVERIFY(!EditBehaviour::fromSettings().rippleDeleteClipAtPlayhead);
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10 20:10"));
        QSettings().setValue(QStringLiteral("edit/rippleDeleteClipAtPlayhead"), true);
        const int before = t.undoCount();
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("0:10"));
        QCOMPARE(t.undoCount(), before + 1);
        // A track with its auto select off keeps its clip (and keeps sync: nothing moves).
        t.f.project.undoStack()->undo();
        t.f.project.beginEdit(QStringLiteral("auto select off"));
        t.f.data().audioTracks[0].autoSelect = false;
        t.f.project.endEdit();
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("0:10 10:10"));
    }

    // Enter on an entry of the Effects library applies it like a double-click: an effect goes
    // onto the selected clips.
    void spec49_6_enterAppliesTheEffectsLibraryEntry()
    {
        Ctl t;
        const quint64 id = t.put(kV1, solidClip(0, 50));
        t.f.selection.setClips({id});
        EffectsLibraryPanel panel(t.f.ctx);
        panel.resize(500, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QTreeWidget* tree = panel.tree();
        QTreeWidgetItem* blur = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it && !blur; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QStringLiteral("gaussian_blur")
                || (*it)->text(0) == QStringLiteral("Gaussian Blur"))
                blur = *it;
        }
        QVERIFY(blur);
        tree->setCurrentItem(blur);
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Return);
        const ClipData* c = ops::clipById(t.f.view(), id);
        QCOMPARE(c->effects.size(), 1);
        QCOMPARE(c->effects[0].type, QStringLiteral("gaussian_blur"));
    }

    // ---- 49.8 Region Blur ----

    // Effects > Region Blur places an adjustment clip with a Gaussian Blur and a centred
    // rectangle mask (fresh ids, a third of the frame) in one undo step.
    void spec49_8_regionBlurIsAnAdjustmentClipWithBlurAndMask()
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(generators::RegionBlur);
        QVERIFY(d && d->category == EffectCategory::Effects);
        Ctl t;
        t.put(kV1, solidClip(0, 200));
        const int before = t.undoCount();
        const quint64 id = effectactions::addGeneratorClip(t.f.project, generators::RegionBlur, 10);
        QVERIFY(id);
        QCOMPARE(t.undoCount(), before + 1);
        const ClipData* c = ops::clipById(t.f.view(), id);
        QCOMPARE(c->kind, ClipKind::Adjustment);
        QCOMPARE(c->effects.size(), 1);
        QCOMPARE(c->effects[0].type, QStringLiteral("gaussian_blur"));
        QVERIFY(c->effects[0].params.value(QStringLiteral("radius"), 0, 0) > 20.0);
        QCOMPARE(c->masks.size(), 1);
        QCOMPARE(c->masks[0].shape, MaskShape::Rectangle);
        QVERIFY(c->effects[0].id != 0 && c->masks[0].id != 0);
        QVERIFY(c->effects[0].id != c->masks[0].id && c->masks[0].id != id);
        QVERIFY(near(c->masks[0].value(maskkeys::Width, 0), t.f.view().width / 3.0));
        // A second one gets ids of its own.
        const quint64 second
            = effectactions::addGeneratorClip(t.f.project, generators::RegionBlur, 10);
        const ClipData* c2 = ops::clipById(t.f.view(), second);
        QVERIFY(c2->effects[0].id != ops::clipById(t.f.view(), id)->effects[0].id);
    }
};

QTEST_MAIN(TestBogInspector)
#include "test_boginspector.moc"
