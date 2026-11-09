// SPDX-License-Identifier: GPL-3.0-or-later
// Clip audio effects on the Edit page (docs/behavior-spec.md section 51): the Inspector's Add
// Effect on audio clips and on the audio part of a linked pair, applying / undoing, editing,
// bypassing, reordering and removing, the Effects library and timeline drops, paste of the
// stack, and saving / loading.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "core/Timeline.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};
const TrackRef kA1 {TrackType::Audio, 0};

QVector<QToolButton*> addEffectButtons(InspectorPanel& panel)
{
    QVector<QToolButton*> found;
    for (QToolButton* b : panel.findChildren<QToolButton*>()) {
        if (b->text() == QLatin1String("Add Effect"))
            found.append(b);
    }
    return found;
}

QStringList menuTexts(QToolButton* button)
{
    QStringList texts;
    for (QAction* a : button->menu()->actions())
        texts.append(a->text());
    return texts;
}

ParamGroupBox* groupTitled(InspectorPanel& panel, const QString& title)
{
    for (ParamGroupBox* g : panel.groups()) {
        if (g->findChild<QLabel*>(QStringLiteral("InspectorGroupTitle"))->text() == title)
            return g;
    }
    return nullptr;
}

QToolButton* headerButton(ParamGroupBox* g, const QString& tip)
{
    for (QToolButton* b : g->findChildren<QToolButton*>()) {
        if (b->toolTip() == tip)
            return b;
    }
    return nullptr;
}

void link(PanelFixture& f, quint64 a, quint64 b)
{
    f.project.beginEdit(QStringLiteral("link"));
    editops::linkClips(f.project.timeline()->mutableData(), {a, b});
    f.project.endEdit();
}

} // namespace

