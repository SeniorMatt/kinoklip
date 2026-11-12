// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the panels, windows and lists of Vim mode (spec section 83): the key context of every
// panel, focus moves between panels by their place, the <C-w> family, pages with gt / gT, and
// the generic list host (motions, search, open, folds, V, the operators) on any list or tree,
// in the Media Pool as well.
#include "app/MainWindow.h"
#include "app/PageBar.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/VimModeLine.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaItem.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelFrame.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimListHost.h"
#include "ui/common/VimOptions.h"
#include "ui/common/VimPicker.h"
#include "ui/common/VimWhichKey.h"
#include "ui/common/VimWindowNav.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/timeline/VimTimelineHost.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

/// Counts the Delete / F2 / Ctrl+V keys a view gets (the list host sends the keys the panel
/// already understands).
class KeyCounter : public QObject
{
public:
    int deletes = 0;
    int renames = 0;
    int pastes = 0;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() != QEvent::KeyPress)
            return false;
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete)
            ++deletes;
        else if (key->key() == Qt::Key_F2)
            ++renames;
        else if (key->key() == Qt::Key_V && key->modifiers() == Qt::ControlModifier)
            ++pastes;
        return false;
    }
};

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
        actions.action(QLatin1String(vimopts::kActionId))->setChecked(true);
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
        project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ClipData v;
            v.kind = ClipKind::Solid;
            v.start = i * 40;
            v.duration = 40;
            v.solidColor = Qt::darkCyan;
            ops::overwriteClip(project.timeline()->mutableData(), kV1, v);
        }
        project.endEdit();
        QCoreApplication::processEvents();
        project.timeline()->setPlayhead(0);
        run(QStringLiteral("page edit"));
        timeline()->setFocus();
    }

    TimelineWidget* timeline() const { return window->editPage()->timeline()->timelineWidget(); }
    VimController* vim() const { return window->vimController(); }
    VimModeLine* modeLine() const { return vim()->modeLine(); }
    QString run(const QString& line) { return vim()->runCommand(line); }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            if (c == QLatin1Char(' '))
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
            else
                QTest::keyClick(QApplication::focusWidget(), c.toLatin1());
        }
    }
    void ctrl(Qt::Key k) { QTest::keyClick(QApplication::focusWidget(), k, Qt::ControlModifier); }
    PanelFrame* focusedFrame() const { return vimwin::frameOf(QApplication::focusWidget()); }
    QString focusedKey() const
    {
        const PanelFrame* f = focusedFrame();
        return f ? f->panelKey() : QString();
    }
    QPoint centre(const PanelFrame* f) const { return f->mapToGlobal(f->rect().center()); }
    PanelFrame* panel(const QString& key) const
    {
        for (PanelFrame* f : vimwin::visiblePanels(window.get())) {
            if (f->panelKey() == key)
                return f;
        }
        return nullptr;
    }
    QAction* page(const char* id) const { return actions.action(QLatin1String(id)); }
};

/// A window with a list and a tree to put the list host through its paces, without the editor.
struct ListFixture
{
    QMainWindow window;
    PanelFrame* frame = nullptr;
    QListWidget* list = nullptr;
    QTreeWidget* tree = nullptr;
    QLineEdit* field = nullptr;
    VimInput input {nullptr};
    VimListHost listHost {nullptr};
    KeyCounter counter;

    /// A host with nothing in it, standing in for the timeline.
    struct NullHost final : VimHost
    {
        bool trigger(const QString&) override { return false; }
        int64_t playhead() const override { return 0; }
        void setPlayhead(int64_t) override { }
        void clearSelection() override { }
        void openCommandLine() override { }
    } nullHost;

    ListFixture()
    {
        input.setHost(&nullHost);
        input.setListHost(&listHost);
        input.setActive(true);
        frame = new PanelFrame(QStringLiteral("Test"));
        auto* body = new QWidget;
        auto* layout = new QVBoxLayout(body);
        field = new QLineEdit;
        frame->setSearchField(field);
        list = new QListWidget;
        for (int i = 1; i <= 30; ++i)
            list->addItem(QStringLiteral("item %1").arg(i));
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree = new QTreeWidget;
        tree->setHeaderHidden(true);
        for (int g = 1; g <= 3; ++g) {
            auto* group = new QTreeWidgetItem(tree, {QStringLiteral("group %1").arg(g)});
            for (int c = 1; c <= 3; ++c)
                new QTreeWidgetItem(group, {QStringLiteral("child %1.%2").arg(g).arg(c)});
        }
        layout->addWidget(field);
        layout->addWidget(list);
        layout->addWidget(tree);
        frame->setContentWidget(body);
        window.setCentralWidget(frame);
        window.resize(500, 700);
        window.show();
        list->installEventFilter(&counter);
        tree->installEventFilter(&counter);
        list->setFocus();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }
    void type(const QString& keys)
    {
        for (const QChar c : keys)
            QTest::keyClick(QApplication::focusWidget(), c.toLatin1());
    }
    int row() const { return list->currentRow(); }
};

} // namespace

