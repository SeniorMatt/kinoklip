// SPDX-License-Identifier: GPL-3.0-or-later
// Application entry point: graphics setup, application objects, command-line modes, main window.
#include "app/AppInfo.h"
#include "app/CommandLine.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/Screenshot.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "render/RenderFrameCommand.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QApplication>
#include <QSurfaceFormat>

#include <cstdio>

namespace {

/// Must run before QApplication exists. The compositor and viewers render with OpenGL 3.3 core;
/// sharing contexts lets the render thread's textures be shown by the viewer widgets without
/// copies (and keeps QOpenGLWidgets in different windows/pages compatible).
void configureOpenGL()
{
    constexpr int kGlMajor = 3;
    constexpr int kGlMinor = 3;
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(kGlMajor, kGlMinor);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1); // vsync: smooth playback without tearing
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}

} // namespace

int main(int argc, char** argv)
{
    configureOpenGL();
    kk::theme::initBeforeApplication(); // bundled Breeze icons; must precede QApplication
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QLatin1String(kk::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(kk::appinfo::name));
    QCoreApplication::setApplicationVersion(QLatin1String(kk::appinfo::version));
    // Matches packaging/linux/org.kinoklip.Kinoklip.desktop: Wayland app_id / X11 WM_CLASS,
    // so the compositor shows the right icon and groups windows with the desktop entry.
    QGuiApplication::setDesktopFileName(QLatin1String(kk::appinfo::desktopId));
    kk::initResources();

    kk::CommandLineOptions options;
    QString error;
    if (!kk::parseCommandLine(QCoreApplication::arguments(), options, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    if (options.isRenderFrame())
        return kk::runRenderFrame(
            options.renderFrameProject, options.renderFrameIndex, options.renderFramePath);

    kk::theme::setSessionOverrides(options.style, options.colorScheme);
    kk::theme::apply(app);

    // Application-wide objects, handed to every widget through AppContext (no singletons).
    kk::Project project;
    kk::SelectionModel selection;
    kk::ActionRegistry actions;
    actions.loadDefaultKeymap();
    kk::registerStandardActions(actions);
    kk::theme::registerActions(actions); // Settings > Color Theme / Style entries
    kk::SettingsPageRegistry settingsPages;
    kk::registerStandardSettingsPages(settingsPages);
    kk::theme::registerSettingsPage(settingsPages, actions);
    kk::AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;
    ctx.settingsPages = &settingsPages;

    // ---- Non-interactive modes ----
    if (options.isScreenshot()) {
        kk::MainWindowOptions windowOptions;
        windowOptions.restoreLayout = false; // deterministic default layout
        windowOptions.saveLayout = false;
        windowOptions.autosave = false;
        kk::MainWindow window(ctx, windowOptions);
        if (!options.projectFile.isEmpty() && !project.load(options.projectFile, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
        return kk::runScreenshot(window, options.screenshotPage, options.screenshotPath);
    }

    // ---- Interactive ----
    kk::MainWindowOptions windowOptions;
    windowOptions.restoreLayout = !options.resetLayout;
    kk::MainWindow window(ctx, windowOptions);
    if (!options.projectFile.isEmpty())
        window.projectController()->openFile(options.projectFile, false);
    window.showRestored();
    return QApplication::exec();
}