class TestAudioClipEffectsUi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    // Spec 52.1: the Inspector's Add Effect is enabled for an audio clip and lists the audio
    // effects only.
    void inspectorEnablesAddEffectForAnAudioClip()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(a);
        const auto buttons = addEffectButtons(panel);
        QCOMPARE(buttons.size(), 1);
        QVERIFY(buttons[0]->isEnabled());
        QStringList expected;
        for (const EffectDescriptor* d :
            EffectRegistry::instance().byCategory(EffectCategory::AudioEffect))
            expected.append(d->name);
        QCOMPARE(menuTexts(buttons[0]), expected);
        QVERIFY(expected.size() >= 8);
        // A video clip still lists video effects only.
        const quint64 v = f.addClip(kV1, 0, 50, ClipKind::Solid);
        f.selection.selectClip(v);
        const auto videoButtons = addEffectButtons(panel);
        QCOMPARE(videoButtons.size(), 1);
        QVERIFY(!menuTexts(videoButtons[0]).contains(QStringLiteral("Gain")));
    }

    // Spec 52.2: a linked video + audio pair shows the video stack and the audio stack, each
    // with its own Add Effect, and the audio effect goes onto the audio clip.
    void linkedPairHasSeparateVideoAndAudioStacks()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        link(f, v, a);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(v);
        const auto buttons = addEffectButtons(panel);
        QCOMPARE(buttons.size(), 2);
        QVERIFY(buttons[0]->isEnabled() && buttons[1]->isEnabled());
        QVERIFY(!menuTexts(buttons[0]).contains(QStringLiteral("Gain")));
        QVERIFY(menuTexts(buttons[1]).contains(QStringLiteral("Gain")));
        // Pick Gain in the audio menu.
        for (QAction* act : buttons[1]->menu()->actions()) {
            if (act->text() == QLatin1String("Gain"))
                act->trigger();
        }
        QCOMPARE(f.clip(a)->effects.size(), 1);
        QVERIFY(f.clip(v)->effects.isEmpty());
        QVERIFY(groupTitled(panel, QStringLiteral("Gain")));
    }

    // Spec 52.3: applying is one undo step, undo / redo restore it; a video effect is refused
    // by an audio clip and an audio effect by a video clip.
    void applyUndoRedoAndRefusals()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        const quint64 v = f.addClip(kV1, 0, 50);
        const int commands = f.project.undoStack()->count();
        QCOMPARE(effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Compressor)), 1);
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QCOMPARE(f.clip(a)->effects.size(), 1);
        QCOMPARE(f.clip(a)->effects[0].type, QLatin1String(afxid::Compressor));
        QVERIFY(f.clip(a)->effects[0].enabled);
        f.project.undo();
        QVERIFY(f.clip(a)->effects.isEmpty());
        f.project.redo();
        QCOMPARE(f.clip(a)->effects.size(), 1);
        QCOMPARE(effectactions::applyEffect(f.project, {a}, QStringLiteral("gaussian_blur")), 0);
        QCOMPARE(effectactions::applyEffect(f.project, {v}, QLatin1String(afxid::Gain)), 0);
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
    }

    // Spec 52.3: Add Effect in the Effects library (double-click) puts the audio effect on the
    // audio part of a selected linked video clip too.
    void libraryActivateReachesTheLinkedAudio()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        link(f, v, a);
        f.selection.selectClip(v);
        QVERIFY(effectactions::activate(f.ctx, QLatin1String(afxid::Reverb)));
        QCOMPARE(f.clip(a)->effects.size(), 1);
        QVERIFY(f.clip(v)->effects.isEmpty());
        // The library lists the audio effects in their own section.
        EffectsLibraryPanel library(f.ctx);
        QStringList ids = library.visibleDescriptorIds();
        for (const EffectDescriptor* d :
            EffectRegistry::instance().byCategory(EffectCategory::AudioEffect))
            QCOMPARE(ids.count(d->id), 1);
    }

    // Spec 52.3: parameters edit in the Inspector, bypass, reorder and remove, all undoable.
    void inspectorEditsBypassesReordersAndRemoves()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Gain));
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Limiter));
        InspectorPanel panel(f.ctx);
        panel.resize(420, 800);
        panel.show();
        f.selection.selectClip(a);

        ParamGroupBox* gain = groupTitled(panel, QStringLiteral("Gain"));
        ParamGroupBox* limiter = groupTitled(panel, QStringLiteral("Limiter"));
        QVERIFY(gain && limiter);
        // Edit: drag the Gain value field (as the timeline tests drag a value).
        auto* field = gain->findChild<ValueField*>(QStringLiteral("value:") + afxkeys::GainDb);
        QVERIFY(field);
        const int commands = f.project.undoStack()->count();
        const auto send = [&](QEvent::Type type, int x, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(type, QPointF(x, field->height() / 2), field->mapToGlobal(QPointF(x, 0)),
                b, bs, Qt::NoModifier);
            QApplication::sendEvent(field, &e);
        };
        send(QEvent::MouseButtonPress, 10, Qt::LeftButton, Qt::LeftButton);
        for (int x : {12, 14, 24, 34})
            send(QEvent::MouseMove, x, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, 34, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        const double edited = f.clip(a)->effects[0].params.value(afxkeys::GainDb, 0, 0.0);
        QVERIFY(edited > 0.0);
        f.project.undo();
        QCOMPARE(f.clip(a)->effects[0].params.value(afxkeys::GainDb, 0, 99.0), 0.0);
        f.project.redo();

        // Bypass.
        QCheckBox* enable = gain->findChild<QCheckBox*>();
        QVERIFY(enable && enable->isChecked());
        enable->click();
        QVERIFY(!f.clip(a)->effects[0].enabled);
        f.project.undo();
        QVERIFY(f.clip(a)->effects[0].enabled);

        // Reorder.
        headerButton(limiter, QStringLiteral("Move up"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(a)->effects[0].type, QLatin1String(afxid::Limiter));
        f.project.undo();
        QCOMPARE(f.clip(a)->effects[0].type, QLatin1String(afxid::Gain));

        // Remove.
        gain = groupTitled(panel, QStringLiteral("Gain"));
        headerButton(gain, QStringLiteral("Remove effect"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(a)->effects.size(), 1);
        f.project.undo();
        QCOMPARE(f.clip(a)->effects.size(), 2);
    }

    // Spec 52.3: the audio effects of a linked pair edit through the pair's one Inspector
    // (the audio stack's buttons act on the audio clip).
    void linkedPairAudioStackEdits()
    {
        PanelFixture f;
        const quint64 v = f.addClip(kV1, 0, 50);
        const quint64 a = f.addClip(kA1, 0, 50);
        link(f, v, a);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Gain));
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Delay));
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(v);
        ParamGroupBox* delay = groupTitled(panel, QStringLiteral("Delay"));
        QVERIFY(delay);
        headerButton(delay, QStringLiteral("Move up"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(a)->effects[0].type, QLatin1String(afxid::Delay));
        QVERIFY(f.clip(v)->effects.isEmpty());
        delay = groupTitled(panel, QStringLiteral("Delay"));
        headerButton(delay, QStringLiteral("Remove effect"))->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.clip(a)->effects.size(), 1);
    }

    // Spec 52.5: the stack survives saving and loading a project.
    void stackSurvivesSaveAndLoad()
    {
        PanelFixture f;
        const quint64 a = f.addClip(kA1, 0, 50);
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Eq));
        effectactions::applyEffect(f.project, {a}, QLatin1String(afxid::Delay));
        f.project.beginEdit(QStringLiteral("edit"));
        ClipData* c = editops::clipById(f.project.timeline()->mutableData(), a);
        c->effects[0].params.setBase(afxkeys::LowGainDb, 7.5);
        c->effects[1].enabled = false;
        c->effects[1].params.setBase(afxkeys::TimeMs, 321.0);
        f.project.endEdit();
        const QVector<EffectInstance> before = f.clip(a)->effects;

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("fx.kkproj"));
        QVERIFY(f.project.save(path));
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData* back = editops::clipById(loaded.timeline()->data(), a);
        QVERIFY(back);
        QCOMPARE(back->effects.size(), 2);
        QVERIFY(back->effects == before);
        QCOMPARE(back->effects[0].params.value(afxkeys::LowGainDb, 0, 0.0), 7.5);
        QVERIFY(!back->effects[1].enabled);
    }
};

QTEST_MAIN(TestAudioClipEffectsUi)
#include "test_audioclipeffectsui.moc"
