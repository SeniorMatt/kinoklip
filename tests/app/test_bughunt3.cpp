// SPDX-License-Identifier: GPL-3.0-or-later
// Third bug-hunting round, in the real main window: one test per fix, named after the spec row
// it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "ui/deliver/DeliverPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/NodeRegistry.h"
#include "core/ParamKeys.h"
#include "core/Plural.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Serialization.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "export/EdlExport.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/TimecodeEntry.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/cut/CutEditor.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/editindex/EditIndexModel.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ValueField.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineDialogs.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr int64_t kSecondUs = 1'000'000;
constexpr TrackRef kV1 {TrackType::Video, 0};

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

class TestBughunt3 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt3"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 10.12: a focused combo box or spin box takes Up / Down (and Page Up / Down); the
    // timeline's previous / next edit act again once the focus is elsewhere.
    void spec10_12_focusedComboAndSpinKeepArrowKeys()
    {
        Fixture f;
        f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[0], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[1], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        f.project.timeline()->setPlayhead(10);
        QCoreApplication::processEvents();

        auto* combo = new QComboBox(f.window->editPage());
        combo->addItems({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        auto* spin = new QSpinBox(f.window->editPage());
        spin->setRange(0, 100);
        spin->setValue(5);
        combo->show();
        spin->show();

        combo->setFocus(Qt::MouseFocusReason);
        QCoreApplication::processEvents();
        QVERIFY(combo->hasFocus());
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(10));
        QTest::keyClick(combo, Qt::Key_Up);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(10));

        combo->setEditable(true);
        combo->setFocus(Qt::MouseFocusReason);
        QCoreApplication::processEvents();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(10));

        spin->setFocus(Qt::MouseFocusReason);
        QCoreApplication::processEvents();
        QWidget* spinFocus = QApplication::focusWidget();
        QTest::keyClick(spinFocus, Qt::Key_Up);
        QCOMPARE(spin->value(), 6);
        QTest::keyClick(spinFocus, Qt::Key_Down);
        QTest::keyClick(spinFocus, Qt::Key_Down);
        QCOMPARE(spin->value(), 4);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(10));

        // Elsewhere Down steps to the next edit again.
        spin->clearFocus();
        QCoreApplication::processEvents();
        QTest::keyClick(f.window.get(), Qt::Key_Down);
        QVERIFY(f.project.timeline()->playhead() > 10);
    }

    // Spec 25.12: new projects run at 24 fps and their timelines start at 01:00:00:00 (also
    // timelines created later); the start survives save / load.
    void spec25_12_newProjectsStartAtOneHourAt24Fps()
    {
        Project project;
        QCOMPARE(project.settings().fps, (Rational {24, 1}));
        QCOMPARE(project.settings().timelineStart, int64_t(24 * 3600));
        QCOMPARE(project.timeline()->data().fps, (Rational {24, 1}));
        QCOMPARE(project.timeline()->data().startTimecode, int64_t(24 * 3600));
        QCOMPARE(
            timecode::formatRecord(0, project.timeline()->data()), QStringLiteral("01:00:00:00"));
        const quint64 second = project.createTimeline(QStringLiteral("Second"));
        QCOMPARE(project.timelineData(second)->startTimecode, int64_t(24 * 3600));

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(project.save(path));
        Project loaded;
        QVERIFY(loaded.load(path));
        QCOMPARE(loaded.timeline()->data().startTimecode, int64_t(24 * 3600));
        QCOMPARE(loaded.settings().timelineStart, int64_t(24 * 3600));
    }

    // Spec 25.12: a project saved before format 17 keeps starting at 00:00:00:00, for its
    // timelines and for the ones created after loading it (nothing in it changes).
    void spec25_12_olderProjectsKeepZeroStart()
    {
        Project project;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.kkproj"));
        QVERIFY(project.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(
            root.value(QStringLiteral("formatVersion")).toInt(), serialization::kFormatVersion);
        root[QStringLiteral("formatVersion")] = 16;
        QJsonObject info = root.value(QStringLiteral("project")).toObject();
        QVERIFY(info.contains(QStringLiteral("timelineStart")));
        info.remove(QStringLiteral("timelineStart"));
        info[QStringLiteral("fps")]
            = QJsonObject {{QStringLiteral("num"), 25}, {QStringLiteral("den"), 1}};
        root[QStringLiteral("project")] = info;
        QJsonArray timelines = root.value(QStringLiteral("timelines")).toArray();
        QJsonObject t = timelines[0].toObject();
        QVERIFY(t.contains(QStringLiteral("startTimecode")));
        t.remove(QStringLiteral("startTimecode"));
        t[QStringLiteral("fps")]
            = QJsonObject {{QStringLiteral("num"), 25}, {QStringLiteral("den"), 1}};
        timelines[0] = t;
        root[QStringLiteral("timelines")] = timelines;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();

        Project loaded;
        QVERIFY(loaded.load(path));
        QCOMPARE(loaded.settings().fps, (Rational {25, 1})); // its own rate, not the new default
        QCOMPARE(loaded.timeline()->data().startTimecode, int64_t(0));
        QCOMPARE(loaded.settings().timelineStart, int64_t(0));
        const quint64 created = loaded.createTimeline(QStringLiteral("Later"));
        QCOMPARE(loaded.timelineData(created)->startTimecode, int64_t(0));
        QCOMPARE(
            timecode::formatRecord(0, loaded.timeline()->data()), QStringLiteral("00:00:00:00"));
    }

    // Spec 25.12: the starting timecode is a per-timeline setting (one undo step) that keeps
    // its HH:MM:SS:FF when the project frame rate changes.
    void spec25_12_startingTimecodeSettingAndRateChange()
    {
        Project project;
        const quint64 uid = project.timeline()->data().uid;
        const int before = project.undoStack()->count();
        QVERIFY(project.setTimelineStartTimecode(uid, 24 * 3600 * 10)); // 10:00:00:00
        QCOMPARE(project.undoStack()->count(), before + 1);
        QCOMPARE(
            timecode::formatRecord(0, project.timeline()->data()), QStringLiteral("10:00:00:00"));
        project.undoStack()->undo();
        QCOMPARE(project.timeline()->data().startTimecode, int64_t(24 * 3600));

        ProjectSettings s = project.settings();
        s.fps = Rational {25, 1};
        project.setSettings(s);
        QCOMPARE(project.timeline()->data().startTimecode, int64_t(25 * 3600));
        QCOMPARE(project.settings().timelineStart, int64_t(25 * 3600));
        QCOMPARE(
            timecode::formatRecord(0, project.timeline()->data()), QStringLiteral("01:00:00:00"));
        QCOMPARE(convertTimecode(24 * 3600 + 23, Rational {24, 1}, Rational {25, 1}),
            int64_t(25 * 3600 + 23));
        QCOMPARE(convertTimecode(25 * 3600 + 24, Rational {25, 1}, Rational {24, 1}),
            int64_t(24 * 3600 + 23)); // frame 24 does not exist at 24 fps
    }

    // Spec 25.12: everything that shows or reads a record timecode counts from the start:
    // the viewer, Edit Index, typed timecodes, the EDL and the marker list.
    void spec25_12_displaysAndExportsUseTheStartingTimecode()
    {
        Fixture f;
        TimelineController* controller = f.window->editPage()->timeline()->controller();
        controller->insertSource(f.ids[0], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        f.project.beginEdit(QStringLiteral("marker"));
        Marker m;
        m.frame = 24;
        m.name = QStringLiteral("one");
        f.project.timeline()->mutableData().markers.append(m);
        f.project.endEdit();
        f.project.timeline()->setPlayhead(48);
        QCoreApplication::processEvents();
        const TimelineData& data = f.project.timeline()->data();
        QCOMPARE(data.startTimecode, int64_t(24 * 3600));

        auto* label = f.window->editPage()->timelineViewer()->findChild<QLabel*>(
            QStringLiteral("TimecodeLabel"));
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("01:00:02:00"));
        // The Deliver page's viewer (not bound to the timeline) as well.
        f.action("view.page_deliver")->trigger();
        QCoreApplication::processEvents();
        auto* deliver = f.window->deliverPage()->viewer()->findChild<QLabel*>(
            QStringLiteral("TimecodeLabel"));
        QVERIFY(deliver);
        QCOMPARE(deliver->text(), QStringLiteral("01:00:02:00"));
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();

        f.window->editPage()->editIndex()->rebuildNow();
        EditIndexModel* index = f.window->editPage()->editIndex()->model();
        QCOMPARE(index->index(0, EditIndexModel::EditRecordIn).data().toString(),
            QStringLiteral("01:00:00:00"));
        QCOMPARE(index->index(0, EditIndexModel::EditRecordOut).data().toString(),
            QStringLiteral("01:00:04:00"));

        // Typed timecodes: a full record timecode, or a short one counted from the start.
        controller->enterTimecode(*timecode::parseTyped(QStringLiteral("01000110"), data.fps));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(34));
        controller->enterTimecode(*timecode::parseTyped(QStringLiteral("200"), data.fps));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(48));

        const QString edl = edl::timelineToEdl(data, f.project.mediaPool());
        QVERIFY2(edl.contains(QStringLiteral("01:00:00:00 01:00:04:00")), qPrintable(edl));
        QVERIFY(edl::markersToCsv(data).contains(QStringLiteral("01:00:01:00")));
        QVERIFY(edl::markersToEdl(data).contains(QStringLiteral("* LOC: 01:00:01:00")));
    }

    // Spec 25.13: the timeline frame rate is locked once a timeline has clips.
    void spec25_13_frameRateLockedOnceTimelineHasClips()
    {
        Project project;
        {
            ProjectSettingsDialog empty(&project);
            auto* fps = empty.findChild<QComboBox*>(QStringLiteral("TimelineFrameRate"));
            QVERIFY(fps);
            QVERIFY(fps->isEnabled());
            auto* start = empty.findChild<QLineEdit*>(QStringLiteral("TimelineStartTimecode"));
            QVERIFY(start);
            QCOMPARE(start->text(), QStringLiteral("01:00:00:00"));
        }
        project.beginEdit(QStringLiteral("clip"));
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.duration = 40;
        ops::overwriteClip(project.timeline()->mutableData(), kV1, clip);
        project.endEdit();
        ProjectSettingsDialog dialog(&project);
        auto* fps = dialog.findChild<QComboBox*>(QStringLiteral("TimelineFrameRate"));
        QVERIFY(!fps->isEnabled());
        QVERIFY(!fps->toolTip().isEmpty());
        dialog.apply();
        QCOMPARE(project.settings().fps, (Rational {24, 1}));
    }

    // Spec 24.19: the Video tab has a Composite group after Cropping with Composite Mode
    // (Resolve's list) and Opacity, which moved there from Transform (same stored key); a new
    // mode is one undo step.
    void spec24_19_inspectorCompositeGroup()
    {
        Fixture f;
        f.window->editPage()->timeline()->controller()->insertSource(
            f.ids[0], 0, 4 * kSecondUs, SourceEditMode::Overwrite);
        const quint64 video = f.selection.currentClip();
        QCoreApplication::processEvents();
        InspectorPanel* inspector = f.window->editPage()->inspector();
        auto groupTitle = [](QWidget* w) {
            for (QWidget* p = w; p; p = p->parentWidget()) {
                if (p->objectName() == QLatin1String("InspectorGroup")) {
                    if (auto* t = p->findChild<QLabel*>(QStringLiteral("InspectorGroupTitle")))
                        return t->text();
                }
            }
            return QString();
        };
        auto* mode
            = inspector->findChild<QComboBox*>(QStringLiteral("choice:") + keys::CompositeMode);
        QVERIFY(mode);
        auto* opacity = inspector->findChild<ValueField*>(QStringLiteral("value:") + keys::Opacity);
        QVERIFY(opacity);
        QCOMPARE(groupTitle(mode), QStringLiteral("Composite"));
        QCOMPARE(groupTitle(opacity), QStringLiteral("Composite"));
        QStringList titles;
        for (auto* t : inspector->findChildren<QLabel*>(QStringLiteral("InspectorGroupTitle")))
            titles << t->text();
        QVERIFY2(titles.indexOf(QStringLiteral("Composite"))
                > titles.indexOf(QStringLiteral("Cropping")),
            qPrintable(titles.join(u',')));
        QStringList items;
        for (int i = 0; i < mode->count(); ++i)
            items << mode->itemText(i);
        QCOMPARE(items.mid(0, 9),
            (QStringList {QStringLiteral("Normal"), QStringLiteral("Add"),
                QStringLiteral("Subtract"), QStringLiteral("Difference"),
                QStringLiteral("Multiply"), QStringLiteral("Screen"), QStringLiteral("Overlay"),
                QStringLiteral("Hard Light"), QStringLiteral("Soft Light")}));
        QCOMPARE(mode->currentText(), QStringLiteral("Normal"));

        const int before = f.project.undoStack()->count();
        const int multiply = int(items.indexOf(QStringLiteral("Multiply")));
        mode->setCurrentIndex(multiply);
        emit mode->activated(multiply); // what choosing it in the list does
        QCoreApplication::processEvents();
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        QCOMPARE(ops::clipById(f.project.timeline()->data(), video)
                     ->transform.staticValue(keys::CompositeMode)
                     .toString(),
            blendmodes::Multiply);
        f.project.undoStack()->undo();
        QVERIFY(!ops::clipById(f.project.timeline()->data(), video)
                     ->transform.staticValue(keys::CompositeMode)
                     .isValid());
    }

    // Spec 25.12: a timeline's media pool menu has Timelines > Starting Timecode... and
    // Timeline Settings... (Resolve's place to change the start).
    void spec25_12_mediaPoolTimelineMenuOffersStartingTimecode()
    {
        Fixture f;
        MediaPoolPanel* panel = f.window->editPage()->mediaPool();
        const QString timelineItem
            = f.project.timelineItemId(f.project.timeline()->data().uid);
        QAbstractItemView* view = nullptr;
        for (QAbstractItemView* v : panel->findChildren<QAbstractItemView*>()) {
            if (v->isVisible() && qobject_cast<QListView*>(v) && v->model()
                && v->model()->rowCount() > 1) {
                view = v;
                break;
            }
        }
        QVERIFY(view);
        QModelIndex cell;
        for (int r = 0; r < view->model()->rowCount(); ++r) {
            const QModelIndex i = view->model()->index(r, 0);
            if (i.data(MediaPoolModel::IdRole).toString() == timelineItem)
                cell = i;
        }
        QVERIFY(cell.isValid());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(cell).center());
        QStringList sub;
        QTimer::singleShot(100, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction* a : menu->actions()) {
                if (a->menu() && a->text() == QStringLiteral("Timelines")) {
                    for (QAction* b : a->menu()->actions())
                        sub << b->text();
                }
            }
            menu->close();
        });
        emit view->customContextMenuRequested(view->visualRect(cell).center());
        QTest::qWait(300);
        QCOMPARE(sub, (QStringList {QStringLiteral("Starting Timecode…"),
                          QStringLiteral("Timeline Settings…")}));

        // The dialog sets the start in one undo step.
        const int before = f.project.undoStack()->count();
        QTimer::singleShot(100, [] {
            auto* dialog = QApplication::activeModalWidget();
            if (!dialog)
                return;
            auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("StartingTimecode"));
            edit->setText(QStringLiteral("10:00:00:00"));
            static_cast<QDialog*>(dialog)->accept();
        });
        QVERIFY(timelinedialogs::editStartingTimecode(
            f.window.get(), f.project, f.project.timeline()->data().uid));
        QCOMPARE(f.project.timeline()->data().startTimecode, int64_t(24 * 36000));
        QCOMPARE(f.project.undoStack()->count(), before + 1);
    }
};

QTEST_MAIN(TestBughunt3)
#include "test_bughunt3.moc"
