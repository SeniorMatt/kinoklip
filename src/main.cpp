// SPDX-License-Identifier: GPL-3.0-or-later
// Application entry point: graphics setup, application objects, command-line modes, main window.
#include "app/AppInfo.h"
#include "app/CliRunner.h"
#include "app/CommandLine.h"
#include "app/MainWindow.h"
#include "app/MenuReference.h"
#include "app/ProjectController.h"
#include "app/Screenshot.h"
#include "app/SettingsTransferDialog.h"
#include "app/Theme.h"
#include "core/EditBehaviour.h"
#include "core/MemoryGuard.h"
#include "core/PowerBins.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/SessionLog.h"
#include "export/RenderCommand.h"
#include "media/HwAccel.h"
#include "matting/MatteService.h"
#include "media/MediaCache.h"
#include "pack/ProjectPackage.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderFrameCommand.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/LiveResize.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/AiModelDialog.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QSettings>
#include <QSslSocket>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

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
    format.setSwapInterval(1); // vsync on X11 (no tearing); 0 on Wayland, see LiveResize.h
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // Zero-copy hardware decoding (VAAPI -> EGL DMA-BUF) needs an EGL context. Wayland always
    // has one; on X11 Qt uses GLX unless asked: KINOKLIP_X11_EGL=1 asks for EGL (tester switch
    // for 4K hardware decoding on X11 sessions; the session log says whether it worked).
    if (qEnvironmentVariableIntValue("KINOKLIP_X11_EGL") > 0
        && !qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION")) {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    }
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

/// True if the arguments ask only for --help/--version (or an option error QCommandLineParser
/// reports and exits on): answered with a QCoreApplication, so it works without a display.
bool wantsTextOnly(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--") // positional arguments only from here on
            return false;
        if (arg == "-h" || arg == "--help" || arg == "--help-all" || arg == "-?" || arg == "-v"
            || arg == "--version" || arg == "--check-tls")
            return true;
    }
    return false;
}

/// True if --menu-reference was given. Checked before anything reads the settings: that mode
/// must run with default settings and leave the user's alone (docs/menu-reference.md has to
/// come out the same on every machine).
bool wantsMenuReference(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--")
            return false;
        if (arg == "--menu-reference" || arg.startsWith("--menu-reference="))
            return true;
    }
    return false;
}

void setApplicationIdentity()
{
    QCoreApplication::setOrganizationName(QLatin1String(kk::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(kk::appinfo::name));
    QCoreApplication::setApplicationVersion(kk::appinfo::displayVersion());
}

/// `kinoklip <command> ...` (app/CliRunner.h): runs one command of the command layer without a
/// window. A QGuiApplication on Qt's offscreen platform, so no display or desktop session is
/// needed (rendering uses an offscreen OpenGL context, as in the tests).
int runCommandLine(int argc, char** argv, const QStringList& arguments)
{
    qputenv("QT_QPA_PLATFORM", "offscreen"); // headless whatever the session says
    configureOpenGL();
    QGuiApplication app(argc, argv);
    setApplicationIdentity();
    return kk::cli::run(arguments, stdout, stderr);
}

/// True if the first argument is a command-line command rather than a project to open.
bool wantsCommandLine(int argc, char** argv, QStringList* arguments)
{
    if (argc < 2 || argv[1][0] == '-')
        return false;
    kk::initResources(); // the command list reads the render presets
    for (int i = 1; i < argc; ++i)
        arguments->append(QString::fromLocal8Bit(argv[i]));
    return kk::cli::isCommand(*arguments);
}

} // namespace

