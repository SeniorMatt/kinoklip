// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime colour-scheme / widget-style switching in the real main window: no crash (run it
// under ASan), and afterwards every widget and every piece of chrome (menu bar, interface
// bar, page bar, panel title bars, timeline, inspector) uses the new palette. Regression
// test for alpha.1: switching Dark -> System / Light / Classic sometimes segfaulted and left
// the menu bar and top bars dark while the panels turned light.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    SettingsPageRegistry settingsPages;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    App()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        registerStandardSettingsPages(settingsPages);
        theme::registerSettingsPage(settingsPages, actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        playback = std::make_unique<PlaybackEngine>(std::move(options));
        playback->setMediaPool(project.mediaPool());
        playback->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.settingsPages = &settingsPages;
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
    }
};

bool darkColor(const QColor& c)
{
    return c.lightness() < 128;
}

/// Lightness class of the pixel in the middle of @p w (grabbed from the window image).
bool widgetLooksDark(const QImage& shot, const QWidget* window, const QWidget* w)
{
    // Sample a pixel near the left/top corner, away from text and buttons in the middle.
    const QPoint local(qMin(4, w->width() - 1), w->height() / 2);
    const QPoint p = w->mapTo(window, local) * shot.devicePixelRatio();
    return darkColor(shot.pixelColor(p));
}

} // namespace

class TestThemeSwitch : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_themeswitch"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void switchSchemesInMainWindow()
    {
        App app;
        QString why;
        if (test::ensureFixtures(&why)) {
            const ImportResult r = app.project.importMedia({test::fixture("h264_25fps.mp4")});
            QCOMPARE(r.importedIds.size(), 1);
        }
        app.window->resize(1400, 900);
        app.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(app.window.get()));

        // Open every menu once (menus get polished/cached), like a user browsing them.
        for (QAction* a : app.window->menuBar()->actions()) {
            if (QMenu* m = a->menu()) {
                m->popup(QPoint(10, 10));
                QCoreApplication::processEvents();
                m->hide();
            }
        }

        QStringList ids;
        for (const theme::ColorSchemeInfo& s : theme::availableColorSchemes())
            ids.append(s.id);
        QVERIFY(ids.contains(QStringLiteral("BreezeLight")));
        // Dark -> each scheme -> Dark, several rounds (the crash was intermittent).
        constexpr int kRounds = 3;
        for (int round = 0; round < kRounds; ++round) {
            for (const QString& id : std::as_const(ids)) {
                for (const QString& step : {id, QStringLiteral("BreezeDark")}) {
                    QAction* action = app.actions.action(theme::colorSchemeActionId(step));
                    QVERIFY2(action, qPrintable(step));
                    action->trigger();
                    QCoreApplication::processEvents();
                    QTest::qWait(1);
                    verifyConsistent(app, step);
                    if (QTest::currentTestFailed())
                        return;
                }
            }
        }
    }

    void switchStylesKeepsScheme()
    {
        App app;
        app.window->resize(1200, 800);
        app.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(app.window.get()));
        app.actions.action(theme::colorSchemeActionId(QStringLiteral("BreezeLight")))->trigger();
        for (const QString& style : theme::availableStyles()) {
            app.actions.action(theme::styleActionId(style))->trigger();
            QCoreApplication::processEvents();
            verifyConsistent(app, QStringLiteral("BreezeLight"));
            if (QTest::currentTestFailed())
                return;
        }
        app.actions.action(theme::styleActionId(theme::availableStyles().first()))->trigger();
        app.actions.action(theme::colorSchemeActionId(QStringLiteral("BreezeDark")))->trigger();
        QCoreApplication::processEvents();
        verifyConsistent(app, QStringLiteral("BreezeDark"));
    }

private:
    /// Every widget without an own palette follows the application palette, and the chrome
    /// actually paints in the scheme's lightness.
    void verifyConsistent(App& app, const QString& scheme)
    {
        const QPalette appPalette = QApplication::palette();
        const QColor window = appPalette.color(QPalette::Window);
        const bool dark = darkColor(window);
        int checked = 0;
        for (QWidget* w : app.window->findChildren<QWidget*>()) {
            if (w->testAttribute(Qt::WA_SetPalette))
                continue; // deliberately own palette (warnings, viewer surrounds)
            const QColor got = w->palette().color(QPalette::Window);
            QVERIFY2(darkColor(got) == dark,
                qPrintable(QStringLiteral("%1 (%2 '%3'): window colour %4, application %5")
                               .arg(scheme, QString::fromLatin1(w->metaObject()->className()),
                                   w->objectName(), got.name(), window.name())));
            ++checked;
        }
        QVERIFY(checked > 50);

        const QImage shot = app.window->grab().toImage();
        // Optional: one screenshot per scheme for a visual check (first round only).
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            const QString file
                = QStringLiteral("%1/themeswitch-%2.png")
                      .arg(dir, scheme.isEmpty() ? QStringLiteral("system") : scheme);
            if (!QFileInfo::exists(file))
                shot.save(file);
        }
        const QStringList chrome = {QStringLiteral("InterfaceBar"), QStringLiteral("PageBar"),
            QStringLiteral("PanelTitleBar")};
        for (const QString& name : chrome) {
            for (QWidget* w : app.window->findChildren<QWidget*>(name)) {
                if (!w->isVisible() || w->width() < 8)
                    continue;
                QVERIFY2(widgetLooksDark(shot, app.window.get(), w) == dark,
                    qPrintable(
                        QStringLiteral("%1: %2 paints %3 on a %4 scheme")
                            .arg(scheme, name, dark ? "light" : "dark", dark ? "dark" : "light")));
            }
        }
        QMenuBar* bar = app.window->menuBar();
        QVERIFY2(widgetLooksDark(shot, app.window.get(), bar) == dark,
            qPrintable(scheme + QStringLiteral(": menu bar does not follow the scheme")));
    }
};

QTEST_MAIN(TestThemeSwitch)
#include "test_themeswitch.moc"
