// SPDX-License-Identifier: GPL-3.0-or-later
// Ninth bug-hunting round (a beginner's full project, round 8 leftovers), in the real main
// window where it matters: one test per fix, named after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/ColorPage.h"
#include "app/pages/EditPage.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorSession.h"
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/Grade.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/common/FolderImport.h"
#include "ui/common/MediaImport.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "platform/FileDialogs.h"
#include "app/pages/CutPage.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace gk = kk::gradekeys;

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

class TestBughunt9 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt9"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 28.19: with several captions selected, typing in the Captions tab changes the
    // shown caption only (each caption has its own words); its name follows only its text.
    void spec28_19_captionTextIsPerCaption()
    {
        Fixture f;
        f.show();
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        f.project.timeline()->setPlayhead(10);
        const quint64 a = controller->addSubtitleAtPlayhead(0);
        f.project.timeline()->setPlayhead(200);
        const quint64 b = controller->addSubtitleAtPlayhead(0);
        QVERIFY(a && b && a != b);
        const QString textA = subtitles::captionText(*editops::clipById(f.project.timeline()->data(), a));
        f.selection.setClips({a, b}); // b is the shown (current) caption
        QCOMPARE(f.selection.currentClip(), b);
        auto* edit
            = f.inspector()->findChild<QPlainTextEdit*>(QStringLiteral("text:") + keys::Text);
        QVERIFY(edit);
        edit->setPlainText(QStringLiteral("Only me"));
        const TimelineData& tl = f.project.timeline()->data();
        QCOMPARE(subtitles::captionText(*editops::clipById(tl, b)), QStringLiteral("Only me"));
        QCOMPARE(subtitles::captionText(*editops::clipById(tl, a)), textA);
        QVERIFY(editops::clipById(tl, a)->name != QStringLiteral("Only me"));
    }

    // Spec 28.20: a cue without text is skipped on import (it would be an invisible caption)
    // and the import says how many were skipped.
    void spec28_20_emptyCuesSkippedAndCounted()
    {
        const QByteArray srt = "1\n00:00:01,000 --> 00:00:02,000\nHello\n\n"
                               "2\n00:00:03,000 --> 00:00:04,000\n<i></i>\n\n"
                               "3\n00:00:05,000 --> 00:00:06,000\n\n"
                               "4\n00:00:07,000 --> 00:00:08,000\nBye\n";
        QVector<SubtitleCue> cues;
        int empty = -1;
        QVERIFY(subtitles::parse(srt, &cues, nullptr, nullptr, &empty));
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0].text, QStringLiteral("Hello"));
        QCOMPARE(cues[1].text, QStringLiteral("Bye"));
        QCOMPARE(empty, 2);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("empty_cues.srt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(srt);
        file.close();
        Project project;
        const ImportResult result = project.importMedia({path});
        QCOMPARE(result.importedIds.size(), 1);
        QVERIFY(result.warnings.isEmpty());
        QCOMPARE(result.notes.size(), 1);
        QVERIFY2(result.notes[0].contains(QStringLiteral("2 empty subtitle cues were skipped")),
            qPrintable(result.notes[0]));
        const MediaItem* item = project.mediaPool()->item(result.importedIds[0]);
        QVERIFY(item);
        QCOMPARE(item->cues.size(), 2);

        // A file of only empty cues is not imported.
        QVector<SubtitleCue> none;
        QString error;
        QVERIFY(!subtitles::parse("1\n00:00:01,000 --> 00:00:02,000\n\n", &none, &error));
        QVERIFY(!error.isEmpty());
    }

    // Spec 45.2: Export subtitle is greyed out for audio-only formats (no picture to burn into,
    // nothing to embed into) and an audio render never carries subtitles.
    void spec45_2_noSubtitleExportForAudio()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("deliver"));
        RenderSettingsPanel* panel = f.window->deliverPage()->settingsPanel();
        auto* box = panel->findChild<QCheckBox*>(QStringLiteral("RenderSubtitleExport"));
        QVERIFY(box);
        ExportSettings s = panel->settings();
        s.subtitles.enabled = true;
        panel->setSettings(s);
        QVERIFY(box->isEnabled());
        QVERIFY(panel->settings().subtitles.enabled);
        for (const char* audio : {"wav", "mp3", "flac"}) {
            s = panel->settings();
            s.container = QLatin1String(audio);
            s.normalize();
            s.subtitles.enabled = true;
            panel->setSettings(s);
            QVERIFY2(!box->isEnabled(), audio);
            QVERIFY2(!panel->settings().subtitles.enabled, audio);
        }
    }

    // Spec 38.31: no interface text is double-encoded UTF-8 ("Importâ€¦" for "Import…"): the
    // sources hold no such byte sequence, and the Project Manager's buttons read right.
    void spec38_31_noDoubleEncodedTexts()
    {
        const QString src = QStringLiteral(KK_TEST_DATA_DIR "/../../src");
        QVERIFY(QDir(src).exists());
        // U+00E2 U+0080..U+00BF in UTF-8: a UTF-8 lead byte E2 (… — “ ” ’) read as Latin-1.
        QStringList bad;
        QDirIterator it(src, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
            QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            for (qsizetype i = bytes.indexOf("\xc3\xa2\xc2"); i >= 0;
                 i = bytes.indexOf("\xc3\xa2\xc2", i + 1)) {
                bad.append(QStringLiteral("%1 at byte %2").arg(file.fileName()).arg(i));
                break;
            }
        }
        QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    }
    // Spec 44b.30: in the Color page's clips strip a Shift / Ctrl selection is framed in the
    // highlight colour, and right-clicking the graded clip keeps that selection, so its menu's
    // Copy Grade to Selected Clips reaches the selected clips (it is greyed out without them).
    void spec44b_30_stripSelectionAndCopyGrade()
    {
        Fixture f(4);
        f.show();
        f.action("view.page_color")->trigger();
        QCoreApplication::processEvents();
        ColorClipStrip* strip = f.window->colorPage()->clipStrip();
        ColorSession* session = f.window->colorPage()->session();
        QCOMPARE(strip->count(), 4);
        const auto centre = [&](int row) { return strip->visualItemRect(strip->item(row)).center(); };
        // Grade clip 1.
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, {}, centre(0));
        QCOMPARE(session->clipId(), f.clips[0]);
        QVERIFY(session->setParam(0, gk::Saturation, 1.6, QStringLiteral("Sat")));
        const ClipGrade graded = f.project.clipGrade(f.project.activeTimelineUid(), f.clips[0]);
        QVERIFY(!graded.isEmpty());
        // Ctrl-click clip 1 out of the selection, Ctrl-click clip 2 in, Shift-click clip 4.
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier, centre(0));
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier, centre(1));
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ShiftModifier, centre(3));
        QCOMPARE(session->selectedClips().size(), 3);
        QVERIFY(!session->selectedClips().contains(f.clips[0]));

        // Selected clips are framed in the highlight colour; unselected ones are not.
        const QImage shot = strip->viewport()->grab().toImage();
        const QColor highlight = strip->palette().color(QPalette::Highlight);
        const auto framed = [&](int row) {
            // Along the item's left edge, beside the thumbnail.
            const QRect r = strip->visualItemRect(strip->item(row));
            for (int x = r.left(); x < r.left() + 10; ++x) {
                if (shot.pixelColor(x * shot.devicePixelRatio(), r.center().y()).rgb()
                    == highlight.rgb())
                    return true;
            }
            return false;
        };
        QVERIFY(framed(1) && framed(2) && framed(3));
        QVERIFY(!framed(0));

        // Right-click clip 1 (not selected): the selection stays; Copy Grade to Selected Clips.
        QTest::mousePress(strip->viewport(), Qt::RightButton, {}, centre(0));
        QTest::mouseRelease(strip->viewport(), Qt::RightButton, {}, centre(0));
        QCOMPARE(session->selectedClips().size(), 3);
        bool triggered = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            for (QAction* a : menu->actions()) {
                if (a->text() == QStringLiteral("Copy Grade to Selected Clips")) {
                    QVERIFY(a->isEnabled());
                    triggered = true;
                    menu->setActiveAction(a);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            }
            menu->close();
        });
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, centre(0),
            strip->viewport()->mapToGlobal(centre(0)));
        QApplication::sendEvent(strip->viewport(), &menuEvent);
        QVERIFY(triggered);
        for (int i = 1; i < 4; ++i)
            QCOMPARE(f.project.clipGrade(f.project.activeTimelineUid(), f.clips[i]), graded);
    }

    // Spec 24.22: Ctrl+S right after typing a title or caption text (before the typing pause
    // closes its undo step) saves the text and leaves the project clean: no "•" and no
    // "save changes?" question on closing.
    void spec24_22_saveRightAfterTypingIsClean()
    {
        Fixture f;
        f.show();
        QTemporaryDir dir;
        QVERIFY(f.project.save(dir.filePath(QStringLiteral("typed.kkproj"))));
        QVERIFY(!f.project.isDirty());
        f.project.timeline()->setPlayhead(10);
        f.action("timeline.add_subtitle")->trigger();
        const quint64 id = f.selection.currentClip();
        QVERIFY(f.action("file.save")->isEnabled());
        f.action("file.save")->trigger();
        QVERIFY(!f.project.isDirty());
        auto* edit
            = f.inspector()->findChild<QPlainTextEdit*>(QStringLiteral("text:") + keys::Text);
        QVERIFY(edit);
        edit->setFocus();
        QTest::keyClicks(edit, QStringLiteral("Hello"));
        QVERIFY(f.project.isEditing()); // the typing step is still open
        f.action("file.save")->trigger(); // well inside the typing pause
        QVERIFY(!f.project.isDirty());
        QTest::qWait(1200); // the pause passes: nothing is left to close
        QVERIFY(!f.project.isDirty());
        QVERIFY(subtitles::captionText(*editops::clipById(f.project.timeline()->data(), id))
                    .contains(QStringLiteral("Hello")));
    }

    // Spec 33.37: the media pool shows the same bin on every page; File > Import > Media goes
    // into the bin shown (as in Resolve), and Import Media Folder then shows the bin it made
    // for the folder, so the imported clips are in view.
    void spec33_37_importIntoTheShownBin()
    {
        Fixture f;
        f.show();
        f.project.beginEdit(QStringLiteral("Add Bin"));
        const QString binId = f.project.mediaPool()->addBin(QStringLiteral("Footage"), {});
        f.project.endEdit();
        MediaPoolPanel* edit = f.window->editPage()->mediaPool();
        edit->setCurrentBin(binId);
        f.action("view.page_cut")->trigger();
        QCoreApplication::processEvents();
        MediaPoolPanel* cut = f.window->cutPage()->mediaPool();
        QCOMPARE(cut->currentBin(), binId);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(edit->currentBin(), binId);

        const QString still = QStringLiteral(KK_TEST_DATA_DIR "/generated/still.jpg");
        QVERIFY(QFileInfo::exists(still));
        platform::setFileDialogHandlerForTests(
            [&](const platform::FileDialogRequest&) { return QStringList {still}; });
        f.action("file.import_media")->trigger();
        const MediaItem* item = f.project.mediaPool()->itemByPath(QFileInfo(still).absoluteFilePath());
        QVERIFY(item);
        QCOMPARE(item->binId, binId);

        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("holiday"));
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(QFile::copy(QStringLiteral(KK_TEST_DATA_DIR "/generated/still_alpha.png"),
            folder + QStringLiteral("/logo.png")));
        platform::setFileDialogHandlerForTests(
            [&](const platform::FileDialogRequest&) { return QStringList {folder}; });
        setFolderImportOptionsHandlerForTests([](FolderImportOptions* o) {
            o->includeSubfolders = true;
            o->createBins = true;
            return true;
        });
        f.action("file.import_media_folder")->trigger();
        platform::setFileDialogHandlerForTests({});
        setFolderImportOptionsHandlerForTests({});
        const MediaItem* logo = f.project.mediaPool()->itemByPath(folder + QStringLiteral("/logo.png"));
        QVERIFY(logo);
        const Bin* made = f.project.mediaPool()->bin(logo->binId);
        QVERIFY(made);
        QCOMPARE(made->name, QStringLiteral("holiday"));
        QCOMPARE(made->parentId, binId); // inside the bin shown
        QCOMPARE(edit->currentBin(), logo->binId); // and now in view
    }

    // Spec 25.18: the first video imported into a project at another frame rate brings the
    // question "Change the project frame rate?" (as in Resolve); Change sets the project and
    // its empty timelines to the clip's rate. Not asked for stills, for later imports, or once
    // a timeline that follows the project has clips.
    void spec25_18_firstImportOffersTheClipFrameRate()
    {
        const QString video = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        const QString still = QStringLiteral(KK_TEST_DATA_DIR "/generated/still.jpg");
        QVERIFY(QFileInfo::exists(video));
        int asked = 0;
        Rational askedClip {0, 1};
        setFrameRatePromptHandlerForTests([&](Rational clip, Rational project) {
            ++asked;
            askedClip = clip;
            return project == Rational {24, 1};
        });
        QString pick;
        platform::setFileDialogHandlerForTests(
            [&](const platform::FileDialogRequest&) { return QStringList {pick}; });
        {
            Fixture f(0);
            f.show();
            QCOMPARE(f.project.settings().fps, (Rational {24, 1}));
            pick = still; // a still has no frame rate: not asked
            f.action("file.import_media")->trigger();
            QCOMPARE(asked, 0);
            f.project.undoStack()->undo(); // back to an empty pool
            pick = video;
            f.action("file.import_media")->trigger();
            QCOMPARE(asked, 1);
            QCOMPARE(askedClip, (Rational {25, 1}));
            QCOMPARE(f.project.settings().fps, (Rational {25, 1}));
            QCOMPARE(f.project.timeline()->data().fps, (Rational {25, 1}));
            pick = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_2997.mp4");
            f.action("file.import_media")->trigger(); // not the first import
            QCOMPARE(asked, 1);
        }
        {
            Fixture f(2); // the timeline already has clips: its rate is locked
            pick = video;
            f.action("file.import_media")->trigger();
            QCOMPARE(asked, 1);
            QCOMPARE(f.project.settings().fps, (Rational {24, 1}));
        }
        platform::setFileDialogHandlerForTests({});
        setFrameRatePromptHandlerForTests({});
    }

    // Spec 43.11: the getting-started guide describes the Compositing and Color pages that
    // ship (it called them "not finished yet") and links their articles.
    void spec43_11_guideDescribesTheShippedPages()
    {
        QFile guide(QStringLiteral(KK_TEST_DATA_DIR "/../../docs/guide/getting-started.md"));
        QVERIFY(guide.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(guide.readAll());
        QVERIFY(!text.contains(QStringLiteral("not finished yet")));
        QVERIFY(text.contains(QStringLiteral("(colour-correct-a-clip.md)")));
        QVERIFY(text.contains(QStringLiteral("(compositing-page.md)")));
    }
};

QTEST_MAIN(TestBughunt9)
#include "test_bughunt9.moc"
