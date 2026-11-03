// SPDX-License-Identifier: GPL-3.0-or-later
// Second bug-hunting round on 1.0.0-beta.1, in the real main window: one test per fix, named
// after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Plural.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/cut/CutEditor.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr int64_t kSecondUs = 1'000'000;

/// Everything main() creates, for one test, with two fake 4 s clips with video and audio.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QStringList ids;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        for (const char* name : {"alpha", "bravo"}) {
            MediaItem m;
            m.name = QLatin1String(name);
            m.path = QStringLiteral("/nonexistent/%1.mp4").arg(m.name);
            m.info.valid = true;
            m.info.hasVideo = true;
            m.info.hasAudio = true;
            m.info.durationUs = 4 * kSecondUs;
            m.info.frameRate = Rational {25, 1};
            m.info.width = 1920;
            m.info.height = 1080;
            ids.append(project.mediaPool()->addItem(m));
        }
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 1000);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }
};

/// Texts of the context menu that a right-click at @p pos in @p w opens (closed again).
QStringList menuTexts(TimelineWidget* w, QPoint pos)
{
    QStringList texts;
    QTimer::singleShot(0, [&texts] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions())
                if (!a->isSeparator())
                    texts.append(a->text().remove(QLatin1Char('&')));
            menu->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, pos, w->viewport()->mapToGlobal(pos));
    QCoreApplication::sendEvent(w->viewport(), &event);
    return texts;
}

/// View position of the middle of clip @p id.
QPoint clipCentre(TimelineWidget* w, const TimelineData& data, quint64 id)
{
    const auto loc = ops::findClip(data, id);
    const ClipData* clip = ops::clipById(data, id);
    const int row = w->layout().rowOf(loc->track);
    return w->layout().clipRect(row, *clip).center().toPoint();
}

} // namespace

