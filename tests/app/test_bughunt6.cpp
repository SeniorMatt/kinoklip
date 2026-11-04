// SPDX-License-Identifier: GPL-3.0-or-later
// Sixth bug-hunting round, in the real main window where it matters: one test per fix, named
// after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CompositingPage.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, with @p clipCount solid clips of 50 frames on V1.
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
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt6 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt6"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 42b.31: the node editor's keys work right after switching to the Compositing page
    // with the keyboard (Shift+5, then 1 views the selected node), without clicking into the
    // page first: the node editor takes the focus when the page shows.
    void spec42b_31_pageKeysWorkAfterSwitchingPages()
    {
        Fixture f;
        f.show();
        f.project.timeline()->setPlayhead(10);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QWidget* edit = f.window->findChild<TimelineWidget*>();
        QVERIFY(edit);
        edit->setFocus();
        QCoreApplication::processEvents();
        f.action("view.page_compositing")->trigger();
        QTest::qWait(20);
        CompositingPage* page = f.window->compositingPage();
        QWidget* focus = QApplication::focusWidget();
        QVERIFY(focus);
        QCOMPARE(focus, static_cast<QWidget*>(page->nodeEditor()));
        CompSession* s = page->session();
        QVERIFY(s->clipId());
        QVERIFY(!s->composition().nodes.isEmpty());
        const quint64 node = s->composition().nodes.front().id;
        s->setSelection({node});
        QTest::keyClick(focus, Qt::Key_2);
        QCoreApplication::processEvents();
        QCOMPARE(s->viewerNode(1), node);
    }
};

QTEST_MAIN(TestBughunt6)
#include "test_bughunt6.moc"
