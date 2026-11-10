// SPDX-License-Identifier: GPL-3.0-or-later
// Tenth bug-hunting round (title presets, round 9 leftovers), in the real main window where it
// matters: one test per fix, named after the spec row it covers. The title pixels (position,
// anchor, scroll; GPU against CPU) are in tests/render/test_compositor.cpp, the drop of a
// video-only clip in tests/timeline/test_timelinedrop.cpp.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/ColorPage.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Grade.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "core/TitlePresets.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/cut/CutEditor.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ValueField.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <memory>

using namespace kk;
namespace gk = kk::gradekeys;

namespace {

/// Everything main() creates, with @p clipCount Solid clips of 50 frames on V1.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 2)
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
            clip.kind = ClipKind::Solid;
            clip.solidColor = QColor(40 + 60 * i, 90, 200);
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
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
    InspectorPanel* inspector() const { return window->editPage()->inspector(); }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt10 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt10"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 14.6: the Effects library's Titles hold our own presets (the lower thirds, Scroll,
    // Text, Text Box), all Title clips with the same parameters; a preset gives its own
    // defaults and the Inspector's Title tab shows and edits them.
    void spec14_6_titlePresetsInTheLibrary()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        QStringList names;
        for (const EffectDescriptor* d : registry.byCategory(EffectCategory::Title))
            names << d->name;
        QCOMPARE(names,
            QStringList({QStringLiteral("Left Lower Third"), QStringLiteral("Middle Lower Third"),
                QStringLiteral("Right Lower Third"), QStringLiteral("Scroll"),
                QStringLiteral("Text"), QStringLiteral("Text Box")}));
        // Every preset has the same parameters as Text (any title can become any other).
        const EffectDescriptor* text = registry.find(titles::Text);
        QVERIFY(text);
        for (const EffectDescriptor* d : registry.byCategory(EffectCategory::Title)) {
            QCOMPARE(d->params.size(), text->params.size());
            for (int i = 0; i < d->params.size(); ++i)
                QCOMPARE(d->params[i].key, text->params[i].key);
        }

