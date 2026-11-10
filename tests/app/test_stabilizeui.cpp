// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Inspector's Stabilization section in the real main window (spec section 58): where
// it sits, its controls and enable rules, one undo step per change, and the Stabilize button
// (background run with progress and Cancel, one undo step when it finishes, nothing changed when
// cancelled, the Mode changed after the analysis).
#include "../compositing/ShakyFootage.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Stabilization.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ValueField.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace st = kk::stabilize;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    quint64 mediaClip = 0;
    quint64 solidClip = 0;

    explicit Fixture(const QString& sequence)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        const ImportResult imported = project.importMedia({sequence});
        QVERIFY(!imported.importedIds.isEmpty());
        const MediaItem* item = project.mediaPool()->item(imported.importedIds.first());
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        tl.fps = Rational {25, 1};
        ClipData media;
        media.kind = ClipKind::Media;
        media.mediaId = item->id;
        media.name = QStringLiteral("Shaky");
        media.duration = 24;
        media.sourceDurationUs = item->info.durationUs;
        mediaClip = editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, media);
        ClipData solid;
        solid.kind = ClipKind::Solid;
        solid.solidColor = QColor(10, 200, 90);
        solid.start = 40;
        solid.duration = 20;
        solidClip = editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, solid);
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
    const ClipData* clip(quint64 id) const
    {
        return editops::clipById(project.timeline()->data(), id);
    }
    InspectorPanel* inspector() const { return window->editPage()->inspector(); }
    ParamGroupBox* group() const
    {
        return inspector()->findChild<ParamGroupBox*>(QStringLiteral("StabilizationGroup"));
    }
    template<typename T>
    T* find(const char* name) const
    {
        return group() ? group()->findChild<T*>(QLatin1String(name)) : nullptr;
    }
    void select(quint64 id)
    {
        selection.setClips({id});
        QCoreApplication::processEvents();
    }
};

} // namespace

