// SPDX-License-Identifier: GPL-3.0-or-later
// The Effects library's "Set as Standard Transition" and the project's standard transition.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "ui/common/Resources.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QSpinBox>
#include <QMenu>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

class TestStdDefaultsUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    static QAction* standardAction(QMenu* menu)
    {
        return menu ? menu->findChild<QAction*>(QStringLiteral("SetStandardTransition")) : nullptr;
    }

    static bool isBold(EffectsLibraryPanel& panel, const QString& name)
    {
        const auto items = panel.tree()->findItems(name, Qt::MatchRecursive | Qt::MatchExactly);
        for (QTreeWidgetItem* item : items) {
            if (!item->data(0, Qt::UserRole).toString().isEmpty()) // not a folder
                return item->font(0).bold();
        }
        return false;
    }

    void setAsStandardTransition()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        QVERIFY(isBold(panel, QStringLiteral("Cross Dissolve")));
        QVERIFY(isBold(panel, QStringLiteral("Cross Fade +3 dB")));
        QVERIFY(!isBold(panel, QStringLiteral("Wipe")));

        std::unique_ptr<QMenu> menu(panel.createContextMenu(QStringLiteral("wipe")));
        QAction* set = standardAction(menu.get());
        QVERIFY(set);
        QVERIFY(set->isEnabled());
        const int steps = f.project.undoStack()->count();
        set->trigger();
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.standardTransitionId(false), QStringLiteral("wipe"));
        QVERIFY(isBold(panel, QStringLiteral("Wipe")));
        QVERIFY(!isBold(panel, QStringLiteral("Cross Dissolve")));

        // Audio transitions have it too and set the audio one.
        menu.reset(panel.createContextMenu(QStringLiteral("audio_crossfade_scurve")));
        QVERIFY(standardAction(menu.get()));
        standardAction(menu.get())->trigger();
        QCOMPARE(f.project.standardTransitionId(true), QStringLiteral("audio_crossfade_scurve"));
        QCOMPARE(f.project.standardTransitionId(false), QStringLiteral("wipe"));
        // Effects have no such entry.
        menu.reset(panel.createContextMenu(QStringLiteral("gaussian_blur")));
        QVERIFY(!standardAction(menu.get()));

        f.project.undo();
        f.project.undo();
        QVERIFY(isBold(panel, QStringLiteral("Cross Dissolve")));
    }

    void projectSettingsDialogEditsThem()
    {
        PanelFixture f;
        ProjectSettingsDialog dialog(&f.project);
        auto* video = dialog.findChild<QComboBox*>(QStringLiteral("StandardVideoTransition"));
        auto* audio = dialog.findChild<QComboBox*>(QStringLiteral("StandardAudioTransition"));
        auto* frames = dialog.findChild<QSpinBox*>(QStringLiteral("StandardTransitionFrames"));
        QVERIFY(video && audio && frames);
        QCOMPARE(video->currentData().toString(), QStringLiteral("cross_dissolve"));
        QCOMPARE(audio->currentData().toString(), QStringLiteral("audio_crossfade_3db"));
        video->setCurrentIndex(video->findData(QStringLiteral("wipe")));
        audio->setCurrentIndex(audio->findData(QStringLiteral("audio_crossfade_0db")));
        frames->setValue(31);
        const int steps = f.project.undoStack()->count();
        dialog.apply();
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.standardTransitionId(false), QStringLiteral("wipe"));
        QCOMPARE(f.project.standardTransitionId(true), QStringLiteral("audio_crossfade_0db"));
        QCOMPARE(f.project.standardTransitionFrames(), int64_t(31));
        f.project.undo();
        QCOMPARE(f.project.standardTransitionId(false), QStringLiteral("cross_dissolve"));
    }

    void defaultTransitionFollowsTheProject()
    {
        PanelFixture f;
        const quint64 v = f.addClip({TrackType::Video, 0}, 0, 100);
        const quint64 a = f.addClip({TrackType::Audio, 0}, 0, 100);
        ProjectSettings s = f.project.settings();
        s.stdVideoTransition = QStringLiteral("wipe");
        s.stdAudioTransition = QStringLiteral("audio_crossfade_scurve");
        s.stdTransitionFrames = 17;
        f.project.setSettings(s);
        QCOMPARE(effectactions::defaultTransitionId(f.project, TrackType::Video),
            QStringLiteral("wipe"));
        QVERIFY(effectactions::applyTransition(f.project, v, QStringLiteral("wipe"), true));
        QCOMPARE(f.clip(v)->transitionIn->duration, int64_t(17));
        QVERIFY(effectactions::applyTransition(f.project, a,
            effectactions::defaultTransitionId(f.project, TrackType::Audio), false));
        QCOMPARE(f.clip(a)->transitionOut->type, QStringLiteral("audio_crossfade_scurve"));
    }
};

QTEST_MAIN(TestStdDefaultsUi)
#include "test_stddefaults_ui.moc"