        Fixture f;
        f.show();
        // The library lists them under Titles.
        auto* library = f.window->findChild<EffectsLibraryPanel*>();
        QVERIFY(library);
        // The effects tree (the other tree is the category sidebar).
        QStringList listed;
        for (QTreeWidget* tree : library->findChildren<QTreeWidget*>()) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* section = tree->topLevelItem(i);
                if (section->text(0) != QStringLiteral("Titles") || section->childCount() == 0)
                    continue;
                for (int j = 0; j < section->childCount(); ++j)
                    listed << section->child(j)->text(0);
            }
        }
        QCOMPARE(listed, names);

        // Double-click equivalent: a Left Lower Third at the playhead, named after the preset.
        f.project.timeline()->setPlayhead(10);
        QVERIFY(effectactions::activate(f.ctx, titles::LeftLowerThird));
        const quint64 id = f.selection.currentClip();
        const ClipData* clip = editops::clipById(f.project.timeline()->data(), id);
        QVERIFY(clip);
        QCOMPARE(clip->kind, ClipKind::Title);
        QCOMPARE(clip->generator, titles::LeftLowerThird);
        QCOMPARE(clip->name, QStringLiteral("Left Lower Third"));
        QCOMPARE(clip->title.staticValue(keys::Anchor).toString(), QStringLiteral("left"));
        QCOMPARE(clip->title.staticValue(keys::BgEnabled).toBool(), true);
        QVERIFY(clip->title.value(keys::TextPosX, 0) < 0.2);
        QVERIFY(clip->title.value(keys::TextPosY, 0) < 0.3);
        QVERIFY(clip->title.staticValue(keys::Text).toString().contains(QLatin1Char('\n')));

        // The Title tab: the preset's group with text, font, size, colours, box and position.
        InspectorPanel* inspector = f.inspector();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::TitleTab));
        QWidget* page = inspector->tabs()->widget(InspectorPanel::TitleTab);
        QStringList groupTitles;
        for (auto* label : page->findChildren<QLabel*>(QStringLiteral("InspectorGroupTitle")))
            groupTitles << label->text();
        // The preset's own group, and the Layers group the Title tab now starts with.
        QVERIFY(groupTitles.contains(QStringLiteral("Left Lower Third")));
        QVERIFY(groupTitles.contains(QStringLiteral("Layers")));
        QCOMPARE(groupTitles.size(), 2);
        for (const QString& key : {keys::Size, keys::TextPosX, keys::TextPosY, keys::BgOpacity})
            QVERIFY2(page->findChild<ValueField*>(QStringLiteral("value:") + key), qPrintable(key));
        auto* textEdit = page->findChild<QPlainTextEdit*>(QStringLiteral("text:") + keys::Text);
        QVERIFY(textEdit);
        // Enter in the text makes a new line (the guide says so).
        textEdit->setFocus();
        textEdit->moveCursor(QTextCursor::End);
        const int lines = textEdit->toPlainText().count(QLatin1Char('\n'));
        QTest::keyClick(textEdit, Qt::Key_Return);
        QCOMPARE(textEdit->toPlainText().count(QLatin1Char('\n')), lines + 1);
        QVERIFY(page->findChild<QComboBox*>(QStringLiteral("choice:") + keys::Anchor));
        auto* posX = page->findChild<ValueField*>(QStringLiteral("value:") + keys::TextPosX);
        emit posX->valueCommitted(0.3);
        clip = editops::clipById(f.project.timeline()->data(), id);
        QCOMPARE(clip->title.value(keys::TextPosX, 0), 0.3);

        // A Scroll title scrolls; a Text title is still "Basic Title", unnamed by a preset.
        f.project.timeline()->setPlayhead(300);
        QVERIFY(effectactions::activate(f.ctx, titles::Scroll));
        clip = editops::clipById(f.project.timeline()->data(), f.selection.currentClip());
        QCOMPARE(clip->title.staticValue(keys::Scroll).toBool(), true);
        f.project.timeline()->setPlayhead(600);
        QVERIFY(effectactions::activate(f.ctx, titles::Text));
        clip = editops::clipById(f.project.timeline()->data(), f.selection.currentClip());
        QVERIFY(clip->generator.isEmpty());
        QCOMPARE(clip->name, QStringLiteral("Basic Title"));
        QCOMPARE(clip->title.staticValue(keys::Scroll).toBool(), false);
    }

    // Spec 14.9: selecting a title clip shows the Inspector's Title tab (where its text is);
    // selecting a clip without a title afterwards goes back to the Video tab.
    void spec14_9_selectingATitleShowsTheTitleTab()
    {
        Fixture f;
        f.show();
        const quint64 title = effectactions::addGeneratorClip(f.project, titles::Text, 200);
        QVERIFY(title);
        InspectorPanel* inspector = f.inspector();
        f.selection.selectClip(f.clips[0]);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::VideoTab));
        f.selection.selectClip(title);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::TitleTab));
        f.selection.selectClip(f.clips[1]);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::VideoTab));
        // A tab the user picks on another clip (Effects) is left alone by a non-title clip.
        inspector->tabs()->setCurrentIndex(InspectorPanel::EffectsTab);
        f.selection.selectClip(f.clips[0]);
        QCoreApplication::processEvents();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::EffectsTab));
    }

    // Spec 35.28: Append on the Cut page of a marked range of a 25 fps source onto a 24 fps
    // timeline covers the whole range: In 1:00 / Out 7:00 (151 source frames, 6.04 s) is a
    // 6:01 clip (145 frames), so the Out frame is shown; same-rate ranges stay exact.
    void spec35_28_appendOfAnotherRateCoversTheMarkedRange()
    {
        Fixture f(0);
        f.project.beginEdit(QStringLiteral("Rate"));
        f.project.timeline()->mutableData().fps = Rational {24, 1};
        f.project.endEdit();
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/clip25.mp4");
        item.name = QStringLiteral("clip25.mp4");
        item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
        item.info.frameRate = Rational {25, 1};
        item.info.durationUs = 20'000'000;
        const QString mediaId = f.project.mediaPool()->addItem(item);
        // In at source frame 25, Out at frame 175 inclusive: the exclusive end is frame 176.
        const SourceSelection marked {mediaId, framesToUs(25, {25, 1}), framesToUs(176, {25, 1})};
        CutEditor* editor = f.window->cutPage()->editor();
        QVERIFY(editor->run(CutEdit::Append, {marked}));
        const TimelineData& tl = f.project.timeline()->data();
        QCOMPARE(int(tl.videoTracks[0].clips.size()), 1);
        const ClipData& clip = tl.videoTracks[0].clips[0];
        QCOMPARE(clip.duration, int64_t(145));
        // Its last frame shows the Out frame (source 7.00 s .. 7.04 s).
        const int64_t lastUs = clip.sourceInUs + framesToUs(clip.duration - 1, tl.fps);
        QVERIFY2(lastUs >= 7'000'000 && lastUs < 7'040'000, QByteArray::number(qint64(lastUs)));

        // A range that ends at the end of the media never runs past it.
        const SourceSelection toEnd {mediaId, 19'000'000, 20'000'000};
        QVERIFY(editor->run(CutEdit::Append, {toEnd}));
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.back().duration, int64_t(24));

        // The same rate: exactly the marked frames (6:01 at 24 = 145 frames).
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        MediaItem same = item;
        same.path = QStringLiteral("/nonexistent/clip24.mp4");
        same.info.frameRate = Rational {24, 1};
        const QString sameId = f.project.mediaPool()->addItem(same);
        const auto clips = controller->makeMediaClips(
            sameId, framesToUs(24, {24, 1}), framesToUs(169, {24, 1}), 0, 0);
        QVERIFY(!clips.isEmpty());
        QCOMPARE(clips.front().clip.duration, int64_t(145));
    }

    // Round 9 leftover (not reproduced, no fix): after copying a grade in the clips strip
    // (Copy Grade to Selected Clips, middle-click), every target thumbnail shows the graded dot
    // at once, also the last one. Checks what was painted, not a fresh repaint.
    void gradedDotAfterCopyIsPainted()
    {
        Fixture f(4);
        f.show();
        f.action("view.page_color")->trigger();
        QCoreApplication::processEvents();
        ColorClipStrip* strip = f.window->colorPage()->clipStrip();
        ColorSession* session = f.window->colorPage()->session();
        QCOMPARE(strip->count(), 4);
        const auto centre
            = [&](int row) { return strip->visualItemRect(strip->item(row)).center(); };
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, {}, centre(0));
        QVERIFY(session->setParam(0, gk::Saturation, 1.6, QStringLiteral("Sat")));
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier, centre(0));
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier, centre(1));
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ShiftModifier, centre(3));
        QCOMPARE(session->selectedClips().size(), 3);
        QTest::qWait(50);

        // What is on screen: the offscreen screen grab returns the window's backing store as
        // last painted (not a fresh repaint), so a missing update shows there.
        const auto dotShown = [&](int row) {
            QCoreApplication::processEvents();
            QWidget* top = f.window.get();
            const QImage shot = top->screen()->grabWindow(top->winId()).toImage();
            const QRect item = strip->visualItemRect(strip->item(row));
            const QPoint origin = strip->viewport()->mapTo(top, item.topLeft());
            const double dpr = shot.devicePixelRatio();
            for (int y = 0; y < item.height() / 2; ++y) {
                for (int x = item.width() / 2; x < item.width(); ++x) {
                    const QColor c
                        = shot.pixelColor(int((origin.x() + x) * dpr), int((origin.y() + y) * dpr));
                    if (std::abs(c.red() - 90) < 25 && std::abs(c.green() - 200) < 25
                        && std::abs(c.blue() - 120) < 25)
                        return true;
                }
            }
            return false;
        };
        QVERIFY(dotShown(0));
        QVERIFY(!dotShown(3));
        // The right-click menu's Copy Grade to Selected Clips (the round 9 workflow).
        QTest::mousePress(strip->viewport(), Qt::RightButton, {}, centre(0));
        QTest::mouseRelease(strip->viewport(), Qt::RightButton, {}, centre(0));
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            for (QAction* a : menu->actions()) {
                if (a->text() == QStringLiteral("Copy Grade to Selected Clips")) {
                    menu->setActiveAction(a);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            }
            menu->close();
        });
        QContextMenuEvent menuEvent(
            QContextMenuEvent::Mouse, centre(0), strip->viewport()->mapToGlobal(centre(0)));
        QApplication::sendEvent(strip->viewport(), &menuEvent);
        QTest::qWait(50);
        for (int row = 1; row < 4; ++row)
            QVERIFY2(dotShown(row), qPrintable(QString::number(row)));

        // Middle-click copies too: reset clip 4, then middle-click clip 1.
        f.project.setClipGrade(
            f.project.activeTimelineUid(), f.clips[3], ClipGrade {}, QStringLiteral("Reset"));
        QTest::qWait(50);
        QVERIFY(!dotShown(3));
        QTest::mouseClick(strip->viewport(), Qt::MiddleButton, {}, centre(0));
        QTest::qWait(50);
        for (int row = 1; row < 4; ++row)
            QVERIFY2(dotShown(row), qPrintable(QString::number(row)));
    }
};

QTEST_MAIN(TestBughunt10)
#include "test_bughunt10.moc"
