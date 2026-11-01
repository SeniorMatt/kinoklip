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
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

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
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QLatin1String(cw::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(cw::appinfo::name));
    QCoreApplication::setApplicationVersion(QLatin1String(cw::appinfo::version));
    QGuiApplication::setDesktopFileName(QLatin1String(cw::appinfo::id));
    cw::initResources();

    cw::CommandLineOptions options;
    QString error;
    if (!cw::parseCommandLine(QCoreApplication::arguments(), options, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }

    cw::theme::apply(app);

    // Application-wide objects, handed to every widget through AppContext (no singletons).
    cw::Project project;
    cw::SelectionModel selection;
    cw::ActionRegistry actions;
    actions.loadDefaultKeymap();
    cw::registerStandardActions(actions);
    cw::AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;

    // ---- Non-interactive modes ----
    if (options.isScreenshot()) {
        cw::MainWindowOptions windowOptions;
        windowOptions.restoreLayout = false; // deterministic default layout
        windowOptions.saveLayout = false;
        windowOptions.autosave = false;
        cw::MainWindow window(ctx, windowOptions);
        if (!options.projectFile.isEmpty() && !project.load(options.projectFile, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
        return cw::runScreenshot(window, options.screenshotPage, options.screenshotPath);
    }

    // ---- Interactive ----
    cw::MainWindowOptions windowOptions;
    windowOptions.restoreLayout = !options.resetLayout;
    cw::MainWindow window(ctx, windowOptions);
    if (!options.projectFile.isEmpty())
        window.projectController()->openFile(options.projectFile, false);
    window.showRestored();
    return QApplication::exec();
}