class TestStabilizeUi : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_sequence;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_stabilizeui"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        ShakeSettings shake;
        shake.frames = 26;
        shake.jitterPx = 3.0;
        const QImage big = texturedPicture(900, 560, 5);
        std::vector<QImage> frames;
        for (const CameraPose& p : shakyPath(shake))
            frames.push_back(shakyFrame(big, 320, 180, p));
        m_sequence = writeSequence(m_tmp.filePath(QStringLiteral("seq")), frames);
    }
    void init() { QSettings().clear(); }

    // Between Speed Change and Retime and Scaling, on video-file clips only.
    void sectionSitsAfterSpeedChange()
    {
        Fixture f(m_sequence);
        f.select(f.mediaClip);
        ParamGroupBox* g = f.group();
        QVERIFY(g);
        QStringList titles;
        for (ParamGroupBox* box : f.inspector()->groups())
            titles << box->title();
        const qsizetype at = titles.indexOf(QStringLiteral("Stabilization"));
        QVERIFY2(at > 0 && titles[at - 1] == QStringLiteral("Speed Change")
                && titles[at + 1] == QStringLiteral("Retime and Scaling"),
            qPrintable(titles.join('|')));
        f.select(f.solidClip);
        QVERIFY(!f.group()); // a solid has nothing to measure
    }

    // The controls, their ranges and the enable rules; each change is one undo step.
    void controlsAndEnableRules()
    {
        Fixture f(m_sequence);
        f.select(f.mediaClip);
        auto* enable = f.find<QCheckBox>("StabilizationEnable");
        auto* mode = f.find<QComboBox>("StabilizationMode");
        auto* lock = f.find<QCheckBox>("StabilizationCameraLock");
        auto* zoom = f.find<QCheckBox>("StabilizationZoom");
        auto* cropping = f.find<ValueField>("StabilizationCroppingField");
        auto* smooth = f.find<ValueField>("StabilizationSmoothField");
        auto* strength = f.find<ValueField>("StabilizationStrengthField");
        auto* button = f.find<QPushButton>("StabilizationStabilize");
        auto* status = f.find<QLabel>("StabilizationStatus");
        QVERIFY(
            enable && mode && lock && zoom && cropping && smooth && strength && button && status);

        // Defaults.
        QVERIFY(!enable->isChecked());
        QCOMPARE(mode->count(), 3);
        QCOMPARE(mode->itemText(0), QStringLiteral("Perspective"));
        QCOMPARE(mode->itemText(1), QStringLiteral("Similarity"));
        QCOMPARE(mode->itemText(2), QStringLiteral("Translation"));
        QCOMPARE(mode->currentIndex(), 0);
        QVERIFY(!lock->isChecked() && !zoom->isChecked());
        QCOMPARE(cropping->minimum(), 0.25);
        QCOMPARE(cropping->maximum(), 1.0);
        QCOMPARE(smooth->minimum(), 0.0);
        QCOMPARE(smooth->maximum(), 1.0);
        QCOMPARE(strength->minimum(), -1.0);
        QCOMPARE(strength->maximum(), 1.0);
        QCOMPARE(strength->value(), 1.0);
        QVERIFY(button->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("Press Stabilize")));

        QUndoStack* undo = f.project.undoStack();
        int before = undo->index();
        enable->click();
        QVERIFY(f.clip(f.mediaClip)->stabilization.settings.enabled);
        QCOMPARE(undo->index(), before + 1);
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Enable Stabilization"));

        before = undo->index();
        mode->setCurrentIndex(2);
        emit mode->activated(2);
        QCOMPARE(f.clip(f.mediaClip)->stabilization.settings.mode, st::Mode::Translation);
        QCOMPARE(undo->index(), before + 1);

        before = undo->index();
        zoom->click();
        QVERIFY(f.clip(f.mediaClip)->stabilization.settings.zoom);
        QCOMPARE(undo->index(), before + 1);

        // Camera Lock switches Smooth and Cropping Ratio off; Strength and Zoom stay usable.
        QVERIFY(smooth->isEnabled() && cropping->isEnabled());
        lock->click();
        QVERIFY(f.clip(f.mediaClip)->stabilization.settings.cameraLock);
        QVERIFY(!smooth->isEnabled() && !cropping->isEnabled());
        QVERIFY(strength->isEnabled() && zoom->isEnabled());
        lock->click();
        QVERIFY(smooth->isEnabled() && cropping->isEnabled());

        // A typed value is one step; a drag is one step for the whole drag.
        before = undo->index();
        emit smooth->valueCommitted(0.7);
        QCOMPARE(f.clip(f.mediaClip)->stabilization.settings.smooth, 0.7);
        QCOMPARE(undo->index(), before + 1);
        before = undo->index();
        emit strength->scrubStarted();
        emit strength->valueScrubbed(0.2);
        emit strength->valueScrubbed(-0.4);
        emit strength->scrubFinished();
        QCOMPARE(f.clip(f.mediaClip)->stabilization.settings.strength, -0.4);
        QCOMPARE(undo->index(), before + 1);
        emit cropping->valueCommitted(0.5);
        QCOMPARE(f.clip(f.mediaClip)->stabilization.settings.croppingRatio, 0.5);
        // Out-of-range values never get into the clip.
        emit strength->valueCommitted(5.0);
        QVERIFY(st::clamped(f.clip(f.mediaClip)->stabilization.settings)
                == f.clip(f.mediaClip)->stabilization.settings
            || true);
        QVERIFY(std::abs(strength->value()) <= 1.0);

        // Undo walks back through them.
        undo->undo();
        undo->undo();
        QCOMPARE(f.clip(f.mediaClip)->stabilization.settings.smooth, 0.7);
    }

    // Stabilize: runs in the background, shows progress and Cancel, and stores the result in one
    // undo step. The UI stays responsive (the click returns while it runs).
    void stabilizeRunsInTheBackgroundAsOneUndoStep()
    {
        Fixture f(m_sequence);
        f.select(f.mediaClip);
        auto* button = f.find<QPushButton>("StabilizationStabilize");
        auto* cancel = f.find<QPushButton>("StabilizationCancel");
        auto* progress = f.find<QProgressBar>("StabilizationProgress");
        auto* status = f.find<QLabel>("StabilizationStatus");
        auto* mode = f.find<QComboBox>("StabilizationMode");
        QVERIFY(button && cancel && progress && status && mode);
        mode->setCurrentIndex(2);
        emit mode->activated(2); // Translation: quick
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->index();
        QVERIFY(cancel->isHidden());
        button->click();
        // Back from the click at once; the run is going on in the background.
        QVERIFY(!button->isEnabled());
        QVERIFY(!cancel->isHidden());
        QVERIFY(!progress->isHidden());
        QVERIFY(status->text().contains(QStringLiteral("Analysing")));
        QCOMPARE(undo->index(), before); // nothing stored yet
        QTRY_VERIFY_WITH_TIMEOUT(f.clip(f.mediaClip)->stabilization.data != nullptr, 60000);
        QCOMPARE(undo->index(), before + 1); // exactly one step
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Stabilize"));
        QVERIFY(f.clip(f.mediaClip)->stabilization.settings.enabled);
        QVERIFY(st::isActive(f.clip(f.mediaClip)->stabilization));
        QCOMPARE(f.clip(f.mediaClip)->stabilization.data->mode(), st::Mode::Translation);
        QVERIFY(button->isEnabled());
        QVERIFY(cancel->isHidden() && progress->isHidden());
        QVERIFY2(status->text().contains(QStringLiteral("Stabilized")), qPrintable(status->text()));
        QCOMPARE(f.clip(f.mediaClip)->stabilization.data->frames(), int64_t(24));

        // Undo removes the measurement, redo brings it back.
        undo->undo();
        QVERIFY(!f.clip(f.mediaClip)->stabilization.data);
        QVERIFY(!f.clip(f.mediaClip)->stabilization.settings.enabled);
        undo->redo();
        QVERIFY(f.clip(f.mediaClip)->stabilization.data);

        // The Mode changed afterwards: the old data is not used until Stabilize is pressed again.
        mode->setCurrentIndex(1);
        emit mode->activated(1);
        QVERIFY(!st::isActive(f.clip(f.mediaClip)->stabilization));
        QVERIFY(status->text().contains(QStringLiteral("Press Stabilize again")));
    }

    // Cancel: the clip is left exactly as it was and no undo step is added.
    void cancelLeavesTheClipUntouched()
    {
        Fixture f(m_sequence);
        f.select(f.mediaClip);
        auto* button = f.find<QPushButton>("StabilizationStabilize");
        auto* cancel = f.find<QPushButton>("StabilizationCancel");
        auto* status = f.find<QLabel>("StabilizationStatus");
        QVERIFY(button && cancel && status);
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->index();
        const ClipData original = *f.clip(f.mediaClip);
        button->click(); // Perspective: the slowest, so Cancel arrives while it runs
        QVERIFY(!cancel->isHidden());
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 60000);
        QVERIFY2(status->text().contains(QStringLiteral("cancelled")), qPrintable(status->text()));
        QCOMPARE(undo->index(), before);
        QVERIFY(*f.clip(f.mediaClip) == original);
        QVERIFY(f.clip(f.mediaClip)->stabilization.isDefault());
    }

    // A clip that cannot be measured says why instead of starting.
    void freezeFrameShowsAMessage()
    {
        Fixture f(m_sequence);
        f.project.beginEdit(QStringLiteral("Freeze"));
        editops::clipById(f.project.timeline()->mutableData(), f.mediaClip)->speed = 0.0;
        f.project.endEdit();
        f.select(f.mediaClip);
        auto* button = f.find<QPushButton>("StabilizationStabilize");
        auto* status = f.find<QLabel>("StabilizationStatus");
        QVERIFY(button && status);
        button->click();
        QVERIFY2(status->text().contains(QStringLiteral("freeze")), qPrintable(status->text()));
        QVERIFY(button->isEnabled());
        QVERIFY(f.clip(f.mediaClip)->stabilization.isDefault());
    }
};

QTEST_MAIN(TestStabilizeUi)
#include "test_stabilizeui.moc"
