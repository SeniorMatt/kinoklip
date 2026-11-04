// SPDX-License-Identifier: GPL-3.0-or-later
// Eighth bug-hunting round (subtitles, menu bar), in the real main window where it matters: one
// test per fix, named after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QToolBar>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, with @p clipCount clips of 50 frames on V1 (Solid, or Media
/// with 20 s of material when @p media).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 2, bool media = false)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = media ? ClipKind::Media : ClipKind::Solid;
            clip.solidColor = QColor(40 + 60 * i, 90, 200);
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clip.sourceDurationUs = 20'000'000;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    const ClipData* clip(int i) const
    {
        return editops::clipById(project.timeline()->data(), clips[i]);
    }
    InspectorPanel* inspector() const { return window->editPage()->inspector(); }
    ParamGroupBox* group(const char* name) const
    {
        return inspector()->findChild<ParamGroupBox*>(QLatin1String(name));
    }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt8 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt8"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 28.15: selecting a caption shows the Inspector's Captions tab (a caption's text and
    // style are all there); selecting a video clip again goes back to the Video tab.
    void spec28_15_captionShowsCaptionsTab()
    {
        Fixture f;
        f.show();
        f.selection.selectClip(f.clips[0]);
        QTabWidget* tabs = f.inspector()->tabs();
        QCOMPARE(tabs->currentIndex(), int(InspectorPanel::VideoTab));
        f.project.timeline()->setPlayhead(10);
        f.action("timeline.add_subtitle")->trigger();
        QCOMPARE(f.project.timeline()->data().subtitleTracks.size(), size_t(1));
        QCOMPARE(tabs->currentIndex(), int(InspectorPanel::TitleTab));
        f.selection.selectClip(f.clips[1]);
        QCOMPARE(tabs->currentIndex(), int(InspectorPanel::VideoTab));
    }

    // Spec 28.16: Add Subtitle from a subtitle track's right-click menu puts the caption on
    // that track, not on ST1.
    void spec28_16_addSubtitleOnTheClickedTrack()
    {
        Fixture f;
        f.action("timeline.add_subtitle_track")->trigger();
        f.action("timeline.add_subtitle_track")->trigger();
        QCOMPARE(f.project.timeline()->data().subtitleTracks.size(), size_t(2));
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        f.project.timeline()->setPlayhead(20);
        const quint64 id = controller->addSubtitleAtPlayhead(1);
        QVERIFY(id != 0);
        const auto loc = editops::findClip(f.project.timeline()->data(), id);
        QVERIFY(loc);
        QCOMPARE(loc->track.type, TrackType::Subtitle);
        QCOMPARE(loc->track.index, 1);
        QCOMPARE(f.project.timeline()->data().subtitleTracks[0].clips.size(), size_t(0));
    }

    // Spec 28.17: a caption is named after its text: typing in the Captions tab renames the
    // clip (first line), so the Inspector header and the Edit Index show the caption.
    void spec28_17_captionNameFollowsItsText()
    {
        Fixture f;
        f.show();
        f.project.timeline()->setPlayhead(10);
        f.action("timeline.add_subtitle")->trigger();
        const quint64 id = f.selection.currentClip();
        auto* edit
            = f.inspector()->findChild<QPlainTextEdit*>(QStringLiteral("text:") + keys::Text);
        QVERIFY(edit);
        edit->setPlainText(QStringLiteral("Hello there\nsecond line"));
        const ClipData* c = editops::clipById(f.project.timeline()->data(), id);
        QVERIFY(c);
        QCOMPARE(c->title.staticValue(keys::Text).toString(),
            QStringLiteral("Hello there\nsecond line"));
        QCOMPARE(c->name, QStringLiteral("Hello there"));
    }
};

QTEST_MAIN(TestBughunt8)
#include "test_bughunt8.moc"
