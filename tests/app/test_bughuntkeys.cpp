// SPDX-License-Identifier: GPL-3.0-or-later
// Bug-hunting round on 1.0.0-beta.1, keys in the real main window: which panel a key acts on
// when the focus is outside the timeline.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAction>
#include <QApplication>
#include <QItemSelectionModel>
#include <QListView>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    /// The media pool's shown item view.
    QAbstractItemView* poolView() const
    {
        for (QListView* v : window->editPage()->mediaPool()->findChildren<QListView*>()) {
            if (v->isVisible() && v->model() && v->model()->rowCount() > 0)
                return v;
        }
        return nullptr;
    }

    /// Selects the media pool row showing item @p id and gives the view the focus.
    bool selectInPool(const QString& id)
    {
        QAbstractItemView* view = poolView();
        if (!view)
            return false;
        QAbstractItemModel* m = view->model();
        for (int r = 0; r < m->rowCount(view->rootIndex()); ++r) {
            const QModelIndex i = m->index(r, 0, view->rootIndex());
            if (i.data(MediaPoolModel::IdRole).toString() == id) {
                view->setFocus(Qt::MouseFocusReason);
                view->selectionModel()->select(i, QItemSelectionModel::ClearAndSelect);
                view->selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
                QCoreApplication::processEvents();
                return view->hasFocus();
            }
        }
        return false;
    }
};

QString addFakeMedia(Project& project, const QString& name)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/") + name;
    item.name = name;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.durationUs = 4'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    return project.mediaPool()->addItem(item);
}

} // namespace

class TestBughuntKeys : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughuntkeys"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // Spec 33.31: with the media pool focused, Delete / Backspace delete the selected media
    // pool item and leave the timeline alone (they used to ripple-delete / lift the clips
    // still selected on the timeline); Ctrl+A selects the bin's items, not the timeline's.
    void spec33_31_mediaPoolKeysActOnTheMediaPool_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("mods");
        QTest::newRow("Delete") << int(Qt::Key_Delete) << int(Qt::NoModifier);
        QTest::newRow("Backspace") << int(Qt::Key_Backspace) << int(Qt::NoModifier);
        QTest::newRow("Shift+Backspace") << int(Qt::Key_Backspace) << int(Qt::ShiftModifier);
    }
    void spec33_31_mediaPoolKeysActOnTheMediaPool()
    {
        QFETCH(int, key);
        QFETCH(int, mods);
        Fixture f;
        f.project.beginEdit(QStringLiteral("setup"));
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.start = 0;
        clip.duration = 40;
        clip.solidColor = Qt::darkCyan;
        const quint64 id = ops::overwriteClip(f.project.timeline()->mutableData(), kV1, clip);
        clip.start = 40;
        ops::overwriteClip(f.project.timeline()->mutableData(), kV1, clip);
        f.project.endEdit();
        const QString keep = addFakeMedia(f.project, QStringLiteral("keep.mp4"));
        const QString unused = addFakeMedia(f.project, QStringLiteral("unused.mp4"));
        QCoreApplication::processEvents();
        f.selection.setClips({id}); // still selected on the timeline
        QVERIFY(f.selectInPool(unused));

        QTest::keyClick(QApplication::focusWidget(), Qt::Key(key), Qt::KeyboardModifiers(mods));
        QVERIFY(!f.project.mediaPool()->item(unused));
        QVERIFY(f.project.mediaPool()->item(keep));
        const auto& clips = f.project.timeline()->data().track(kV1)->clips;
        QCOMPARE(int(clips.size()), 2);
        QCOMPARE(clips[0].id, id);
    }

    void spec33_31_ctrlASelectsTheMediaPool()
    {
        Fixture f;
        f.project.beginEdit(QStringLiteral("setup"));
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.duration = 40;
        ops::overwriteClip(f.project.timeline()->mutableData(), kV1, clip);
        f.project.endEdit();
        const QString a = addFakeMedia(f.project, QStringLiteral("a.mp4"));
        addFakeMedia(f.project, QStringLiteral("b.mp4"));
        QCoreApplication::processEvents();
        QVERIFY(f.selectInPool(a));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QVERIFY(f.selection.clips().isEmpty()); // the timeline's clips stay unselected
        QVERIFY(f.window->editPage()->mediaPool()->selectedMediaIds().size() >= 2);
    }

    // Spec 10.11: with a list or tree panel focused (here the Effects library), Delete,
    // Backspace, Ctrl+A and the arrow keys act on the list, not on the timeline: they used to
    // ripple-delete the clips selected on the timeline, select every clip or step the playhead.
    // Space still plays the timeline from there.
    void spec10_11_listPanelKeepsEditingKeys()
    {
        Fixture f;
        f.project.beginEdit(QStringLiteral("setup"));
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.duration = 40;
        clip.solidColor = Qt::darkCyan;
        const quint64 id = ops::overwriteClip(f.project.timeline()->mutableData(), kV1, clip);
        clip.start = 40;
        ops::overwriteClip(f.project.timeline()->mutableData(), kV1, clip);
        f.project.endEdit();
        f.project.timeline()->setPlayhead(10);
        QCoreApplication::processEvents();
        EffectsLibraryPanel* fx = f.window->editPage()->effectsLibrary();
        if (!fx->isVisible())
            f.actions.action(QStringLiteral("view.toggle_effects"))->trigger();
        QVERIFY(fx->isVisible());
        QTreeWidget* tree = fx->tree();
        tree->setFocus(Qt::MouseFocusReason);
        QCoreApplication::processEvents();
        QVERIFY(tree->hasFocus());
        tree->setCurrentItem(tree->topLevelItem(0));
        f.selection.setClips({id});

        for (const Qt::Key key : {Qt::Key_Delete, Qt::Key_Backspace})
            QTest::keyClick(tree, key);
        QCOMPARE(int(f.project.timeline()->data().track(kV1)->clips.size()), 2);
        QTest::keyClick(tree, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(f.selection.clips(), QVector<quint64> {id});
        QTest::keyClick(tree, Qt::Key_Right);
        QTest::keyClick(tree, Qt::Key_Left);
        const QTreeWidgetItem* before = tree->currentItem();
        QTest::keyClick(tree, Qt::Key_Down);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(10));
        QVERIFY(tree->currentItem() != before); // Down moved the list's selection

        // Transport keys still reach the window: Space plays and stops the timeline.
        QAction* play = f.actions.action(QStringLiteral("transport.play_pause"));
        QVERIFY(play);
        QSignalSpy played(play, &QAction::triggered);
        QTest::keyClick(tree, Qt::Key_Space);
        QCOMPARE(played.count(), 1);
        QTest::keyClick(tree, Qt::Key_Space);

        // With the timeline focused again the keys edit the timeline.
        tree->clearFocus();
        QCoreApplication::processEvents();
        QTest::keyClick(f.window.get(), Qt::Key_Delete);
        QCOMPARE(int(f.project.timeline()->data().track(kV1)->clips.size()), 1);
    }
};

QTEST_MAIN(TestBughuntKeys)
#include "test_bughuntkeys.moc"
