// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen smoke test of the main window: pages, panel toggles, undo wiring, title, autosave.
#include "app/AutosaveController.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "app/pages/MediaPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaBrowserPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, for one test.
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
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
};

/// The Edit page's top row: left column | source | timeline viewer | right column.
QSplitter* topRowOf(EditPage* page)
{
    for (QSplitter* s : page->findChildren<QSplitter*>())
        if (s->orientation() == Qt::Horizontal && s->count() == 4)
            return s;
    return nullptr;
}

/// True if all Inspector tabs (Video, Audio, Effects, Title, Transition) fit without the tab
/// bar's scroll arrows.
bool inspectorTabsFit(EditPage* page)
{
    const QTabBar* bar = page->inspector()->tabs()->tabBar();
    return bar->count() == 5 && bar->sizeHint().width() <= bar->width();
}

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mainwindow"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void pagesSwitch()
    {
        Fixture f;
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        f.action("view.page_media")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("media"));
        QVERIFY(f.window->setCurrentPage(QStringLiteral("deliver")));
        QVERIFY(f.action("view.page_deliver")->isChecked());
        // The Color page (spec 44b) is built: Shift+6 opens it.
        QVERIFY(f.action("view.page_color")->isEnabled());
        f.action("view.page_color")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("color"));
    }

    void panelTogglesFollowPage()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("edit"));
        EditPage* edit = f.window->editPage();
        QAction* effects = f.action("view.toggle_effects");
        QVERIFY(effects->isEnabled());
        QVERIFY(!effects->isChecked());
        effects->trigger();
        QVERIFY(edit->isPanelVisible(PanelId::Effects));
        QVERIFY(effects->isChecked());

        f.action("view.toggle_inspector")->trigger(); // hide
        QVERIFY(!edit->isPanelVisible(PanelId::Inspector));

        f.window->setCurrentPage(QStringLiteral("deliver"));
        QVERIFY(!effects->isEnabled());
        f.window->setCurrentPage(QStringLiteral("edit"));
        QVERIFY(effects->isChecked());
        QVERIFY(!f.action("view.toggle_inspector")->isChecked());

        f.action("view.single_viewer")->trigger();
        QVERIFY(edit->isSingleViewer());
    }

    /// Alpha.2 feedback #5 (spec 24.4 - 24.6): a viewer-mode button at the right of both viewer
    /// headers, in step with View > Single Viewer Mode; single mode shows the timeline, or the
    /// source clip once one is opened; Q swaps; the choice persists.
    void spec24_4_singleViewerToggle()
    {
        {
            Fixture f;
            f.window->resize(1600, 900);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            EditPage* edit = f.window->editPage();
            QAction* single = f.action("view.single_viewer");
            QVERIFY(single->isCheckable());
            QCOMPARE(edit->viewerModeButtons().size(), qsizetype(2));
            QToolButton* button = edit->viewerModeButtons().at(1); // timeline viewer header
            QVERIFY(button->isVisible());
            QVERIFY(button->toolTip().contains(QLatin1String("Single Viewer")));
            const int viewerWidth = edit->sourceViewer()->width() + edit->timelineViewer()->width();
            button->click();
            QVERIFY(edit->isSingleViewer());
            QVERIFY(single->isChecked());
            QVERIFY(edit->sourceViewer()->isHidden());
            QVERIFY(!edit->timelineViewer()->isHidden());
            QVERIFY(button->toolTip().contains(QLatin1String("Dual Viewer")));
            QCoreApplication::processEvents();
            QVERIFY(edit->timelineViewer()->width() >= viewerWidth - 10);
            // The header button sits right of the timecode, inside the panel.
            const QPoint br = button->mapTo(edit->timelineViewer(), button->rect().topRight());
            QVERIFY(br.x() < edit->timelineViewer()->width());

            // Opening a clip shows the source viewer in place of the timeline viewer.
            MediaItem item;
            item.path = QStringLiteral("/nonexistent/clip.mp4");
            item.name = QStringLiteral("clip.mp4");
            item.info.valid = true;
            item.info.hasVideo = true;
            item.info.durationUs = 10'000'000;
            item.info.width = 640;
            item.info.height = 360;
            const QString id = f.project.mediaPool()->addItem(item);
            emit edit->mediaPool()->openInSourceViewer(id);
            QVERIFY(edit->isSingleViewer());
            QVERIFY(edit->singleViewerShowsSource());
            QVERIFY(edit->timelineViewer()->isHidden());
            // Q swaps back to the timeline.
            f.action("view.toggle_source_timeline")->trigger();
            QVERIFY(!edit->singleViewerShowsSource());
            QVERIFY(!edit->timelineViewer()->isHidden());
            // The menu entry leaves single mode again; both viewers come back.
            single->trigger();
            QVERIFY(!edit->isSingleViewer());
            QVERIFY(!edit->sourceViewer()->isHidden());
            QVERIFY(!edit->timelineViewer()->isHidden());
            single->trigger(); // single again, then persist
            f.window->close();
        }
        Fixture g;
        QVERIFY(g.window->editPage()->isSingleViewer());
        QVERIFY(g.action("view.single_viewer")->isChecked());
        QSettings().clear();
    }

    void layoutPersists()
    {
        {
            Fixture f;
            f.action("view.toggle_mixer")->trigger();
            f.window->close(); // not dirty → no prompt; saves layout
        }
        Fixture g;
        QVERIFY(g.window->editPage()->isPanelVisible(PanelId::Mixer));
        QSettings().clear();
    }

    void sideColumnsKeepDefaultShare()
    {
        // The window starts small and is then enlarged (maximised): the media pool / effects
        // column and the inspector column keep their default share instead of only the
        // viewers growing (owner feedback on 0.3.2-alpha.1).
        QSettings().clear();
        const auto topRow = [](EditPage* page) -> QSplitter* {
            for (QSplitter* s : page->findChildren<QSplitter*>())
                if (s->orientation() == Qt::Horizontal && s->count() == 4)
                    return s;
            return nullptr;
        };
        const auto shares = [&](EditPage* page) {
            QSplitter* row = topRow(page);
            const double w = row->width();
            return std::pair {row->sizes().first() / w, row->sizes().last() / w};
        };
        {
            Fixture f;
            f.window->resize(1000, 700);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            f.window->resize(1920, 1080);
            QTRY_VERIFY(topRow(f.window->editPage())->width() > 1500);
            QTRY_VERIFY(shares(f.window->editPage()).first > 0.21);
            const auto [left, right] = shares(f.window->editPage());
            QVERIFY2(left > 0.21 && left < 0.27, qPrintable(QString::number(left)));
            QVERIFY2(right > 0.22 && right < 0.28, qPrintable(QString::number(right)));

            // A layout the user made is stored and kept.
            QSplitter* row = topRow(f.window->editPage());
            const int w = row->width();
            row->setSizes({w / 8, w * 3 / 8, w * 3 / 8, w / 8});
            emit row->splitterMoved(w / 8, 1);
            f.window->close();
        }
        {
            Fixture g;
            g.window->resize(1920, 1080);
            g.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(g.window.get()));
            QTRY_VERIFY(shares(g.window->editPage()).first < 0.17);
            g.window->close();
        }
        // A layout stored by version 1 (squeezed columns) is replaced by the defaults once.
        QSettings().setValue(QStringLiteral("pages/edit/layoutVersion"), 1);
        {
            Fixture h;
            h.window->resize(1920, 1080);
            h.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
            QTRY_VERIFY(shares(h.window->editPage()).first > 0.21);
            h.window->close();
        }
        QSettings().clear();
    }

    void keyframesDockInLeftColumn()
    {
        // Resolve 20: the Keyframes toggle is on the left of the interface bar and the docked
        // panel shares the left column with the Media Pool (spec 15.21).
        QSettings().clear();
        Fixture f;
        f.window->resize(1920, 1080);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        EditPage* edit = f.window->editPage();
        f.action("view.toggle_keyframes")->trigger();
        QVERIFY(edit->isPanelVisible(PanelId::Keyframes));
        auto* keyframes = edit->findChild<KeyframesPanel*>();
        auto* pool = edit->findChild<MediaPoolPanel*>();
        QVERIFY(keyframes && pool);
        auto* column = qobject_cast<QSplitter*>(keyframes->parentWidget());
        QVERIFY(column);
        QCOMPARE(column->orientation(), Qt::Vertical);
        QCOMPARE(column->indexOf(pool) >= 0, true);
        QTRY_VERIFY(keyframes->isVisible() && keyframes->height() >= 160);
        // The column is wide enough for the curve editor: about a third of the width.
        QTRY_VERIFY(column->width() > f.window->width() * 0.30);
        f.action("view.toggle_keyframes")->trigger();
        QVERIFY(!keyframes->isVisible());
        f.window->close();
        QSettings().clear();
    }

    /// Owner report on 1.1.4 (spec 24.23): after a start in single viewer mode the side columns
    /// were squeezed (Media Pool ~320 px, Inspector ~190 px with its tabs behind scroll arrows).
    /// Single mode keeps the side columns of dual mode, also after a restart.
    void spec24_23_singleViewerKeepsSideColumns_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("1869") << 1869;
        QTest::newRow("1920") << 1920;
    }

    void spec24_23_singleViewerKeepsSideColumns()
    {
        QFETCH(int, width);
        QSettings().clear();
        int dualLeft = 0;
        int dualRight = 0;
        const auto checkColumns = [&](EditPage* edit, const char* when) {
            QSplitter* row = topRowOf(edit);
            const QList<int> sizes = row->sizes();
            const QString what = QStringLiteral("%1: left %2 right %3, dual %4 / %5")
                                     .arg(QLatin1String(when))
                                     .arg(sizes.first())
                                     .arg(sizes.last())
                                     .arg(dualLeft)
                                     .arg(dualRight);
            QVERIFY2(std::abs(sizes.first() - dualLeft) <= 3, qPrintable(what));
            QVERIFY2(std::abs(sizes.last() - dualRight) <= 3, qPrintable(what));
            QVERIFY2(inspectorTabsFit(edit), when);
        };
        {
            Fixture f;
            f.window->resize(width, 1080);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            EditPage* edit = f.window->editPage();
            QTRY_VERIFY(topRowOf(edit)->width() > width - 40);
            dualLeft = topRowOf(edit)->sizes().first();
            dualRight = topRowOf(edit)->sizes().last();
            const double w = topRowOf(edit)->width();
            // Below 1920 px the columns give way (spec 24.26): (1869 - 1065) * 480 / 940 = 410 px is 21.9 %.
            QVERIFY2(dualLeft / w > 0.21 && dualRight / w > 0.215, qPrintable(QString::number(dualLeft)));
            QVERIFY(inspectorTabsFit(edit));
            f.action("view.single_viewer")->trigger();
            QCoreApplication::processEvents();
            checkColumns(edit, "single");
            f.window->close();
        }
        // The restart in single mode: the stored mode is restored before the first layout.
        {
            Fixture g;
            g.window->resize(width, 1080);
            g.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(g.window.get()));
            EditPage* edit = g.window->editPage();
            QVERIFY(edit->isSingleViewer());
            QTRY_VERIFY(topRowOf(edit)->width() > width - 40);
            checkColumns(edit, "single after restart");
            g.action("view.single_viewer")->trigger();
            QCoreApplication::processEvents();
            checkColumns(edit, "dual after restart");
            g.window->close();
        }
        QSettings().clear();
    }

    /// Owner request on 1.1.4 (spec 24.24): every panel size is remembered: across a restart,
    /// a page switch, single / dual viewer and hiding and showing panels.
    void spec24_24_panelSizesPersist()
    {
        QSettings().clear();
        struct Sizes
        {
            QList<int> top, main, left, right, bottom, pool, effects, browser;
        };
        const auto splitters = [](MainWindow* window) {
            EditPage* edit = window->editPage();
            return QList<QSplitter*> {topRowOf(edit),
                edit->findChild<QSplitter*>(QString(), Qt::FindDirectChildrenOnly),
                qobject_cast<QSplitter*>(edit->mediaPool()->parentWidget()),
                qobject_cast<QSplitter*>(edit->inspector()->parentWidget()),
                qobject_cast<QSplitter*>(edit->timeline()->parentWidget()),
                edit->mediaPool()->findChild<QSplitter*>(),
                edit->effectsLibrary()->findChild<QSplitter*>(),
                window->findChild<MediaPage*>()->browser()->findChild<QSplitter*>()};
        };
        const auto near = [](const QList<int>& a, const QList<int>& b) {
            if (a.size() != b.size())
                return false;
            for (qsizetype i = 0; i < a.size(); ++i)
                if (std::abs(a[i] - b[i]) > 2)
                    return false;
            return true;
        };
        const auto dump = [](const QList<int>& a, const QList<int>& b) {
            QString s;
            for (int v : a)
                s += QString::number(v) + QLatin1Char(' ');
            s += QLatin1String("vs ");
            for (int v : b)
                s += QString::number(v) + QLatin1Char(' ');
            return s;
        };
        QList<QList<int>> dragged;
        const auto compareAll = [&](MainWindow* window, const char* when) {
            const QList<QSplitter*> now = splitters(window);
            for (qsizetype i = 0; i < now.size(); ++i) {
                if (i == now.size() - 1 && !now[i]->isVisible())
                    continue; // the Media page's browser: compared while that page is shown
                QVERIFY2(near(now[i]->sizes(), dragged[i]),
                    qPrintable(QStringLiteral("%1, splitter %2: %3")
                            .arg(QLatin1String(when))
                            .arg(i)
                            .arg(dump(now[i]->sizes(), dragged[i]))));
            }
        };
        {
            Fixture f;
            f.window->resize(1920, 1080);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            EditPage* edit = f.window->editPage();
            for (const char* id : {"view.toggle_effects", "view.toggle_metadata", "view.toggle_mixer"})
                f.action(id)->trigger();
            edit->mediaPool()->setBinsVisible(true);
            QTRY_VERIFY(topRowOf(edit)->width() > 1800);
            f.window->setCurrentPage(QStringLiteral("media"));
            QCoreApplication::processEvents();
            f.window->setCurrentPage(QStringLiteral("edit"));
            QCoreApplication::processEvents();
            // "Drag" every splitter: new sizes, then the signal a handle drag sends.
            QList<QSplitter*> all = splitters(f.window.get());
            const auto drag = [](QSplitter* s, const QList<int>& shares) {
                const int total = s->orientation() == Qt::Horizontal ? s->width() : s->height();
                int sum = 0;
                for (int v : shares)
                    sum += v;
                QList<int> sizes;
                for (int v : shares)
                    sizes.append(total * v / sum);
                s->setSizes(sizes);
                emit s->splitterMoved(sizes.first(), 1);
            };
            drag(all[0], {3, 4, 6, 3});
            drag(all[1], {3, 2});
            drag(all[2], {1, 2, 1, 1}); // media pool, effects (edit index, keyframes hidden)
            drag(all[3], {2, 1});
            drag(all[4], {4, 1});
            drag(all[5], {1, 1});
            drag(all[6], {1, 2});
            for (QSplitter* s : all.mid(0, 7))
                dragged.append(s->sizes());
            f.window->setCurrentPage(QStringLiteral("media"));
            QTRY_VERIFY(all[7]->isVisible());
            drag(all[7], {1, 3});
            dragged.append(all[7]->sizes());
            // Stored shortly after the drag, without quitting.
            QTRY_VERIFY(QSettings().contains(QStringLiteral("pages/media/browserPanel/splitter0")));
            f.window->setCurrentPage(QStringLiteral("edit"));
            QCoreApplication::processEvents();
            compareAll(f.window.get(), "page switch");

            // Single viewer: the side columns stay; dual again: the viewers' split comes back.
            f.action("view.single_viewer")->trigger();
            QCoreApplication::processEvents();
            const QList<int> single = all[0]->sizes();
            QVERIFY2(near({single.first(), single.last()}, {dragged[0].first(), dragged[0].last()}),
                qPrintable(dump(single, dragged[0])));
            f.action("view.single_viewer")->trigger();
            QCoreApplication::processEvents();
            compareAll(f.window.get(), "viewer mode toggle");

            // Hiding and showing panels (a whole column, a panel of a column) keeps the sizes.
            const QList<QStringList> toggles {{QStringLiteral("view.toggle_inspector")},
                {QStringLiteral("view.toggle_metadata")}, {QStringLiteral("view.toggle_mixer")},
                {QStringLiteral("view.toggle_effects")},
                {QStringLiteral("view.toggle_inspector"), QStringLiteral("view.toggle_metadata")},
                {QStringLiteral("view.toggle_media_pool"), QStringLiteral("view.toggle_effects")}};
            for (const QStringList& ids : toggles) {
                for (int pass = 0; pass < 2; ++pass) { // hide, show
                    for (const QString& id : ids)
                        f.actions.action(id)->trigger();
                    QCoreApplication::processEvents();
                }
                compareAll(f.window.get(), qPrintable(ids.join(QLatin1Char('+'))));
            }
            f.window->close();
        }
        {
            Fixture g;
            g.window->resize(1920, 1080);
            g.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(g.window.get()));
            QTRY_VERIFY(topRowOf(g.window->editPage())->width() > 1800);
            QCoreApplication::processEvents();
            compareAll(g.window.get(), "restart");
            g.window->setCurrentPage(QStringLiteral("media"));
            QCoreApplication::processEvents();
            QVERIFY(near(splitters(g.window.get())[7]->sizes(), dragged[7]));
            g.window->setCurrentPage(QStringLiteral("edit"));
            g.action("view.single_viewer")->trigger();
            g.window->close();
        }
        // A restart in single mode keeps the dragged columns; dual gets its viewer split back.
        {
            Fixture h;
            h.window->resize(1920, 1080);
            h.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(h.window.get()));
            QSplitter* row = topRowOf(h.window->editPage());
            QTRY_VERIFY(row->width() > 1800);
            QVERIFY(h.window->editPage()->isSingleViewer());
            const QList<int> single = row->sizes();
            QVERIFY2(near({single.first(), single.last()}, {dragged[0].first(), dragged[0].last()}),
                qPrintable(dump(single, dragged[0])));
            h.action("view.single_viewer")->trigger();
            QCoreApplication::processEvents();
            QVERIFY2(near(row->sizes(), dragged[0]), qPrintable(dump(row->sizes(), dragged[0])));
            h.window->close();
        }
        QSettings().clear();
    }

    /// Spec 24.24 on every page: each splitter on screen, moved by a drag, comes back after a
    /// restart.
    void spec24_24_everyPageKeepsItsSplitters()
    {
        QSettings().clear();
        const QStringList pages {QStringLiteral("media"), QStringLiteral("cut"),
            QStringLiteral("edit"), QStringLiteral("compositing"), QStringLiteral("color"),
            QStringLiteral("audio"), QStringLiteral("deliver")};
        const auto pageSplitters = [](MainWindow* window, const QString& id) {
            for (Page* page : window->findChildren<Page*>())
                if (page->pageId() == id)
                    return page->findChildren<QSplitter*>();
            return QList<QSplitter*> {};
        };
        QHash<QString, QList<QList<int>>> dragged;
        {
            Fixture f;
            f.window->resize(1920, 1080);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            for (const QString& id : pages) {
                QVERIFY(f.window->setCurrentPage(id));
                QTest::qWait(50);
                int moved = 0;
                for (QSplitter* splitter : pageSplitters(f.window.get(), id)) {
                    QList<int> sizes = splitter->sizes();
                    // Shrink the largest pane for the next visible one.
                    const auto largest = std::max_element(sizes.begin(), sizes.end());
                    const qsizetype at = largest - sizes.begin();
                    qsizetype other = -1;
                    for (qsizetype i = 0; i < sizes.size(); ++i)
                        if (i != at && sizes[i] > 0 && (other < 0 || sizes[i] < sizes[other]))
                            other = i;
                    if (!splitter->isVisible() || other < 0)
                        continue;
                    const int step = std::min(40, sizes[at] / 10);
                    sizes[at] -= step;
                    sizes[other] += step;
                    splitter->setSizes(sizes);
                    emit splitter->splitterMoved(sizes[0], 1);
                    ++moved;
                }
                QVERIFY2(moved > 0, qPrintable(id));
                QTest::qWait(50);
                QList<QList<int>> now;
                for (QSplitter* splitter : pageSplitters(f.window.get(), id))
                    now.append(splitter->isVisible() ? splitter->sizes() : QList<int> {});
                dragged.insert(id, now);
            }
            f.window->close();
        }
        Fixture g;
        g.window->resize(1920, 1080);
        g.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(g.window.get()));
        for (const QString& id : pages) {
            QVERIFY(g.window->setCurrentPage(id));
            QTest::qWait(50);
            const QList<QSplitter*> splitters = pageSplitters(g.window.get(), id);
            const QList<QList<int>>& expected = dragged.value(id);
            QCOMPARE(splitters.size(), expected.size());
            for (qsizetype i = 0; i < splitters.size(); ++i) {
                if (expected[i].isEmpty() || !splitters[i]->isVisible())
                    continue;
                const QList<int> sizes = splitters[i]->sizes();
                QString text = id + QStringLiteral(" splitter %1:").arg(i);
                bool same = sizes.size() == expected[i].size();
                for (qsizetype k = 0; same && k < sizes.size(); ++k)
                    same = std::abs(sizes[k] - expected[i][k]) <= 2;
                for (qsizetype k = 0; k < sizes.size(); ++k)
                    text += QStringLiteral(" %1/%2").arg(sizes[k]).arg(expected[i].value(k));
                QVERIFY2(same, qPrintable(text));
            }
        }
        g.window->close();
        QSettings().clear();
    }

    /// Spec 24.23: a layout stored by 1.1.4 (version 3) may hold the squeezed columns; it is
    /// replaced by the default widths once, then the user's widths are kept again.
    void spec24_23_oldLayoutResetOnce()
    {
        QSettings().clear();
        const auto leftShare = [](MainWindow* window) {
            QSplitter* row = topRowOf(window->editPage());
            return row->sizes().first() / double(row->width());
        };
        const auto open = [](Fixture& f) {
            f.window->resize(1869, 1080);
            f.window->show();
            return QTest::qWaitForWindowExposed(f.window.get());
        };
        {
            Fixture f;
            QVERIFY(open(f));
            QSplitter* row = topRowOf(f.window->editPage());
            QTRY_VERIFY(row->width() > 1800);
            const int w = row->width();
            row->setSizes({w / 6, w / 3, w / 3, w / 10}); // squeezed
            emit row->splitterMoved(w / 6, 1);
            f.action("view.single_viewer")->trigger();
            f.window->close();
        }
        QSettings().setValue(QStringLiteral("pages/edit/layoutVersion"), 3);
        {
            Fixture g;
            QVERIFY(open(g));
            QVERIFY(g.window->editPage()->isSingleViewer());
            QTRY_VERIFY(topRowOf(g.window->editPage())->width() > 1800);
            QTRY_VERIFY2(leftShare(g.window.get()) > 0.21,
                qPrintable(QString::number(leftShare(g.window.get()))));
            QVERIFY(inspectorTabsFit(g.window->editPage()));
            QSplitter* row = topRowOf(g.window->editPage());
            const int w = row->width();
            row->setSizes({w / 6, 0, w - w / 6 - w / 4, w / 4});
            emit row->splitterMoved(w / 6, 1);
            g.window->close();
        }
        {
            Fixture h;
            QVERIFY(open(h));
            QTRY_VERIFY(topRowOf(h.window->editPage())->width() > 1800);
            QCoreApplication::processEvents();
            const double share = leftShare(h.window.get());
            QVERIFY2(share > 0.15 && share < 0.18, qPrintable(QString::number(share)));
            h.window->close();
        }
        QSettings().clear();
    }

    /// Spec 24.25: Workspace > Reset UI Layout brings back the default panels and sizes, which
    /// follow window resizes again.
    void spec24_25_resetUiLayout()
    {
        QSettings().clear();
        Fixture f;
        f.window->resize(1920, 1080);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        EditPage* edit = f.window->editPage();
        QSplitter* row = topRowOf(edit);
        QTRY_VERIFY(row->width() > 1800);
        const QList<int> defaults = row->sizes();
        auto* rows = edit->findChild<QSplitter*>(QString(), Qt::FindDirectChildrenOnly);
        const QList<int> defaultHeights = rows->sizes();
        for (const char* id : {"view.toggle_effects", "view.toggle_mixer", "view.single_viewer"})
            f.action(id)->trigger();
        const int w = row->width();
        row->setSizes({w / 8, 0, w - w / 4, w / 8});
        emit row->splitterMoved(w / 8, 1);
        rows->setSizes({200, rows->height() - 200});
        emit rows->splitterMoved(200, 1);
        QAction* reset = f.action("view.reset_ui_layout");
        QVERIFY(reset);
        reset->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!edit->isSingleViewer());
        QVERIFY(!f.action("view.single_viewer")->isChecked());
        QVERIFY(!edit->isPanelVisible(PanelId::Effects));
        QVERIFY(!edit->isPanelVisible(PanelId::Mixer));
        QVERIFY(!f.action("view.toggle_mixer")->isChecked());
        QVERIFY(edit->isPanelVisible(PanelId::MediaPool));
        QVERIFY(edit->isPanelVisible(PanelId::Inspector));
        for (qsizetype i = 0; i < defaults.size(); ++i)
            QVERIFY2(std::abs(row->sizes()[i] - defaults[i]) <= 3,
                qPrintable(QString::number(row->sizes()[i])));
        QVERIFY(std::abs(rows->sizes().first() - defaultHeights.first()) <= 3);
        // The default widths follow a resize again (larger: below about 1920 px the viewers'
        // minimum widths take from the side columns).
        f.window->resize(2400, 1200);
        QTRY_VERIFY(row->width() > 2300);
        QTRY_VERIFY2(row->sizes().first() / double(row->width()) > 0.21,
            qPrintable(QStringLiteral("%1 of %2, last %3")
                    .arg(row->sizes().first())
                    .arg(row->width())
                    .arg(row->sizes().last())));
        f.window->close();
        QSettings().clear();
    }

    void undoActionsAndTitle()
    {
        Fixture f;
        QAction* undo = f.action("edit.undo");
        QVERIFY(!undo->isEnabled());
        f.project.beginEdit(QStringLiteral("Add Clip"));
        ClipData clip;
        clip.duration = 10;
        editops::overwriteClip(f.project.timeline()->mutableData(), TrackRef {}, clip);
        f.project.endEdit();
        QVERIFY(undo->isEnabled());
        QVERIFY(undo->text().contains(QLatin1String("Add Clip")));
        QVERIFY(f.window->isWindowModified());
        undo->trigger();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(0));
        QVERIFY(!f.window->isWindowModified());
        QVERIFY(f.action("edit.redo")->isEnabled());
        f.project.setName(QStringLiteral("Trip"));
        QVERIFY(f.window->windowTitle().startsWith(QLatin1String("Trip")));
    }

    void undoClosesAnEditLeftOpen()
    {
        // Owner report on 0.4.0, "Ctrl+Z doesn't work": a gesture whose mouse release never
        // arrived left an edit open, and every undo was refused afterwards.
        Fixture f;
        f.project.beginEdit(QStringLiteral("Add Clip"));
        ClipData clip;
        clip.duration = 10;
        editops::overwriteClip(f.project.timeline()->mutableData(), TrackRef {}, clip);
        f.project.endEdit();
        f.project.beginEdit(QStringLiteral("Lost Drag"));
        QTest::ignoreMessage(QtWarningMsg, "closed an edit left open: Lost Drag");
        f.action("edit.undo")->trigger();
        QVERIFY(!f.project.isEditing());
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(0));
    }

    void findInMediaPool()
    {
        // Alt+F: the selected timeline clip's media is selected in the media pool, in its bin.
        Fixture f;
        f.project.beginEdit(QStringLiteral("Setup"));
        const QString bin = f.project.mediaPool()->addBin(QStringLiteral("Footage"));
        MediaItem item;
        item.path = QCoreApplication::applicationFilePath();
        item.binId = bin;
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.durationUs = 4'000'000;
        const QString id = f.project.mediaPool()->addItem(item);
        ClipData clip;
        clip.mediaId = id;
        clip.duration = 25;
        const quint64 clipId
            = editops::overwriteClip(f.project.timeline()->mutableData(), TrackRef {}, clip);
        f.project.endEdit();
        MediaPoolPanel* pool = f.window->editPage()->mediaPool();
        QVERIFY(!f.window->editPage()->findInMediaPool() || pool->currentMediaId() == id);
        f.selection.setClips({clipId});
        QVERIFY(f.window->editPage()->findInMediaPool());
        QCOMPARE(pool->currentBin(), bin);
        QCOMPARE(pool->currentMediaId(), id);
        QVERIFY(f.actions.contains(QStringLiteral("timeline.find_in_media_pool")));
        QCOMPARE(f.actions.shortcuts(QStringLiteral("timeline.find_in_media_pool")).value(0),
            QKeySequence(QStringLiteral("Alt+F")));
    }

    void autosaveWritesCopy()
    {
        Project project;
        AutosaveController autosave(&project);
        QVERIFY(autosave.autosaveNow()); // nothing to do
        QVERIFY(!QFileInfo::exists(autosave.autosavePath()));
        project.setName(QStringLiteral("Auto/Save:Test"));
        project.beginEdit(QStringLiteral("x"));
        ClipData clip;
        clip.duration = 5;
        editops::overwriteClip(project.timeline()->mutableData(), TrackRef {}, clip);
        project.endEdit();
        QVERIFY(autosave.autosaveNow());
        QVERIFY(QFileInfo::exists(autosave.autosavePath()));
        QVERIFY(!autosave.autosavePath().contains(QLatin1Char(':')));
        QVERIFY(project.isDirty()); // autosave never clears the modified flag
        QFile::remove(autosave.autosavePath());
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
