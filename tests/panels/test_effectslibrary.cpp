// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel + EffectActions: sections, search, drag payload, activation semantics.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"

#include <QApplication>
#include <QMimeData>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

class TestEffectsLibrary : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    void sectionsAndSearch()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        panel.resize(320, 600);
        QStringList sections;
        for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i)
            sections.append(panel.tree()->topLevelItem(i)->text(0));
        QCOMPARE(sections.mid(0, 6),
            QStringList({QStringLiteral("Video Transitions"), QStringLiteral("Audio Transitions"),
                QStringLiteral("Titles"), QStringLiteral("Generators"), QStringLiteral("Effects"),
                QStringLiteral("Video Effects")}));
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("gaussian_blur")));
        saveScreenshot(panel, QStringLiteral("effects_library"));

        panel.setFilterText(QStringLiteral("gaussian"));
        QCOMPARE(panel.visibleDescriptorIds(), QStringList {QStringLiteral("gaussian_blur")});
        panel.setFilterText(QStringLiteral("DISSOLVE"));
        // Four in the Dissolve folder and Noise Dissolve have "dissolve" in the name (53.1); the
        // old expectation of one predates the folders.
        QStringList dissolves = panel.visibleDescriptorIds();
        dissolves.sort();
        QCOMPARE(dissolves,
            QStringList({QStringLiteral("additive_dissolve"), QStringLiteral("blur_dissolve"),
                QStringLiteral("cross_dissolve"), QStringLiteral("noise_dissolve"),
                QStringLiteral("non_additive_dissolve")}));
        panel.setFilterText(QStringLiteral("nothing-matches"));
        QVERIFY(panel.visibleDescriptorIds().isEmpty());
        panel.setFilterText({});
        QVERIFY(panel.visibleDescriptorIds().size() > 10);

        // The sidebar category narrows the list; a search still looks everywhere.
        panel.setCategoryFilter(int(EffectCategory::Transition));
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("cross_dissolve")));
        QVERIFY(!panel.visibleDescriptorIds().contains(QStringLiteral("gaussian_blur")));
        panel.setFilterText(QStringLiteral("gaussian"));
        QCOMPARE(panel.visibleDescriptorIds(), QStringList {QStringLiteral("gaussian_blur")});
        panel.setFilterText({});
        panel.setCategoryFilter(-1);
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("gaussian_blur")));
    }

    void spec53_1_transitionFolders()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        const QTreeWidgetItem* section = panel.tree()->topLevelItem(0);
        QCOMPARE(section->text(0), QStringLiteral("Video Transitions"));
        QStringList folders;
        for (int i = 0; i < section->childCount(); ++i)
            folders.append(section->child(i)->text(0));
        QCOMPARE(folders,
            QStringList({QStringLiteral("Dissolve"), QStringLiteral("Fusion Transitions"),
                QStringLiteral("Iris"), QStringLiteral("Motion"),
                QStringLiteral("Resolve FX Transitions"), QStringLiteral("Shape"),
                QStringLiteral("Wipe")}));
        auto names = [&](int folder) {
            QStringList n;
            for (int i = 0; i < section->child(folder)->childCount(); ++i)
                n.append(section->child(folder)->child(i)->text(0));
            return n;
        };
        QVERIFY(names(0).contains(QStringLiteral("Smooth Cut")));
        QVERIFY(names(0).contains(QStringLiteral("Cross Dissolve")));
        QCOMPARE(names(1).size(), 15);
        QCOMPARE(names(2).size(), 9);
        QCOMPARE(names(3).size(), 4);
        QCOMPARE(names(4).size(), 1);
        QCOMPARE(names(5).size(), 5);
        QCOMPARE(names(6).size(), 8);
        panel.setFilterText(QStringLiteral("smooth"));
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("smooth_cut")));
    }

    void spec13_11_audioTransitionsSection()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        QTreeWidgetItem* section = nullptr;
        for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i) {
            if (panel.tree()->topLevelItem(i)->text(0) == QStringLiteral("Audio Transitions"))
                section = panel.tree()->topLevelItem(i);
        }
        QVERIFY(section);
        QStringList names;
        for (int i = 0; i < section->childCount(); ++i) {
            names.append(section->child(i)->text(0));
            QVERIFY(!section->child(i)->icon(0).isNull());
        }
        QCOMPARE(names,
            QStringList({QStringLiteral("Cross Fade 0 dB"), QStringLiteral("Cross Fade +3 dB"),
                QStringLiteral("Cross Fade -3 dB"), QStringLiteral("Exponential"),
                QStringLiteral("Logarithmic"), QStringLiteral("S-Curve")}));
        // The sidebar entry shows just those six.
        panel.setCategoryFilter(int(EffectCategory::AudioTransition));
        QCOMPARE(panel.visibleDescriptorIds().size(), 6);
        QVERIFY(panel.visibleDescriptorIds().contains(QStringLiteral("audio_crossfade_3db")));
    }

    void spec13_15_audioTransitionActivation()
    {
        // Double-click with a linked video + audio pair selected: the audio clip gets it.
        PanelFixture f;
        const quint64 v = f.addClip({TrackType::Video, 0}, 100, 100);
        const quint64 a = f.addClip({TrackType::Audio, 0}, 100, 100);
        f.selection.setClips({a, v});
        f.project.timeline()->setPlayhead(105);
        QVERIFY(effectactions::activate(f.ctx, QStringLiteral("audio_crossfade_scurve")));
        QVERIFY(!f.clip(v)->transitionIn);
        QVERIFY(f.clip(a)->transitionIn);
        QCOMPARE(f.clip(a)->transitionIn->type, QStringLiteral("audio_crossfade_scurve"));
        // Video transitions never go onto audio clips, nor audio ones onto video clips.
        QVERIFY(!effectactions::applyTransition(f.project, a, QStringLiteral("wipe"), false));
        QVERIFY(!effectactions::applyTransition(
            f.project, v, QStringLiteral("audio_crossfade_0db"), false));
        QCOMPARE(effectactions::defaultTransitionId(TrackType::Audio),
            QStringLiteral("audio_crossfade_3db"));
        QCOMPARE(
            effectactions::defaultTransitionId(TrackType::Video), QStringLiteral("cross_dissolve"));
    }

    void dragPayloadIsDescriptorId()
    {
        PanelFixture f;
        EffectsLibraryPanel panel(f.ctx);
        QTreeWidget* tree = panel.tree();
        QTreeWidgetItem* blur = nullptr;
        for (QTreeWidgetItem* item :
            tree->findItems(QStringLiteral("Gaussian Blur"), Qt::MatchExactly | Qt::MatchRecursive))
            blur = item;
        QVERIFY(blur);
        const QModelIndex index = tree->indexFromItem(blur);
        std::unique_ptr<QMimeData> data(tree->model()->mimeData({index}));
        QVERIFY(data);
        QCOMPARE(QString::fromUtf8(data->data(QLatin1String(mime::Effect))),
            QStringLiteral("gaussian_blur"));
    }

    void activateEffectOnSelection()
    {
        PanelFixture f;
        const quint64 v = f.addClip({TrackType::Video, 0}, 0, 50);
        const quint64 a = f.addClip({TrackType::Audio, 0}, 0, 50);
        f.selection.setClips({v, a});
        const int commands = f.project.undoStack()->count();
        QVERIFY(effectactions::activate(f.ctx, QStringLiteral("vignette")));
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
        QCOMPARE(f.clip(v)->effects.size(), 1);
        QCOMPARE(f.clip(a)->effects.size(), 0); // video effect does not fit an audio clip
        // Nothing selected: no-op, no undo step.
        f.selection.clear();
        QVERIFY(!effectactions::activate(f.ctx, QStringLiteral("vignette")));
        QCOMPARE(f.project.undoStack()->count(), commands + 1);
    }

    void activateTransitionUsesNearerEnd()
    {
        PanelFixture f;
        const quint64 v = f.addClip({TrackType::Video, 0}, 100, 100);
        f.selection.selectClip(v);
        f.project.timeline()->setPlayhead(190);
        QVERIFY(effectactions::activate(f.ctx, QStringLiteral("cross_dissolve")));
        QVERIFY(!f.clip(v)->transitionIn);
        QVERIFY(f.clip(v)->transitionOut);
        QCOMPARE(f.clip(v)->transitionOut->duration, int64_t(25));
        // Retyping keeps the length.
        QVERIFY(effectactions::applyTransition(f.project, v, QStringLiteral("wipe"), false, -1));
        QCOMPARE(f.clip(v)->transitionOut->type, QStringLiteral("wipe"));
        QCOMPARE(f.clip(v)->transitionOut->duration, int64_t(25));
        QVERIFY(effectactions::removeTransition(f.project, v, false));
        QVERIFY(!f.clip(v)->transitionOut);
    }

    void titleIsPlacedOnTop()
    {
        PanelFixture f;
        f.addClip({TrackType::Video, 0}, 0, 200);
        f.addClip({TrackType::Video, 1}, 50, 20);
        f.project.timeline()->setPlayhead(40);
        QVERIFY(effectactions::activate(f.ctx, QStringLiteral("text_title")));
        const quint64 title = f.selection.currentClip();
        const auto location = editops::findClip(f.project.timeline()->data(), title);
        QVERIFY(location);
        // V2 is busy in [40, 165) -> a new V3 is created.
        QCOMPARE(location->track.index, 2);
        QCOMPARE(f.clip(title)->kind, ClipKind::Title);
        QCOMPARE(f.clip(title)->start, int64_t(40));
        QCOMPARE(f.clip(title)->duration, int64_t(125));

        // Empty timeline region: lands on V1.
        f.project.timeline()->setPlayhead(1000);
        QVERIFY(effectactions::activate(f.ctx, QStringLiteral("solid_color")));
        QCOMPARE(
            editops::findClip(f.project.timeline()->data(), f.selection.currentClip())->track.index,
            0);
    }
};

QTEST_MAIN(TestEffectsLibrary)
#include "test_effectslibrary.moc"