class TestBughunt2 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt2"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 35.27: a Cut page edit selects the new clip with its video part current, so the
    // Edit page Inspector stays on the Video tab (it switched to Audio after every Cut edit).
    void spec35_27_cutEditKeepsVideoTab()
    {
        Fixture f;
        f.action("view.page_cut")->trigger();
        QCoreApplication::processEvents();
        InspectorPanel* inspector = f.window->editPage()->inspector();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::VideoTab));
        CutEditor* editor = f.window->cutPage()->editor();
        QVERIFY(editor->run(CutEdit::Append, {SourceSelection {f.ids[0], 0, 0}}));
        QCOMPARE(f.selection.clips().size(), 2);
        const auto loc = ops::findClip(f.project.timeline()->data(), f.selection.currentClip());
        QVERIFY(loc);
        QCOMPARE(loc->track.type, TrackType::Video);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(inspector->tabs()->currentIndex(), int(InspectorPanel::VideoTab));
    }

    // Spec 19.9: Shift+Z fits the timeline; pressed again it returns to the zoom and scroll
    // from before; after a zoom by other means it fits again.
    void spec19_9_shiftZTogglesZoomToFit()
    {
        Fixture f;
        f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[0], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[1], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        QCoreApplication::processEvents();
        TimelineWidget* w = f.timeline();
        w->setPixelsPerFrame(20.0, 300.0);
        w->scrollToFrame(120.0);
        QCoreApplication::processEvents();
        const double ppf = w->layout().pixelsPerFrame();
        const double scroll = w->layout().scrollX();
        QVERIFY(scroll > 0.0);

        f.action("timeline.zoom_fit")->trigger();
        const double fitted = w->layout().pixelsPerFrame();
        QVERIFY(fitted < ppf);
        QCOMPARE(w->layout().scrollX(), 0.0);

        f.action("timeline.zoom_fit")->trigger(); // back to before
        QCOMPARE(w->layout().pixelsPerFrame(), ppf);
        QCOMPARE(w->layout().scrollX(), scroll);

        f.action("timeline.zoom_fit")->trigger(); // fits again
        QCOMPARE(w->layout().pixelsPerFrame(), fitted);
        f.action("timeline.zoom_in")->trigger(); // zoomed by other means: Shift+Z fits
        QVERIFY(w->layout().pixelsPerFrame() > fitted);
        f.action("timeline.zoom_fit")->trigger();
        QCOMPARE(w->layout().pixelsPerFrame(), fitted);
    }

    // Spec 12.15: double-clicking a track name opens an inline editor; Enter renames the track
    // in one undo step, Esc cancels.
    void spec12_15_doubleClickRenamesTrack()
    {
        Fixture f;
        TimelineWidget* w = f.timeline();
        QCoreApplication::processEvents();
        int row = -1;
        for (int r = 0; r < int(w->layout().rows().size()); ++r) {
            if (w->layout().rows()[size_t(r)].ref == TrackRef {TrackType::Video, 0})
                row = r;
        }
        QVERIFY(row >= 0);
        const QRect name = w->layout().headerNameRect(row);
        QVERIFY(!name.isEmpty());
        QTest::mouseDClick(w->viewport(), Qt::LeftButton, Qt::NoModifier, name.center());
        QLineEdit* editor = w->trackNameEditor();
        QVERIFY(editor);
        QCOMPARE(editor->text(), QStringLiteral("Video 1"));
        QVERIFY(editor->hasFocus());
        const int steps = f.project.undoStack()->count();
        QTest::keyClicks(editor, QStringLiteral("Interview"));
        QTest::keyClick(editor, Qt::Key_Return);
        QVERIFY(!w->trackNameEditor());
        QCOMPARE(f.project.timeline()->data().track(TrackRef {TrackType::Video, 0})->name,
            QStringLiteral("Interview"));
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Rename Track"));

        QTest::mouseDClick(w->viewport(), Qt::LeftButton, Qt::NoModifier, name.center());
        editor = w->trackNameEditor();
        QVERIFY(editor);
        QTest::keyClicks(editor, QStringLiteral("Other"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!w->trackNameEditor());
        QCOMPARE(f.project.timeline()->data().track(TrackRef {TrackType::Video, 0})->name,
            QStringLiteral("Interview"));
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
    }

    // Spec 16.7: a timeline clip's context menu offers Paste Attributes (Alt+V) next to Paste.
    void spec16_7_clipMenuOffersPasteAttributes()
    {
        Fixture f;
        const QVector<quint64> ids = f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[0], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        QVERIFY(!ids.isEmpty());
        QCoreApplication::processEvents();
        TimelineWidget* w = f.timeline();
        const QStringList texts = menuTexts(w, clipCentre(w, f.project.timeline()->data(), ids[0]));
        QVERIFY2(texts.contains(QStringLiteral("Paste Attributes…")), qPrintable(texts.join('|')));
        QCOMPARE(texts.indexOf(QStringLiteral("Paste Attributes…")),
            texts.indexOf(QStringLiteral("Paste")) + 1);
    }

    // Spec 24.17: double-clicking a media clip on the Edit page timeline opens its source in
    // the source viewer (as Match Frame at the clicked frame).
    void spec24_17_doubleClickOpensTheSource()
    {
        Fixture f;
        const QVector<quint64> ids = f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[1], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        QVERIFY(!ids.isEmpty());
        QCoreApplication::processEvents();
        ViewerPanel* source = f.window->editPage()->sourceViewer();
        QVERIFY(source->sourceMedia().isEmpty());
        TimelineWidget* w = f.timeline();
        quint64 video = 0;
        for (quint64 id : ids) {
            if (ops::findClip(f.project.timeline()->data(), id)->track.type == TrackType::Video)
                video = id;
        }
        QTest::mouseDClick(w->viewport(), Qt::LeftButton, Qt::NoModifier,
            clipCentre(w, f.project.timeline()->data(), video));
        QCOMPARE(source->sourceMedia(), f.ids[1]);
        QCOMPARE(
            int(f.project.timeline()->data().track(TrackRef {TrackType::Video, 0})->clips.size()),
            1); // nothing moved
    }

    // Spec 23.9: Alt+V (Paste Attributes), Alt+F (Find in Media Pool) and Alt+M (Delete
    // Marker) reach their commands: the menu bar titles drop the mnemonic of a bound
    // Alt+letter (Qt ignored both as an ambiguous shortcut), the others keep theirs, and a
    // rebind frees it again.
    void spec23_9_altShortcutsWinOverMenuMnemonics()
    {
        Fixture f;
        QStringList titles;
        for (QAction* a : f.window->menuBar()->actions())
            titles.append(a->text());
        QVERIFY2(titles.contains(QStringLiteral("View")), qPrintable(titles.join('|')));
        QVERIFY(titles.contains(QStringLiteral("File")));
        QVERIFY(titles.contains(QStringLiteral("Mark")));
        QVERIFY(titles.contains(QStringLiteral("&Edit")));
        QVERIFY(titles.contains(QStringLiteral("&Playback")));

        QSignalSpy paste(f.action("edit.paste_attributes"), &QAction::triggered);
        QTest::keyClick(f.window.get(), Qt::Key_V, Qt::AltModifier);
        QCOMPARE(paste.count(), 1);

        f.actions.setShortcuts(QStringLiteral("edit.paste_attributes"), {});
        bool view = false;
        for (QAction* a : f.window->menuBar()->actions())
            view = view || a->text() == QLatin1String("&View");
        QVERIFY(view);
    }

    // Spec 33.32: counted messages read "1 caption" / "2 captions", never "1 caption(s)"
    // (Qt's %n plural forms need a translation, which Kinoklip does not ship).
    void spec33_32_countedMessagesArePlural()
    {
        QCOMPARE(plural(1, QStringLiteral("%n clip"), QStringLiteral("%n clips")),
            QStringLiteral("1 clip"));
        QCOMPARE(plural(0, QStringLiteral("%n clip"), QStringLiteral("%n clips")),
            QStringLiteral("0 clips"));
        Project project;
        MediaItem sub;
        sub.kind = MediaItemKind::Subtitle;
        sub.name = QStringLiteral("one.srt");
        sub.path = QStringLiteral("/nonexistent/one.srt");
        sub.cues.append(SubtitleCue {0, kSecondUs, QStringLiteral("Hello")});
        project.mediaPool()->addItem(sub);
        sub.name = QStringLiteral("two.srt");
        sub.path = QStringLiteral("/nonexistent/two.srt");
        sub.cues.append(SubtitleCue {kSecondUs, 2 * kSecondUs, QStringLiteral("again")});
        project.mediaPool()->addItem(sub);
        MediaPoolModel model(&project);
        QStringList texts;
        for (int r = 0; r < model.rowCount(); ++r) {
            for (int c = 0; c < model.columnCount(); ++c) {
                const QString t = model.index(r, c).data().toString();
                if (t.contains(QLatin1String("caption")))
                    texts.append(t);
            }
        }
        texts.sort();
        QCOMPARE(texts, (QStringList {QStringLiteral("1 caption"), QStringLiteral("2 captions")}));
    }
};

QTEST_MAIN(TestBughunt2)
#include "test_bughunt2.moc"
