// SPDX-License-Identifier: GPL-3.0-or-later
// Application entry point: graphics setup, application objects, command-line modes, main window.
#include "app/AppInfo.h"
#include "app/CliRunner.h"
#include "app/CommandLine.h"
#include "app/GraphicsCheck.h"
#include "app/MainWindow.h"
#include "app/MenuReference.h"
#include "app/ProjectController.h"
#include "app/Screenshot.h"
#include "app/SettingsTransferDialog.h"
#include "app/Theme.h"
#include "app/WelcomeDialog.h"
#include "core/EditBehaviour.h"
#include "core/MemoryBudget.h"
#include "core/MemoryGuard.h"
#include "core/Translations.h"
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
#include "plugins/AudioPluginRegistry.h"
#include "plugins/OfxRegistry.h"
#include "plugins/PluginRegistry.h"
#include "playback/PlaybackEngine.h"
#include "render/GlApi.h"
#include "render/RenderFrameCommand.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/UiFonts.h"
#include "ui/common/LiveResize.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/Resources.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/StandardActions.h"
#include "ui/dialogs/AiModelDialog.h"
#include "ui/dialogs/PluginsPage.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
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
    // Desktop OpenGL 3.3 core first; kk::render::chooseGlApi() (after QApplication exists)
    // switches the default to OpenGL ES 3.0 when the driver cannot make a 3.3 core context
    // (Raspberry Pi 5), or when KK_GL_API=gles asks for it.
    QSurfaceFormat format = kk::render::glFormat(kk::render::requestedGlApi());
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

/// After QApplication: picks desktop OpenGL 3.3 core or OpenGL ES 3.0 for this session and
/// logs the choice. Platforms without OpenGL (offscreen, minimal) keep the default format;
/// their compositor and viewer fall back to the CPU as before.
void chooseOpenGLApi()
{
    static const QStringList kNoGl {QStringLiteral("offscreen"), QStringLiteral("minimal"),
        QStringLiteral("vnc"), QStringLiteral("linuxfb")};
    if (kNoGl.contains(QGuiApplication::platformName())
        || qEnvironmentVariableIntValue("KK_NO_OPENGL") != 0
        || qEnvironmentVariable("KINOKLIP_RENDER").compare(QLatin1String("cpu"), Qt::CaseInsensitive) == 0)
        return;
    kk::render::chooseGlApi();
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

/// True if --graphics-check was given: runs on the real platform (that is what is tested); with no
/// display at all it runs on Qt's offscreen platform and the report says that no card was tested.
bool wantsGraphicsCheck(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--")
            return false;
        if (arg == "--graphics-check")
            return true;
    }
    return false;
}

void setApplicationIdentity()
{
    QCoreApplication::setOrganizationName(QLatin1String(kk::appinfo::id));
    QCoreApplication::setApplicationName(QLatin1String(kk::appinfo::name));
    QCoreApplication::setApplicationVersion(kk::appinfo::displayVersion());
    QGuiApplication::setApplicationDisplayName(kk::appinfo::displayName()); // "Kinoklip Dev"
}

/// plugin.render_preview and the compile check of plugin.validate need an OpenGL context, which
/// Qt's offscreen platform does not give.
bool commandNeedsOpenGl(const QStringList& arguments)
{
    if (arguments.value(0) != QLatin1String("plugin"))
        return false;
    const QString verb = arguments.value(1);
    return verb == QLatin1String("render_preview")
        || (verb == QLatin1String("validate") && !arguments.contains(QLatin1String("--static")));
}

/// Runs this command again under xvfb-run with the xcb platform (the way the GL tests do) when
/// the session has no display. Returns the exit code, or -1 when that is not possible.
int rerunUnderXvfb(const QStringList& arguments)
{
    if (!qEnvironmentVariableIsEmpty("KK_NO_XVFB"))
        return -1;
    const QString xvfb = QStandardPaths::findExecutable(QStringLiteral("xvfb-run"));
    if (xvfb.isEmpty())
        return -1;
    const QByteArray appImage = qgetenv("APPIMAGE");
    QStringList args {QStringLiteral("-a"), QStringLiteral("env"),
        QStringLiteral("QT_QPA_PLATFORM=xcb"), QStringLiteral("KK_NO_XVFB=1"),
        appImage.isEmpty() ? QCoreApplication::applicationFilePath()
                           : QString::fromLocal8Bit(appImage)};
    args += arguments;
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(xvfb, args);
    if (child.waitForStarted() && child.waitForFinished(-1)
        && child.exitStatus() == QProcess::NormalExit)
        return child.exitCode();
    return -1;
}