class TestVimPanels : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimpanels"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // ---- Contexts ----

    void everyPanelHasAKeyContext()
    {
        Fixture f;
        int checked = 0;
        for (const QString& id : {QStringLiteral("media"), QStringLiteral("cut"),
                 QStringLiteral("edit"), QStringLiteral("comp"), QStringLiteral("color"),
                 QStringLiteral("audio"), QStringLiteral("deliver")}) {
            f.run(QStringLiteral("page ") + id);
            for (PanelFrame* frame : f.window->findChildren<PanelFrame*>()) {
                if (!frame->isVisible())
                    continue;
                QVERIFY2(!VimInput::contextOf(frame).isEmpty(), qPrintable(frame->title()));
                ++checked;
            }
        }
        QVERIFY(checked > 10);
        f.run(QStringLiteral("page edit"));
        QCOMPARE(VimInput::contextOf(f.timeline()), QStringLiteral("timeline"));
        // A list or tree anywhere in a panel is "list".
        auto* pool = f.window->findChild<MediaPoolPanel*>();
        QVERIFY(pool);
        QCOMPARE(VimInput::contextOf(pool->binList()), QStringLiteral("list"));
        // A dialog's list is not.
        QDialog dialog;
        QListWidget inDialog(&dialog);
        QVERIFY(VimInput::contextOf(&inDialog).isEmpty());
    }

    // ---- Windows ----

    void neighbourFollowsThePlaceOnTheScreen()
    {
        // left | top right / bottom right, and a strip along the bottom.
        QWidget host;
        host.resize(1000, 800);
        auto make = [&](const QString& key, QRect r) {
            auto* f = new PanelFrame(key, &host);
            f->setPanelKey(key);
            f->setGeometry(r);
            return f;
        };
        PanelFrame* left = make(QStringLiteral("left"), QRect(0, 0, 300, 600));
        PanelFrame* topRight = make(QStringLiteral("topright"), QRect(310, 0, 690, 250));
        PanelFrame* midRight = make(QStringLiteral("midright"), QRect(310, 260, 690, 340));
        PanelFrame* strip = make(QStringLiteral("strip"), QRect(0, 610, 1000, 190));
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const auto panels = vimwin::visiblePanels(&host);
        QCOMPARE(panels.size(), qsizetype(4));
        using vimwin::Dir;
        QCOMPARE(vimwin::neighbour(left, Dir::Right, panels),
            midRight); // equally near: the bigger overlap wins
        QCOMPARE(vimwin::neighbour(topRight, Dir::Down, panels), midRight);
        QCOMPARE(vimwin::neighbour(midRight, Dir::Down, panels), strip);
        QCOMPARE(vimwin::neighbour(midRight, Dir::Left, panels), left);
        QCOMPARE(vimwin::neighbour(strip, Dir::Up, panels),
            midRight); // equally near: the bigger overlap wins
        QCOMPARE(vimwin::neighbour(topRight, Dir::Up, panels), nullptr);
        QCOMPARE(vimwin::neighbour(left, Dir::Left, panels), nullptr);
        QCOMPARE(vimwin::neighbour(strip, Dir::Down, panels), nullptr);
        // Reading order: top to bottom, then left to right; stepping wraps round.
        QCOMPARE(panels, (QVector<PanelFrame*> {left, topRight, midRight, strip}));
        QCOMPARE(vimwin::step(strip, 1, panels), left);
        QCOMPARE(vimwin::step(left, -1, panels), strip);
        QCOMPARE(vimwin::step(left, 2, panels), midRight);
        QCOMPARE(vimwin::step(nullptr, 1, panels), left);
    }

    void ctrlHjklMoveBetweenPanels()
    {
        Fixture f;
        f.timeline()->setFocus();
        QCOMPARE(f.focusedKey(), QStringLiteral("timeline"));
        PanelFrame* timelineFrame = f.focusedFrame();
        // Up from the timeline: a panel above it, whatever the layout puts there.
        f.ctrl(Qt::Key_K);
        PanelFrame* above = f.focusedFrame();
        QVERIFY(above && above != timelineFrame);
        QVERIFY(f.centre(above).y() < f.centre(timelineFrame).y());
        QVERIFY(above->vimFocusFrame()); // the accent frame follows
        QVERIFY(!timelineFrame->vimFocusFrame());
        // And back down.
        f.ctrl(Qt::Key_J);
        QCOMPARE(f.focusedFrame(), timelineFrame);
        QVERIFY(QApplication::focusWidget() == f.timeline()); // the timeline widget itself
        // Sideways: from the timeline to the left and right.
        f.ctrl(Qt::Key_H);
        PanelFrame* leftOf = f.focusedFrame();
        if (leftOf != timelineFrame) {
            QVERIFY(f.centre(leftOf).x() < f.centre(timelineFrame).x());
            f.ctrl(Qt::Key_L);
            QCOMPARE(f.focusedFrame(), timelineFrame);
        }
        // Nothing beyond the edge: the focus stays.
        for (int i = 0; i < 6; ++i)
            f.ctrl(Qt::Key_J);
        QVERIFY(f.focusedFrame());
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("NORMAL"));
    }

    void ctrlWFamily()
    {
        Fixture f;
        f.timeline()->setFocus();
        PanelFrame* timelineFrame = f.focusedFrame();
        // <C-w>k / <C-w>j are <C-k> / <C-j>.
        f.type(QString());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_K);
        PanelFrame* above = f.focusedFrame();
        QVERIFY(above != timelineFrame);
        // <C-w>p goes back to the one before.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_P);
        QCOMPARE(f.focusedFrame(), timelineFrame);
        // w steps through the panels in reading order and wraps round; W goes back.
        const auto panels = vimwin::visiblePanels(f.window.get());
        QVERIFY(panels.size() >= 4);
        PanelFrame* start = f.focusedFrame();
        const int at = int(panels.indexOf(start));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W);
        QCOMPARE(f.focusedFrame(), panels[(at + 1) % panels.size()]);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), 'W'); // Shift+W
        QCOMPARE(f.focusedFrame(), start);
        for (int i = 0; i < panels.size(); ++i) { // all the way round
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_W);
        }
        QCOMPARE(f.focusedFrame(), start);
        // A count: 2<C-w>w goes to panel 2 (the second in reading order).
        f.type(QStringLiteral("2"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W);
        QCOMPARE(f.focusedFrame(), panels[1]);
    }

    void onlyZoomsAndEqualsBringsBack()
    {
        Fixture f;
        f.timeline()->setFocus();
        const auto before = vimwin::visiblePanels(f.window.get());
        QVERIFY(before.size() >= 4);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
        QCOMPARE(vimwin::visiblePanels(f.window.get()).size(), qsizetype(1));
        QVERIFY(QApplication::focusWidget() == f.timeline());
        // Again: back.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_O);
        QCOMPARE(vimwin::visiblePanels(f.window.get()).size(), before.size());
        // = brings them back too.
        f.run(QStringLiteral("only"));
        QCOMPARE(vimwin::visiblePanels(f.window.get()).size(), qsizetype(1));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Equal);
        QCOMPARE(vimwin::visiblePanels(f.window.get()).size(), before.size());
    }

    void ctrlWCHidesThePanel()
    {
        Fixture f;
        f.run(QStringLiteral("panel media"));
        QCOMPARE(f.focusedKey(), QStringLiteral("media_pool"));
        QAction* toggle = f.actions.action(QStringLiteral("view.toggle_media_pool"));
        QVERIFY(toggle->isChecked());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_W, Qt::ControlModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_C);
        QVERIFY(!toggle->isChecked());
        QVERIFY(!f.panel(QStringLiteral("media_pool")));
        // <leader>wm shows it again and focuses it.
        f.timeline()->setFocus();
        f.type(QStringLiteral(" wm"));
        QVERIFY(toggle->isChecked());
        QCOMPARE(f.focusedKey(), QStringLiteral("media_pool"));
    }

    void leaderWFocusesNamedPanels()
    {
        Fixture f;
        const QVector<QPair<QString, QString>> keys {
            {QStringLiteral("m"), QStringLiteral("media_pool")},
            {QStringLiteral("e"), QStringLiteral("effects")},
            {QStringLiteral("i"), QStringLiteral("inspector")},
            {QStringLiteral("t"), QStringLiteral("timeline")},
            {QStringLiteral("v"), QStringLiteral("viewer")}};
        for (const auto& k : keys) {
            f.timeline()->setFocus();
            f.type(QStringLiteral(" w") + k.first);
            QCOMPARE(f.focusedKey(), k.second);
            QVERIFY(f.focusedFrame()->vimFocusFrame());
        }
        // The which-key list names the keys.
        f.timeline()->setFocus();
        f.type(QStringLiteral(" w"));
        f.vim()->input()->machine().timeoutNow();
        QVERIFY(f.vim()->whichKey()->rows().size() >= 8);
        f.type(QStringLiteral("t"));
        // :panel does the same by name.
        bool ok = true;
        f.vim()->runCommand(QStringLiteral("panel inspector"), &ok);
        QVERIFY(ok);
        QCOMPARE(f.focusedKey(), QStringLiteral("inspector"));
        f.vim()->runCommand(QStringLiteral("panel nonsense"), &ok);
        QVERIFY(!ok);
    }

    // ---- Pages ----

    void gtAndGTChangeThePage()
    {
        Fixture f;
        QVERIFY(f.page("view.page_edit")->isChecked());
        f.type(QStringLiteral("gt"));
        QTRY_VERIFY(f.page("view.page_compositing")->isChecked()); // after Edit comes Compositing
        // The focus lands in a panel of the new page, and the keys go on working there.
        QTRY_VERIFY(f.focusedFrame() != nullptr);
        f.type(QStringLiteral("gT"));
        QTRY_VERIFY(f.page("view.page_edit")->isChecked());
        // {n}gt goes to page n (1 Media ... 7 Deliver).
        QTRY_VERIFY(f.focusedFrame() != nullptr);
        f.type(QStringLiteral("7gt"));
        QTRY_VERIFY(f.page("view.page_deliver")->isChecked());
        QTRY_VERIFY(f.focusedFrame() != nullptr);
        // gt from the last page wraps round to the first; gT from the first to the last.
        f.type(QStringLiteral("gt"));
        QTRY_VERIFY(f.page("view.page_media")->isChecked());
        QTRY_VERIFY(f.focusedFrame() != nullptr);
        f.type(QStringLiteral("gT"));
        QTRY_VERIFY(f.page("view.page_deliver")->isChecked());
        // {n}gT goes n pages back.
        QTRY_VERIFY(f.focusedFrame() != nullptr);
        f.type(QStringLiteral("3gT"));
        QTRY_VERIFY(f.page("view.page_compositing")->isChecked()); // 7 - 3 = page 4
    }

    void pageKeysAreNotTakenInATextField()
    {
        Fixture f;
        auto* pool = f.window->findChild<MediaPoolPanel*>();
        QVERIFY(pool);
        PanelFrame* frame = nullptr;
        for (PanelFrame* p : vimwin::visiblePanels(f.window.get())) {
            if (p->searchField() && p->searchField()->isVisible())
                frame = p;
        }
        QVERIFY(frame);
        frame->searchField()->setFocus();
        QTest::keyClicks(frame->searchField(), QStringLiteral("gtgt"));
        QCOMPARE(frame->searchField()->text(), QStringLiteral("gtgt"));
        QVERIFY(f.page("view.page_edit")->isChecked());
        frame->searchField()->clear();
    }

    // ---- Real panels ----

    void mediaPoolBinsAndClipsWorkWithoutCodeOfTheirOwn()
    {
        Fixture f;
        MediaPool* pool = f.project.mediaPool();
        QStringList ids;
        for (const char* name : {"alpha.mp4", "bravo.mp4", "charlie.mp4"}) {
            MediaItem item;
            item.name = QLatin1String(name);
            item.path = QStringLiteral("/nonexistent/") + QLatin1String(name);
            ids.append(pool->addItem(item));
        }
        f.run(QStringLiteral("page media"));
        QTRY_VERIFY(f.page("view.page_media")->isChecked());
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        auto* panel = qobject_cast<MediaPoolPanel*>(f.focusedFrame());
        QVERIFY(panel);
        auto* view = qobject_cast<QAbstractItemView*>(QApplication::focusWidget());
        QVERIFY(view);
        QCOMPARE(VimInput::contextOf(view), QStringLiteral("list"));
        // The rows: the timeline, then the three clips.
        QCOMPARE(f.vim()->listHost()->rowText(0), QStringLiteral("Timeline 1"));
        QCOMPARE(f.vim()->listHost()->rowText(1), QStringLiteral("alpha.mp4"));
        // Rows: the first, the last, and one in the middle.
        f.type(QStringLiteral("gg"));
        QCOMPARE(view->currentIndex().row(), 0);
        f.type(QStringLiteral("j"));
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[0]}));
        f.type(QStringLiteral("j"));
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[1]}));
        f.type(QStringLiteral("G"));
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[2]}));
        f.type(QStringLiteral("2G"));
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[0]})); // row 2
        // V grows a selection over rows.
        f.type(QStringLiteral("Vj"));
        QCOMPARE(panel->selectedMediaIds().size(), 2);
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("V-LINE"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("NORMAL"));
        // o loads the clip in the Source viewer.
        QSignalSpy opened(panel, &MediaPoolPanel::openInSourceViewer);
        f.type(QStringLiteral("2G"));
        f.type(QStringLiteral("o"));
        QCOMPARE(opened.count(), 1);
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media"))); // the viewer took the focus
        // dd deletes the row (nothing in the timeline uses it, so no question).
        f.type(QStringLiteral("2G"));
        f.type(QStringLiteral("dd"));
        QTRY_VERIFY(!pool->item(ids[0]));
        QVERIFY(pool->item(ids[1]) && pool->item(ids[2]));
        // a makes a bin, -  goes up a level.
        QCOMPARE(int(pool->bins().size()), 0);
        f.type(QStringLiteral("a"));
        QCOMPARE(int(pool->bins().size()), 1);
        // The new bin is being renamed (the panel opens its editor): Enter keeps the name.
        auto* editor = QApplication::focusWidget();
        QVERIFY(editor && VimInput::isTextField(editor));
        QTest::keyClick(editor, Qt::Key_Return);
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        const QString bin = pool->bins().first().id;
        panel->setCurrentBin(bin);
        QCOMPARE(panel->currentBin(), bin);
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        f.type(QStringLiteral("-"));
        QTRY_COMPARE(panel->currentBin(), QString());
    }

    void searchFieldFiltersAndKeepsTheKeys()
    {
        Fixture f;
        MediaPool* pool = f.project.mediaPool();
        for (const char* name : {"alpha.mp4", "bravo.mp4", "charlie.mp4"}) {
            MediaItem item;
            item.name = QLatin1String(name);
            item.path = QStringLiteral("/nonexistent/") + QLatin1String(name);
            pool->addItem(item);
        }
        f.run(QStringLiteral("page media"));
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        PanelFrame* frame = f.focusedFrame();
        QLineEdit* field = frame->searchField();
        QVERIFY(field);
        QWidget* view = QApplication::focusWidget();
        f.type(QStringLiteral("/"));
        QVERIFY(QApplication::focusWidget() == field);
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("INSERT")); // a text field
        QTest::keyClicks(field, QStringLiteral("a"));
        QCOMPARE(field->text(), QStringLiteral("a"));
        // Ctrl+J / Ctrl+K move the list's selection; Return gives the list the focus.
        QTest::keyClick(field, Qt::Key_J, Qt::ControlModifier);
        QTest::keyClick(field, Qt::Key_K, Qt::ControlModifier);
        QTest::keyClick(field, Qt::Key_Return);
        QTRY_VERIFY(QApplication::focusWidget() == view);
        QCOMPARE(field->text(), QStringLiteral("a")); // the filter stays
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("NORMAL"));
        // n and N walk the filtered rows.
        f.type(QStringLiteral("gg"));
        f.type(QStringLiteral("n"));
        f.type(QStringLiteral("N"));
        // Esc in the field goes back to the list.
        f.type(QStringLiteral("/"));
        QVERIFY(QApplication::focusWidget() == field);
        QTest::keyClick(field, Qt::Key_Escape);
        QTRY_VERIFY(QApplication::focusWidget() == view);
        field->clear();
    }

    void ctrlJKStepsThroughAListFromTheSearchField()
    {
        Fixture f;
        MediaPool* pool = f.project.mediaPool();
        QStringList ids;
        for (const char* name : {"alpha.mp4", "bravo.mp4", "charlie.mp4"}) {
            MediaItem item;
            item.name = QLatin1String(name);
            item.path = QStringLiteral("/nonexistent/") + QLatin1String(name);
            ids.append(pool->addItem(item));
        }
        f.run(QStringLiteral("page media"));
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        auto* panel = qobject_cast<MediaPoolPanel*>(f.focusedFrame());
        QVERIFY(panel);
        f.type(QStringLiteral("gg"));
        f.type(QStringLiteral("j")); // the first clip (row 0 is the timeline)
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[0]}));
        QLineEdit* field = panel->searchField();
        field->setFocus();
        QTest::keyClick(field, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[1]}));
        QTest::keyClick(field, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[2]}));
        QTest::keyClick(field, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(panel->selectedMediaIds(), (QStringList {ids[1]}));
    }

    void globalKeysWorkInEveryPanel()
    {
        Fixture f;
        // The Inspector has fields and buttons, no list: the "form" context (global keys and
        // the form's own).
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("inspector")));
        QCOMPARE(f.focusedKey(), QStringLiteral("inspector"));
        QCOMPARE(VimInput::contextOf(QApplication::focusWidget()), QStringLiteral("form"));
        f.type(QStringLiteral("j")); // no clip, no rows: nothing happens
        QCOMPARE(f.focusedKey(), QStringLiteral("inspector"));
        // : opens the line, a picker opens, windows move, pages change.
        f.type(QStringLiteral(":"));
        QVERIFY(f.vim()->commandLine()->isVisible());
        QTest::keyClick(f.vim()->commandLine(), Qt::Key_Escape);
        QVERIFY(QApplication::focusWidget() != f.vim()->commandLine());
        QCOMPARE(f.focusedKey(), QStringLiteral("inspector")); // the focus comes back
        f.type(QStringLiteral(" fc"));
        QTRY_VERIFY(f.vim()->picker()->isOpen());
        QTest::keyClick(f.vim()->picker()->filterField(), Qt::Key_Escape);
        f.ctrl(Qt::Key_H);
        QVERIFY(f.focusedKey() != QStringLiteral("inspector"));
        f.ctrl(Qt::Key_L);
        f.type(QStringLiteral("gt"));
        QTRY_VERIFY(f.page("view.page_compositing")->isChecked());
    }

    void theHostFollowsTheFocusBetweenTimelineAndList()
    {
        Fixture f;
        MediaPool* pool = f.project.mediaPool();
        MediaItem item;
        item.name = QStringLiteral("alpha.mp4");
        item.path = QStringLiteral("/nonexistent/alpha.mp4");
        pool->addItem(item);
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        // A half-typed count in the list is dropped when the focus goes to the timeline.
        f.type(QStringLiteral("3"));
        QCOMPARE(f.modeLine()->pendingText(), QStringLiteral("3"));
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("timeline")));
        QTRY_COMPARE(f.modeLine()->pendingText(), QString());
        // The timeline's keys work again: dd removes the clip, 5l moves five frames.
        f.type(QStringLiteral("5l"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(5));
        f.type(QStringLiteral("dd"));
        QCOMPARE(int(f.project.timeline()->data().videoTracks.front().clips.size()), 2);
        // And back in the list: j is a row, not a track.
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        f.type(QStringLiteral("gg"));
        const int track = f.vim()->host()->cursorTrack();
        f.type(QStringLiteral("j"));
        QCOMPARE(f.vim()->host()->cursorTrack(), track);
    }

    void listModeIsShownInTheModeLine()
    {
        Fixture f;
        QVERIFY(f.vim()->focusPanelKey(QStringLiteral("media")));
        QCOMPARE(f.modeLine()->panelText(), QStringLiteral("media pool"));
        f.type(QStringLiteral("V"));
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("V-LINE"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QCOMPARE(f.modeLine()->modeText(), QStringLiteral("NORMAL"));
        f.type(QStringLiteral("2"));
        QCOMPARE(f.modeLine()->pendingText(), QStringLiteral("2"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QCOMPARE(f.modeLine()->pendingText(), QString());
    }

    // ---- The list host on any list or tree ----

    void rowMotions()
    {
        ListFixture f;
        QCOMPARE(VimInput::contextOf(f.list), QStringLiteral("list"));
        f.list->setCurrentRow(0);
        f.type(QStringLiteral("j"));
        QCOMPARE(f.row(), 1);
        f.type(QStringLiteral("3j"));
        QCOMPARE(f.row(), 4);
        f.type(QStringLiteral("k"));
        QCOMPARE(f.row(), 3);
        f.type(QStringLiteral("10k"));
        QCOMPARE(f.row(), 0); // stops at the first row
        f.type(QStringLiteral("G"));
        QCOMPARE(f.row(), 29);
        f.type(QStringLiteral("gg"));
        QCOMPARE(f.row(), 0);
        f.type(QStringLiteral("5G"));
        QCOMPARE(f.row(), 4); // row 5
        f.type(QStringLiteral("99G"));
        QCOMPARE(f.row(), 29); // past the end: the last row
        f.type(QStringLiteral("gg"));
        QTest::keyClick(f.list, Qt::Key_D, Qt::ControlModifier);
        const int half = f.row();
        QVERIFY(half > 0 && half < 29);
        QTest::keyClick(f.list, Qt::Key_D, Qt::ControlModifier);
        QVERIFY(f.row() > half);
        QTest::keyClick(f.list, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(f.row(), half);
        // Arrow keys still reach the view.
        QTest::keyClick(f.list, Qt::Key_Down);
        QCOMPARE(f.row(), half + 1);
        // The mode line machine says NORMAL; unbound keys do nothing.
        QCOMPARE(f.input.machine().displayName(), QStringLiteral("NORMAL"));
        f.type(QStringLiteral("q"));
        QCOMPARE(f.row(), half + 1);
    }

    void visualLineSelectsRowsAndOperatorsSendTheViewsKeys()
    {
        ListFixture f;
        f.list->setCurrentRow(2);
        f.type(QStringLiteral("V"));
        QCOMPARE(f.input.machine().displayName(), QStringLiteral("V-LINE"));
        f.type(QStringLiteral("jj"));
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 3);
        QCOMPARE(f.row(), 4);
        f.type(QStringLiteral("k")); // shrinks
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 2);
        f.type(QStringLiteral("d"));
        QCOMPARE(f.counter.deletes, 1);
        QCOMPARE(
            f.list->selectionModel()->selectedRows().size(), 2); // the view decides what to delete
        QCOMPARE(f.input.machine().displayName(), QStringLiteral("NORMAL"));
        // dd works on this row, 3dd on three, dj on two (linewise), x on the selection or the row.
        f.list->setCurrentRow(10);
        f.type(QStringLiteral("dd"));
        QCOMPARE(f.counter.deletes, 2);
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 1);
        f.type(QStringLiteral("3dd"));
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 3);
        f.list->setCurrentRow(10);
        f.type(QStringLiteral("dj"));
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 2);
        f.list->setCurrentRow(10);
        f.type(QStringLiteral("x"));
        QCOMPARE(f.counter.deletes, 5);
        // V then Esc leaves and keeps one row.
        f.list->setCurrentRow(3);
        f.type(QStringLiteral("Vjj"));
        QTest::keyClick(f.list, Qt::Key_Escape);
        QCOMPARE(f.input.machine().displayName(), QStringLiteral("NORMAL"));
        QCOMPARE(f.list->selectionModel()->selectedRows().size(), 1);
    }

    void yankCopiesTheNamesAndPPastes()
    {
        ListFixture f;
        f.list->setCurrentRow(4);
        f.type(QStringLiteral("yy"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("item 5"));
        f.type(QStringLiteral("2yy"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("item 5\nitem 6"));
        f.list->setCurrentRow(7);
        f.type(QStringLiteral("Vjy"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("item 8\nitem 9"));
        QCOMPARE(f.counter.deletes, 0);
        f.type(QStringLiteral("p"));
        QCOMPARE(f.counter.pastes, 1); // the panel's own Ctrl+V
        // r renames: the view's edit key.
        f.type(QStringLiteral("r"));
        QCOMPARE(f.counter.renames, 1);
    }

    void searchWalksTheRowsAndFilterFieldTakesTheFocus()
    {
        ListFixture f;
        // The frame has a search field: / goes there (the panel filters; n N walk the rows).
        f.list->setCurrentRow(0);
        f.type(QStringLiteral("/"));
        // The line of the window is not here (no controller); the host asks for the field.
        QVERIFY(f.listHost.search(QStringLiteral("item 2")));
        QCOMPARE(f.row(), 1); // the first match after the cursor: "item 2"
        f.listHost.searchStep(true, 1);
        QCOMPARE(f.row(), 19); // "item 20" (the matches are item 2, item 20 ... item 29)
        f.type(QStringLiteral("n"));
        QCOMPARE(f.row(), 20);
        f.type(QStringLiteral("N"));
        QCOMPARE(f.row(), 19);
        f.type(QStringLiteral("2N")); // back past the first match: wraps round to the last
        QCOMPARE(f.row(), 28);
        f.type(QStringLiteral("N"));
        QCOMPARE(f.row(), 27);
        QVERIFY(!f.listHost.search(QStringLiteral("zzz")));
        // Without a pattern n and N are the next and previous row.
        f.listHost.clearSearch();
        f.list->setCurrentRow(5);
        f.type(QStringLiteral("n"));
        QCOMPARE(f.row(), 6);
        f.type(QStringLiteral("N"));
        QCOMPARE(f.row(), 5);
    }

    void treeOpenCloseAndFolds()
    {
        ListFixture f;
        f.tree->setFocus();
        f.tree->setCurrentItem(f.tree->topLevelItem(0));
        QVERIFY(!f.tree->topLevelItem(0)->isExpanded());
        // l opens a closed branch, then j walks into it; h closes or goes to the parent.
        f.type(QStringLiteral("l"));
        QVERIFY(f.tree->topLevelItem(0)->isExpanded());
        f.type(QStringLiteral("jj"));
        QCOMPARE(f.tree->currentItem(), f.tree->topLevelItem(0)->child(1));
        f.type(QStringLiteral("h"));
        QCOMPARE(f.tree->currentItem(), f.tree->topLevelItem(0)); // a leaf: to the parent row
        f.type(QStringLiteral("h"));
        QVERIFY(!f.tree->topLevelItem(0)->isExpanded()); // an open branch: closes
        // Rows are the visible ones: the closed branches are skipped by j.
        f.type(QStringLiteral("j"));
        QCOMPARE(f.tree->currentItem(), f.tree->topLevelItem(1));
        f.type(QStringLiteral("zR"));
        for (int i = 0; i < 3; ++i)
            QVERIFY(f.tree->topLevelItem(i)->isExpanded());
        f.type(QStringLiteral("G"));
        QCOMPARE(f.tree->currentItem(), f.tree->topLevelItem(2)->child(2));
        f.type(QStringLiteral("zM"));
        for (int i = 0; i < 3; ++i)
            QVERIFY(!f.tree->topLevelItem(i)->isExpanded());
        f.tree->setCurrentItem(f.tree->topLevelItem(1));
        f.type(QStringLiteral("za"));
        QVERIFY(f.tree->topLevelItem(1)->isExpanded());
        f.type(QStringLiteral("za"));
        QVERIFY(!f.tree->topLevelItem(1)->isExpanded());
        f.type(QStringLiteral("zo"));
        QVERIFY(f.tree->topLevelItem(1)->isExpanded());
        f.type(QStringLiteral("zc"));
        QVERIFY(!f.tree->topLevelItem(1)->isExpanded());
        // A child's - goes to the parent row.
        f.tree->topLevelItem(2)->setExpanded(true);
        f.tree->setCurrentItem(f.tree->topLevelItem(2)->child(1));
        f.type(QStringLiteral("-"));
        QCOMPARE(f.tree->currentItem(), f.tree->topLevelItem(2));
    }

    void openSendsTheViewsOpenSignal()
    {
        ListFixture f;
        // A view nobody listens to "activated" on gets a double click (what the panels use).
        QSignalSpy doubled(f.list, &QListWidget::itemDoubleClicked);
        f.list->setCurrentRow(3);
        f.type(QStringLiteral("o"));
        QCOMPARE(doubled.count(), 1);
        f.type(QStringLiteral("l"));
        QCOMPARE(doubled.count(), 2);
        // A view that listens for "activated" gets that.
        QSignalSpy activated(f.list, &QListWidget::itemActivated);
        f.type(QStringLiteral("o"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(doubled.count(), 2);
    }

    void dotRepeatsAListChange()
    {
        ListFixture f;
        f.list->setCurrentRow(5);
        f.type(QStringLiteral("dd"));
        QCOMPARE(f.counter.deletes, 1);
        f.type(QStringLiteral("."));
        QCOMPARE(f.counter.deletes, 2);
    }

    void dialogsKeepTheirKeysAndCtrlJKMoveInLists()
    {
        Fixture f;
        QDialog dialog;
        auto* layout = new QVBoxLayout(&dialog);
        auto* filter = new QLineEdit;
        auto* list = new QListWidget;
        for (int i = 0; i < 5; ++i)
            list->addItem(QStringLiteral("row %1").arg(i));
        layout->addWidget(filter);
        layout->addWidget(list);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        list->setFocus();
        list->setCurrentRow(0);
        // Plain Vim keys are not taken in a dialog.
        QTest::keyClick(list, Qt::Key_J);
        QCOMPARE(list->currentRow(), 0);
        QTest::keyClick(list, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClick(list, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(list->currentRow(), 0);
        // From the filter field above the only list.
        filter->setFocus();
        QTest::keyClick(filter, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClick(filter, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->currentRow(), 2);
        QTest::keyClick(filter, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClicks(filter, QStringLiteral("jj"));
        QCOMPARE(filter->text(), QStringLiteral("jj"));
    }
};

QTEST_MAIN(TestVimPanels)
#include "test_vimpanels.moc"
