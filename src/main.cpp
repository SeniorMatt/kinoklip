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
#include "export/RenderCommand.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderFrameCommand.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QApplication>
#include <QSettings>
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

/// Must run before QApplication exists: applies the "Interface scale" preference
/// (ui/scaleFactor = "auto" or a factor like "1.25", written by the Preferences dialog) through
/// QT_SCALE_FACTOR, unless the user already set that variable.
void applyUiScale()
{
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        return; // explicit environment wins
    }
    // QSettings needs the organisation/application names, which are static and may be set early.
    QCoreApplication::setOrganizationName(QLatin1String(kk::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(kk::appinfo::name));
    const QString value
        = QSettings().value(QStringLiteral("ui/scaleFactor"), QStringLiteral("auto")).toString();
    bool ok = false;
    const double factor = value.toDouble(&ok);
    constexpr double kMinScale = 0.5;
    constexpr double kMaxScale = 4.0;
    if (ok && factor >= kMinScale && factor <= kMaxScale && factor != 1.0) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(factor));
    }
}

} // namespace

int main(int argc, char** argv)
{
    applyUiScale();
    configureOpenGL();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QLatin1String(kk::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(kk::appinfo::name));
    QCoreApplication::setApplicationVersion(QLatin1String(kk::appinfo::version));
    QGuiApplication::setDesktopFileName(QLatin1String(kk::appinfo::id));
    kk::initResources();

    kk::CommandLineOptions options;
    QString error;
    if (!kk::parseCommandLine(QCoreApplication::arguments(), options, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    if (options.isRender())
        return kk::runRender(options.renderProject, options.renderOutput, options.renderPreset);
    if (options.isRenderFrame())
        return kk::runRenderFrame(
            options.renderFrameProject, options.renderFrameIndex, options.renderFramePath);

    kk::theme::apply(app);

    // Application-wide objects, handed to every widget through AppContext (no singletons).
    kk::Project project;
    kk::SelectionModel selection;
    kk::ActionRegistry actions;
    actions.loadDefaultKeymap();
    kk::registerStandardActions(actions);
    // Program playback: owns the compositor (render thread + GL context) shared by every
    // viewer, follows the project's timeline and media pool.
    kk::PlaybackEngine playback;
    playback.setBehaviour(kk::PlaybackBehaviour::fromSettings());
    playback.setMediaPool(project.mediaPool());
    playback.setTimeline(project.timeline());

    kk::AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;
    ctx.playback = &playback;

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
        // Optional playhead for documentation screenshots (the viewer shows that frame).
        if (qEnvironmentVariableIsSet("KK_SCREENSHOT_FRAME")) {
            project.timeline()->setPlayhead(qEnvironmentVariableIntValue("KK_SCREENSHOT_FRAME"));
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