/// `kinoklip <command> ...` (app/CliRunner.h): runs one command of the command layer without a
/// window. A QGuiApplication on Qt's offscreen platform, so no display or desktop session is
/// needed (rendering uses an offscreen OpenGL context, as in the tests).
int runCommandLine(int argc, char** argv, const QStringList& arguments)
{
    const bool hasDisplay = !qEnvironmentVariableIsEmpty("DISPLAY")
        || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    const bool needsGl = commandNeedsOpenGl(arguments);
    if (!needsGl || !hasDisplay) {
        qputenv("QT_QPA_PLATFORM", "offscreen"); // headless whatever the session says
        // Without a display the OpenGL commands start again under xvfb-run, where a context
        // exists (the answer, files and exit code are the same).
        if (needsGl) {
            QCoreApplication probe(argc, argv);
            const int code = rerunUnderXvfb(arguments);
            if (code >= 0)
                return code;
        }
    }
    configureOpenGL();
    QGuiApplication app(argc, argv);
    setApplicationIdentity();
    kk::plugins::PluginRegistry::instance().load();
    kk::plugins::OfxRegistry::instance().load();
    kk::plugins::AudioPluginRegistry::instance().load();
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
    kk::membudget::tuneAllocator(); // before the first big allocation (core/MemoryBudget.h)
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
    if (wantsGraphicsCheck(argc, argv) && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    applyUiScale();
    configureOpenGL();
    kk::theme::initBeforeApplication(); // bundled Breeze icons; must precede QApplication
    QApplication app(argc, argv);
    chooseOpenGLApi(); // needs a platform: after QApplication, before any viewer or compositor
    kk::liveresize::adjustSwapInterval(); // needs the platform name: after QApplication
    setApplicationIdentity();
    // Matches packaging/linux/org.kinoklip.Kinoklip.desktop: Wayland app_id / X11 WM_CLASS,
    // so the compositor shows the right icon and groups windows with the desktop entry.
    QGuiApplication::setDesktopFileName(QLatin1String(kk::appinfo::desktopId));
    kk::initResources();
    if (wantsGraphicsCheck(argc, argv)) {
        // Before the plugin scans and the settings migration: nothing but the card is tested.
        kk::CommandLineOptions check;
        QString checkError;
        if (!kk::parseCommandLine(QCoreApplication::arguments(), check, &checkError)) {
            std::fprintf(stderr, "%s\n", qPrintable(checkError));
            return 2;
        }
        return kk::graphicscheck::runCommandLine(check.graphicsCheckFile);
    }
    kk::EditBehaviour::migrateSettings();
    kk::plugins::PluginRegistry::instance().load(); // before any project loads
    // OpenFX, LV2 and CLAP bundles: the cache is read now, new or changed bundles are probed in the
    // background (up to three at once) and the effects library updates when they are done
    kk::plugins::OfxRegistry::instance().loadAsync();
    kk::plugins::AudioPluginRegistry::instance().loadAsync();

    kk::CommandLineOptions options;
    QString error;
    if (!kk::parseCommandLine(QCoreApplication::arguments(), options, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    // Before any widget exists: --lang, KK_LANG, the Preferences choice, the desktop language.
    kk::i18n::install(options.language);
    if (options.isRender())
        return kk::runRender(options.renderProject, options.renderOutput, options.renderPreset);
    if (options.isRenderFrame())
        return kk::runRenderFrame(
            options.renderFrameProject, options.renderFrameIndex, options.renderFramePath);

    kk::uifonts::setSessionOverride(options.interfaceFont);
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
    kk::registerPluginsPage(settingsPages); // Preferences > Plugins
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
    // First start (or the first after an update that adds questions): the welcome window opens
    // over the editor; the Project Manager waits until it is closed.
    kk::WelcomeDialog* welcome = nullptr;
    if (kk::WelcomeDialog::wantedAtStartup(options.noWelcome, QGuiApplication::platformName()))
        welcome = window.showWelcome();
    if (!options.projectFile.isEmpty()) {
        projects->openFile(options.projectFile, false); // offers that project's autosave
    } else if (projects->offerRecovery()) {
        // restored: that project is open
    } else if (!kk::prefs::startupShowsProjectManager()) {
        projects->openLastProject();
    } else if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        if (welcome) {
            QObject::connect(welcome, &QDialog::finished, projects,
                [projects] { projects->showProjectManager(); });
        } else {
            projects->showProjectManager();
        }
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