int main(int argc, char** argv)
{
    // First of all: keep this session's messages for Help > Export Log File (in memory only;
    // stderr is unchanged because the previous handler still gets every message).
    kk::sessionlog::installMessageHandler();
    QStringList commandArguments;
    if (wantsCommandLine(argc, argv, &commandArguments))
        return runCommandLine(argc, argv, commandArguments);
    if (wantsTextOnly(argc, argv)) {
        // No GUI: QApplication would abort without a display before we could parse.
        QCoreApplication app(argc, argv);
        setApplicationIdentity();
        kk::CommandLineOptions options;
        QString error;
        const bool parsed = kk::parseCommandLine(QCoreApplication::arguments(), options, &error);
        if (parsed && options.checkTls) {
            // Secure downloads (the AI model) need a TLS backend plugin and its library.
            const bool ok = QSslSocket::supportsSsl();
            std::printf("TLS: %s, backend %s, library %s\n", ok ? "ok" : "NOT AVAILABLE",
                qPrintable(QSslSocket::activeBackend()),
                qPrintable(QSslSocket::sslLibraryVersionString()));
            return ok ? 0 : 1;
        }
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    // A private, empty settings and data folder for --menu-reference (removed on exit).
    std::unique_ptr<QTemporaryDir> isolatedSettings;
    if (wantsMenuReference(argc, argv)) {
        isolatedSettings = std::make_unique<QTemporaryDir>();
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("XDG_CONFIG_HOME", isolatedSettings->path().toLocal8Bit());
        qputenv("XDG_DATA_HOME", isolatedSettings->path().toLocal8Bit());
        qputenv("XDG_CACHE_HOME", isolatedSettings->path().toLocal8Bit());
    }
    applyUiScale();
    configureOpenGL();
    kk::theme::initBeforeApplication(); // bundled Breeze icons; must precede QApplication
    QApplication app(argc, argv);
    kk::liveresize::adjustSwapInterval(); // needs the platform name: after QApplication
    setApplicationIdentity();
    // Matches packaging/linux/org.kinoklip.Kinoklip.desktop: Wayland app_id / X11 WM_CLASS,
    // so the compositor shows the right icon and groups windows with the desktop entry.
    QGuiApplication::setDesktopFileName(QLatin1String(kk::appinfo::desktopId));
    kk::initResources();
    kk::EditBehaviour::migrateSettings();

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

    kk::theme::setSessionOverrides(options.style, options.colorScheme);
    kk::theme::apply(app);

    // Application-wide objects, handed to every widget through AppContext (no singletons).
    kk::Project project;
    kk::SelectionModel selection;
    kk::ActionRegistry actions;
    actions.loadDefaultKeymap();
    kk::registerStandardActions(actions);
    kk::theme::registerActions(actions); // Workspace > Color Theme / Style entries
    kk::SettingsPageRegistry settingsPages;
    kk::registerStandardSettingsPages(settingsPages, &actions);
    kk::theme::registerSettingsPage(settingsPages, actions);
    kk::registerSettingsTransferPage(settingsPages); // Preferences > Import and Export
    kk::registerAiModelsPage(settingsPages); // Preferences > AI Models (Background Removal)
    // Program playback: owns the compositor (render thread + GL context) shared by every
    // viewer, follows the project's timeline and media pool.
    kk::applyStartupPreferences(); // before the compositor exists (GPU processing mode)
    kk::PlaybackEngine playback;
    playback.setBehaviour(kk::PlaybackBehaviour::fromSettings());
    playback.setMediaPool(project.mediaPool());
    playback.setTimeline(project.timeline());
    QObject::connect(
        &project, &kk::Project::editCommitted, &playback, &kk::PlaybackEngine::noteEditCommitted);

    // A Background Removal matte finished in the background: draw the paused frame again
    // (at most every 250 ms).
    auto* matteRedraw = new QTimer(&playback);
    matteRedraw->setSingleShot(true);
    matteRedraw->setInterval(250);
    QObject::connect(matteRedraw, &QTimer::timeout, &playback, &kk::PlaybackEngine::redrawStill);
    QObject::connect(&kk::matting::MatteService::instance(), &kk::matting::MatteService::matteComputed,
        matteRedraw, [matteRedraw] {
            if (!matteRedraw->isActive())
                matteRedraw->start();
        });

    // Thumbnails / waveforms give way to playback (spec: thumbnails during playback).
    QObject::connect(&playback, &kk::PlaybackEngine::rateChanged, &playback,
        [](double rate) { kk::media::MediaCache::instance().setPlaybackActive(rate != 0.0); });

    kk::AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;
    ctx.settingsPages = &settingsPages;
    ctx.playback = &playback;
    // Power bins: shared by every project (AppData; KK_POWER_BINS overrides the file, used for
    // documentation screenshots and tests).
    kk::PowerBins powerBins(qEnvironmentVariable("KK_POWER_BINS"));
    ctx.powerBins = &powerBins;
    // Proxy / optimized media and render cache (sets ctx.media; before any panel exists).
    kk::MediaManagement mediaManagement(ctx);
    // Voice-over recording (arm buttons, Record, input level); sets ctx.voiceOver.
    kk::VoiceOverController voiceOver(ctx);

    // ---- Non-interactive modes ----
    if (options.isMenuReference()) {
        kk::MainWindowOptions windowOptions;
        windowOptions.restoreLayout = false;
        windowOptions.saveLayout = false;
        windowOptions.autosave = false;
        kk::MainWindow window(ctx, windowOptions);
        if (!kk::menureference::writeFile(window, options.menuReferenceFile, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
        return 0;
    }
    if (options.isScreenshot()) {
        kk::MainWindowOptions windowOptions;
        windowOptions.restoreLayout = false; // deterministic default layout
        windowOptions.saveLayout = false;
        windowOptions.autosave = false;
        kk::MainWindow window(ctx, windowOptions);
        // A single-file project opens through the controller (it holds the container).
        if (!options.projectFile.isEmpty() && kk::pack::isContainerFile(options.projectFile)) {
            if (!window.projectController()->openFile(options.projectFile, false))
                return 1;
        } else if (!options.projectFile.isEmpty() && !project.load(options.projectFile, &error)) {
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
    // The hardware decoder device (the window applied the stored preference): opened in the
    // background now, so the first frame after an import does not wait 200 - 300 ms for it.
    kk::media::warmUpHwDecoding();
    window.showRestored();
    // The project given on the command line (openFile() first offers its autosave if a crash
    // left one), otherwise crash recovery of any project. Without either, the Project Manager (or,
    // by preference, the last project). Offscreen runs (CI, scripted checks) never get the Project
    // Manager: nobody could close it.
    kk::ProjectController* projects = window.projectController();
    if (!options.projectFile.isEmpty()) {
        projects->openFile(options.projectFile, false); // offers that project's autosave
    } else if (projects->offerRecovery()) {
        // restored: that project is open
    } else if (!kk::prefs::startupShowsProjectManager()) {
        projects->openLastProject();
    } else if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        projects->showProjectManager();
    }
    // A render that started and never finished: the kernel most likely killed the program.
    if (const QString killed = kk::memguard::takeInterruptedRender(); !killed.isEmpty()) {
        qWarning().noquote() << "the last render was interrupted:" << killed;
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
            QMessageBox::warning(&window, QObject::tr("Render Interrupted"),
                QObject::tr("The last render did not finish: %1\n\nThe program was probably "
                            "closed by the system because the computer ran out of memory. Close "
                            "other programs, or render in smaller parts, and try again.")
                    .arg(killed));
        }
    }
    return QApplication::exec();
}
