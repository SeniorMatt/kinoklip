// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the spring-loaded page buttons (docs/behavior-spec.md 71): a drag resting
// on a page button for kSpringLoadDelayMs switches to that page, leaving early cancels, the
// current page's button does nothing, a drop on a button switches at once and imports nothing.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/SpringLoad.h"
#include "ui/common/UiAnimation.h"

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QToolButton>
#include <QUrl>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPos = QPointF;
#else
using DragPos = QPoint;
#endif

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
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->activateWindow();
    }

    QToolButton* button(const char* actionId) const
    {
        QAction* a = actions.action(QLatin1String(actionId));
        for (QToolButton* b : window->findChildren<QToolButton*>())
            if (b->defaultAction() == a)
                return b;
        return nullptr;
    }
};

qint64 g_now = 0;

void advance(int ms)
{
    g_now += ms;
    QTest::qWait(60); // lets the 16 ms tick run
}

} // namespace

class TestPageSpring : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_pagespring"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        uianim::setClockForTesting([] { return g_now; });
    }
    void cleanupTestCase() { uianim::setClockForTesting({}); }

    void hoverSwitchesPage()
    {
        Fixture f;
        f.actions.action(QStringLiteral("view.page_edit"))->trigger();
        QToolButton* color = f.button("view.page_color");
        QVERIFY(color);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/none.mp4"))});
        QDragEnterEvent enter(DragPos(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, {});
        enter.setAccepted(false);
        QCoreApplication::sendEvent(color, &enter);
        QVERIFY(enter.isAccepted()); // no forbidden cursor
        auto* spring = color->findChild<SpringLoad*>();
        QVERIFY(spring && spring->active());
        advance(kSpringLoadDelayMs / 2);
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        QVERIFY(spring->active());
        advance(kSpringLoadDelayMs / 2 + 50);
        QCOMPARE(f.window->currentPageId(), QStringLiteral("color"));
        QVERIFY(!spring->active());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(color, &leave);
    }
    void leavingEarlyCancels()
    {
        Fixture f;
        f.actions.action(QStringLiteral("view.page_edit"))->trigger();
        QToolButton* audio = f.button("view.page_audio");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/none.mp4"))});
        QDragEnterEvent enter(DragPos(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(audio, &enter);
        advance(kSpringLoadDelayMs / 2);
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(audio, &leave);
        QVERIFY(!audio->findChild<SpringLoad*>()->active());
        advance(kSpringLoadDelayMs * 2);
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
    }
    void currentPageButtonDoesNothing()
    {
        Fixture f;
        f.actions.action(QStringLiteral("view.page_edit"))->trigger();
        QToolButton* edit = f.button("view.page_edit");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/none.mp4"))});
        QDragEnterEvent enter(DragPos(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, {});
        enter.setAccepted(false);
        QCoreApplication::sendEvent(edit, &enter);
        QVERIFY(enter.isAccepted());
        QVERIFY(!edit->findChild<SpringLoad*>()->active());
        advance(kSpringLoadDelayMs * 2);
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(edit, &leave);
    }
    void dropSwitchesAtOnceAndImportsNothing()
    {
        Fixture f;
        f.actions.action(QStringLiteral("view.page_edit"))->trigger();
        QToolButton* deliver = f.button("view.page_deliver");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/none.mp4"))});
        QDragEnterEvent enter(DragPos(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(deliver, &enter);
        QDropEvent drop(DragPos(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, {});
        drop.setAccepted(true);
        QCoreApplication::sendEvent(deliver, &drop);
        QVERIFY(!drop.isAccepted());
        QCOMPARE(f.window->currentPageId(), QStringLiteral("deliver"));
        QCOMPARE(f.project.mediaPool()->items().size(), 0);
    }
};

QTEST_MAIN(TestPageSpring)
#include "test_pagespring.moc"
