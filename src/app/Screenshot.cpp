// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation: pages, dialogs and open menus rendered to PNG.
#include "app/Screenshot.h"

#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/ScreenshotAudio.h"
#include "app/ScreenshotGeometry.h"
#include "app/pages/ColorPage.h"
#include "app/pages/CompositingPage.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/RetimeOps.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "projects/ProjectLibrary.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/comp/CompSession.h"
#include "ui/cut/CutEditor.h"
#include "ui/cut/CutViewer.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/NormalizeAudioDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MulticamSync.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/projects/ProjectManagerWindow.h"
#include "ui/scopes/ScopeView.h"
#include "ui/timeline/ChangeSpeedDialog.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace kk {

namespace {
const QSize kScreenshotSize(1920, 1080);
constexpr int kSettleDelayMs = 400; // let layouts, file system model and GL widgets settle
/// Compositing page: the node renders of both viewers (about 1 s on llvmpipe at 1080p).
constexpr int kCompositingSettleDelayMs = 2500;
constexpr int kDialogSettleDelayMs = 1500;
constexpr int kProjectManagerSettleDelayMs = 4000;
constexpr int kImportDialogDelayMs = 6000; // the portal and its dialog process start up //
                                           // Preferences probes the GPU on a worker thread
constexpr int kMenuMargin = 24; // px of window around an open menu in menu shots

QMenu* findMenu(QWidget* root, const QString& category)
{
    return root->findChild<QMenu*>(QStringLiteral("menu:") + category);
}

/// Window grab with the open popup menus painted on top (popups are separate windows, so
/// QWidget::grab() of the main window does not contain them), cropped around the menus.
QRect menuRect(QWidget& window, QMenu* menu)
{
    return QRect(window.mapFromGlobal(menu->mapToGlobal(QPoint(0, 0))), menu->size());
}

/// The part of the window a menu shot keeps: the open menus with a margin, from the window's
/// top left (the menu bar stays in the picture).
QRect menuShotArea(QWidget& window, const QList<QMenu*>& menus)
{
    QRect area;
    for (QMenu* menu : menus)
        area |= menuRect(window, menu);
    area = area.adjusted(-kMenuMargin, -kMenuMargin, kMenuMargin, kMenuMargin);
    area.setTop(0);
    area.setLeft(0);
    return area & window.rect();
}

QPixmap grabWithMenus(QWidget& window, const QList<QMenu*>& menus)
{
    QPixmap shot = window.grab();
    QPainter painter(&shot);
    for (QMenu* menu : menus)
        painter.drawPixmap(menuRect(window, menu).topLeft(), menu->grab());
    painter.end();
    return shot.copy(menuShotArea(window, menus));
}

/// Geometry sidecar of a menu shot (ScreenshotGeometry.h format): one `menu` entry per open
/// menu and one `menuitem` entry per visible command (`action` = its id, `text` without the
/// mnemonic, `submenu` = the submenu it opens), in the cropped picture's coordinates.
bool writeMenuGeometry(QWidget& window, const QList<QMenu*>& menus, const QString& jsonPath)
{
    const QRect area = menuShotArea(window, menus);
    QJsonArray entries;
    auto rectJson = [](const QRect& r) { return QJsonArray {r.x(), r.y(), r.width(), r.height()}; };
    for (QMenu* menu : menus) {
        const QRect m = menuRect(window, menu).translated(-area.topLeft());
        entries.append(QJsonObject {{QStringLiteral("kind"), QStringLiteral("menu")},
            {QStringLiteral("name"), menu->objectName()}, {QStringLiteral("rect"), rectJson(m)}});
        for (QAction* a : menu->actions()) {
            if (!a->isVisible() || a->isSeparator())
                continue;
            const QRect r = menu->actionGeometry(a).translated(m.topLeft());
            QJsonObject o {{QStringLiteral("kind"), QStringLiteral("menuitem")},
                {QStringLiteral("text"), a->text().remove(QLatin1Char('&'))},
                {QStringLiteral("rect"), rectJson(r)}};
            if (!a->objectName().isEmpty())
                o.insert(QStringLiteral("action"), a->objectName());
            if (a->menu())
                o.insert(QStringLiteral("submenu"), a->menu()->objectName());
            entries.append(o);
        }
    }
    QFile file(jsonPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QJsonObject doc {{QStringLiteral("width"), area.width()},
        {QStringLiteral("height"), area.height()}, {QStringLiteral("entries"), entries}};
    return file.write(QJsonDocument(doc).toJson()) > 0;
}

constexpr int kFramePollMs = 50; // then wait for the viewers' first rendered frames...
constexpr int kFrameTimeoutMs = 5000; // ...but never longer than this

/// True when every visible viewer with content shows a rendered frame and has nothing in
/// flight (decoders open and seek asynchronously, so a fixed delay is not enough).
bool viewersSettled()
{
    for (ViewerPanel* viewer : ViewerPanel::allViewers()) {
        PlaybackEngine* engine = viewer->engine();
        if (!viewer->isVisible() || !engine || engine->duration() <= 0) {
            continue;
        }
        if (!engine->currentFrame() || engine->scheduler().busy()) {
            return false;
        }
    }
    return true;
}
/// "import-dialog": File > Import Media is opened and the whole screen is grabbed (on X11 that
/// includes the desktop's file dialog, another process' window when it comes from the XDG
/// portal). KK_SCREENSHOT_DIALOG_DELAY: milliseconds to wait for the dialog (default 6000).
int grabImportDialog(MainWindow& window, const QString& pngPath)
{
    int exitCode = 1;
    const int delay = qEnvironmentVariableIsSet("KK_SCREENSHOT_DIALOG_DELAY")
        ? qEnvironmentVariableIntValue("KK_SCREENSHOT_DIALOG_DELAY")
        : kImportDialogDelayMs;
    QTimer::singleShot(kSettleDelayMs, &window, [&] {
        // Runs while the dialog is open (its nested event loop keeps timers going).
        QTimer::singleShot(delay, &window, [&] {
            QScreen* screen = QGuiApplication::primaryScreen();
            const QPixmap pixmap = screen ? screen->grabWindow(0) : QPixmap();
            if (!pixmap.isNull() && pixmap.save(pngPath, "PNG"))
                exitCode = 0;
            else
                std::fprintf(stderr, "cannot grab the screen into %s\n", qPrintable(pngPath));
            QApplication::exit(exitCode); // also ends the dialog's event loop
        });
        window.projectController()->importMedia();
    });
    QApplication::exec();
    return exitCode;
}

/// "multicam[:viewer|timeline]": a four-angle multicam clip (the project's multicam clip, else
/// one made from the first four media items, or from the files in KK_SCREENSHOT_IMPORT,
/// colon-separated, synced by sound when they share it) on the timeline with three cuts, the
/// source viewer in multicam mode following the second segment. Returns the panel to save
/// (nullptr = the window), or sets @p error.
QWidget* setupMulticamShot(MainWindow& window, const QString& section, QString* error)
{
    AppContext& ctx = window.context();
    Project* project = ctx.project;
    const QStringList importPaths
        = qEnvironmentVariable("KK_SCREENSHOT_IMPORT").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (!importPaths.isEmpty())
        project->importMedia(importPaths);
    quint64 uid = 0;
    for (const quint64 t : project->timelineUids()) {
        if (!uid && project->timelineData(t)->multicam)
            uid = t;
    }
    if (!uid) {
        QVector<SyncInput> inputs;
        for (const MediaItem& item : project->mediaPool()->items()) {
            if (inputs.size() < 4 && !item.isTimeline() && item.info.hasVideo
                && !item.info.isStillImage && item.info.durationUs > 0)
                inputs.append({item.id, item.name, item.path, item.info, 0});
        }
        if (inputs.isEmpty()) {
            *error = QStringLiteral("multicam: needs media (KK_SCREENSHOT_IMPORT=a.mp4:b.mp4)");
            return nullptr;
        }
        SyncReport report = computeSync(inputs, multicam::SyncMethod::Waveform);
        QVector<multicam::AngleSource> sources;
        for (qsizetype k = 0; k < inputs.size(); ++k)
            sources.append({inputs[k].mediaId, multicam::defaultAngleName(inputs[k].name),
                inputs[k].info, report.offsetsUs.value(k).value_or(0)});
        const media::MediaInfo& first = inputs.first().info;
        uid = project->createMulticamClip(sources, QStringLiteral("Concert Multicam"),
            first.frameRate.isValid() ? first.frameRate : project->settings().fps,
            first.displayWidth > 0 ? first.displayWidth : first.width,
            first.displayHeight > 0 ? first.displayHeight : first.height);
    }
    const auto mc = project->timelineData(uid);
    if (!mc) {
        *error = QStringLiteral("multicam: could not create the multicam clip");
        return nullptr;
    }
    // A new timeline with the multicam clip and cuts to angles 2, 3, 4 (or as many as exist).
    const quint64 tl = project->createTimeline(QStringLiteral("Multicam Edit"));
    project->openTab(tl);
    project->beginEdit(QStringLiteral("Place"));
    TimelineData& d = project->timeline()->mutableData();
    const ClipData clip = multicam::makeMulticamClip(*mc, project->timelineItemId(uid), 0);
    editops::linkClips(d,
        editops::overwriteClips(
            d, {{TrackRef {TrackType::Video, 0}, clip}, {TrackRef {TrackType::Audio, 0}, clip}}));
    const int angles = multicam::angleCount(*mc);
    const auto lookup = project->timelineLookup();
    for (int k = 1; k < std::min(angles, 4); ++k)
        multicam::cutToAngle(d, clip.duration * k / 4, k, multicam::AngleTarget::Both, lookup);
    project->endEdit();
    project->undoStack()->setClean(); // demo edits: closing must not ask to save them
    project->timeline()->setPlayhead(clip.duration * 3 / 8);
    window.editPage()->setSingleViewer(false);
    ViewerPanel* source = window.editPage()->sourceViewer();
    source->show();
    source->setMulticamMode(true);
    auto* timelinePanel = window.findChild<TimelinePanel*>();
    if (timelinePanel) {
        TimelineWidget* w = timelinePanel->timelineWidget();
        const int64_t duration = clip.duration;
        QTimer::singleShot(0, w, [w, duration] {
            const double width = std::max(200, w->viewport()->width() - 220);
            w->setPixelsPerFrame(width * 0.9 / double(duration), 0);
            w->scrollToFrame(0);
        });
    }
    if (section == QLatin1String("viewer"))
        return source;
    if (section == QLatin1String("timeline"))
        return timelinePanel;
    return nullptr;
}

/// Selects the Effects library row of descriptor @p id, expanding its folders and scrolling
/// to it. False when there is no such row.
bool selectLibraryItem(EffectsLibraryPanel* library, const QString& id)
{
    QTreeWidget* tree = library->tree();
    QTreeWidgetItemIterator it(tree);
    for (; *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, Qt::UserRole).toString() != id)
            continue;
        for (QTreeWidgetItem* p = item->parent(); p; p = p->parent())
            p->setExpanded(true);
        tree->setCurrentItem(item);
        QTimer::singleShot(0, tree, [tree, item] { tree->scrollToItem(item); });
        return true;
    }
    return false;
}

/// The visible timeline viewer (Edit page), or nullptr.
ViewerPanel* timelineViewer()
{
    for (ViewerPanel* viewer : ViewerPanel::allViewers()) {
        if (viewer->mode() == ViewerPanel::Mode::Timeline && viewer->isVisible())
            return viewer;
    }
    return nullptr;
}

} // namespace

int runScreenshot(MainWindow& window, const QString& target, const QString& pngPath)
{
    const bool isPage = window.setCurrentPage(target);
    std::unique_ptr<QWidget> dialog;
    QWidget* panel = nullptr; // panel targets: only this widget is saved
    QWidget* toolWindow = nullptr; // a floating editor composited into the window grab
    QList<QMenu*> menus;
    if (!isPage)
        window.setCurrentPage(QStringLiteral("edit"));
    // KK_SCREENSHOT_SIZE=1366x768 checks the layout on small screens.
    QSize size = kScreenshotSize;
    const QStringList wh = qEnvironmentVariable("KK_SCREENSHOT_SIZE").split(QLatin1Char('x'));
    if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0)
        size = QSize(wh[0].toInt(), wh[1].toInt());
    window.resize(size);
    window.show();

    AppContext& ctx = window.context();
    int delayMs = kSettleDelayMs;
    // "preferences[:section]" and "project-settings[:section]" open a dialog section.
    const QString section = target.section(QLatin1Char(':'), 1);
    const QString base = target.section(QLatin1Char(':'), 0, 0);
    if (base == QLatin1String("preferences")) {
        auto prefs = std::make_unique<PreferencesDialog>(ctx.settingsPages, &window);
        if (!section.isEmpty() && !prefs->showSection(section)) {
            std::fprintf(stderr, "unknown preferences section '%s'\n", qPrintable(section));
            return 2;
        }
        dialog = std::move(prefs);
        delayMs = kDialogSettleDelayMs;
    } else if (base == QLatin1String("project-settings")) {
        auto settings = std::make_unique<ProjectSettingsDialog>(ctx.project, &window);
        if (!section.isEmpty() && !settings->showSection(section)) {
            std::fprintf(stderr, "unknown project settings section '%s'\n", qPrintable(section));
            return 2;
        }
        dialog = std::move(settings);
    } else if (target == QLatin1String("project-manager")) {
        // The Project Manager over the library of the preferences, or KK_PROJECT_LIBRARY (demo
        // libraries for documentation); a project given on the command line is marked open.
        const QString root = qEnvironmentVariableIsSet("KK_PROJECT_LIBRARY")
            ? qEnvironmentVariable("KK_PROJECT_LIBRARY")
            : prefs::projectLibraryDirectory();
        static std::unique_ptr<ProjectLibrary> library; // outlives the dialog (static: one run)
        library = std::make_unique<ProjectLibrary>(root);
        auto manager = std::make_unique<ProjectManagerWindow>(library.get(), &window);
        manager->setRecentProjects(ProjectController::recentProjects());
        manager->setOpenProjectPath(ctx.project->filePath());
        manager->resize(1180, 720);
        dialog = std::move(manager);
        delayMs = kProjectManagerSettleDelayMs; // posters: metadata, then the media cache
    } else if (target == QLatin1String("new-project")) {
        // The Project Manager's New Project dialog (name and kind).
        dialog = std::make_unique<NewProjectDialog>(QStringLiteral("Untitled Project"), &window);
    } else if (target == QLatin1String("keyboard")) {
        dialog = std::make_unique<KeyboardCustomizationDialog>(*ctx.actions, &window);
    } else if (base == QLatin1String("keyboard")) {
        // keyboard:<profile id>: Keyboard Customization with that preset chosen, filtered to
        // the split commands (the guide's "Use the shortcuts of your old editor").
        ctx.actions->setActiveProfile(section);
        auto keyboard = std::make_unique<KeyboardCustomizationDialog>(*ctx.actions, &window);
        keyboard->setFilterText(QStringLiteral("split"));
        dialog = std::move(keyboard);
    } else if (target == QLatin1String("menu-color-theme") || target == QLatin1String("menu-style")
        || target == QLatin1String("menu-playback-resolution")) {
        const bool playback = target == QLatin1String("menu-playback-resolution");
        QMenu* settings
            = findMenu(&window, playback ? QStringLiteral("playback") : QStringLiteral("settings"));
        QMenu* sub = findMenu(&window,
            playback                                    ? QStringLiteral("playback.resolution")
                : target == QLatin1String("menu-style") ? QStringLiteral("settings.style")
                                                        : QStringLiteral("settings.color_scheme"));
        if (!settings || !sub) {
            std::fprintf(stderr, "no such menu\n");
            return 2;
        }
        QMenuBar* bar = window.menuBar();
        const QRect barItem = bar->actionGeometry(settings->menuAction());
        settings->popup(bar->mapToGlobal(barItem.bottomLeft()));
        settings->setActiveAction(sub->menuAction());
        const QRect subItem = settings->actionGeometry(sub->menuAction());
        sub->popup(settings->mapToGlobal(subItem.topRight()));
        menus = {settings, sub};
    } else if (target == QLatin1String("inspector")) {
        // Select the first video clip so the inspector shows its parameter rows.
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            if (!track.clips.empty()) {
                ctx.selection->selectClip(track.clips.front().id);
                break;
            }
        }
        panel = window.findChild<InspectorPanel*>();
    } else if (base == QLatin1String("keyframes") || target == QLatin1String("keyframes-window")) {
        // Keyframe editor + curve editor of the first animated video clip, a few keys selected.
        auto* timelinePanel = window.findChild<TimelinePanel*>();
        const ClipData* animated = nullptr;
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            for (const ClipData& c : track.clips) {
                if (!animated && !keyframeops::allKeyFrames(c).isEmpty())
                    animated = &c;
            }
        }
        if (!timelinePanel || !animated) {
            std::fprintf(stderr, "keyframes: needs a project with an animated video clip\n");
            return 2;
        }
        const quint64 id = animated->id;
        ctx.selection->selectClip(id);
        const QVector<int64_t> frames = keyframeops::allKeyFrames(*animated);
        ctx.selection->setKeyframes(keyframeops::keysAt(*animated, frames.value(1)));
        KeyframeLanes* lanes = timelinePanel->timelineWidget()->keyframeLanes();
        // keyframes:curves shows the curve lane only, keyframes:narrow zooms the timeline out
        // so the clip is too narrow and its lane offers the Keyframes panel.
        lanes->setKeyframeEditorOpen(id, section != QLatin1String("curves"));
        lanes->setCurveEditorOpen(id, true);
        if (section == QLatin1String("narrow")) {
            TimelineWidget* w = timelinePanel->timelineWidget();
            const int64_t start = animated->start;
            QTimer::singleShot(0, w, [w, start] {
                w->setPixelsPerFrame(0.6, 0);
                w->scrollToFrame(double(start) - 300);
            });
        }
        if (base == QLatin1String("keyframes"))
            panel = timelinePanel;
    } else if (base == QLatin1String("keyframes-panel")) {
        // The Keyframes panel on the most animated video clip, a few keys selected, the
        // playhead inside the clip. Sections: dope | curves | absolute | float.
        const ClipData* animated = nullptr;
        qsizetype best = 0;
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            for (const ClipData& c : track.clips) {
                const qsizetype n = keyframeops::animatedParams(c).size();
                if (n > best) {
                    best = n;
                    animated = &c;
                }
            }
        }
        EditPage* edit = window.editPage();
        if (!animated || !edit) {
            std::fprintf(stderr, "keyframes-panel: needs a project with an animated video clip\n");
            return 2;
        }
        ctx.selection->selectClip(animated->id);
        const QVector<int64_t> frames = keyframeops::allKeyFrames(*animated);
        ctx.selection->setKeyframes(keyframeops::keysAt(*animated, frames.value(2)));
        ctx.project->timeline()->setPlayhead(animated->start + frames.value(3));
        edit->setPanelVisible(PanelId::Keyframes, true);
        KeyframesPanel* kf = edit->keyframes();
        if (section == QLatin1String("dope"))
            kf->setViews(true, false);
        else if (section == QLatin1String("curves"))
            kf->setViews(false, true);
        else if (section == QLatin1String("absolute"))
            kf->setNormalised(false);
        else if (section == QLatin1String("float")) {
            edit->setKeyframesFloating(true);
            if (QWidget* w = edit->keyframesWindow()) {
                w->resize(1600, 640);
                panel = w;
            }
        }
    } else if (target == QLatin1String("edit-single") || target == QLatin1String("edit-dual")) {
        // The viewer mode button in the viewer headers (spec 24.4).
        window.editPage()->setSingleViewer(target == QLatin1String("edit-single"));
    } else if (target == QLatin1String("transition")) {
        // The first transition on a video track, selected (spec 13.10), timeline zoomed to it.
        auto* timelinePanel = window.findChild<TimelinePanel*>();
        const TimelineData& data = ctx.project->timeline()->data();
        std::optional<TransitionRef> ref;
        for (const TrackData& track : data.videoTracks) {
            for (const ClipData& c : track.clips) {
                if (!ref && (c.transitionIn || c.transitionOut))
                    ref = TransitionRef {c.id, c.transitionIn.has_value()};
            }
        }
        if (!timelinePanel || !ref) {
            std::fprintf(stderr, "transition: needs a project with a video transition\n");
            return 2;
        }
        timelinePanel->controller()->selectTransition(*ref);
        if (const auto range = editops::transitionWindow(data, ref->clipId, ref->head)) {
            TimelineWidget* w = timelinePanel->timelineWidget();
            QTimer::singleShot(0, w, [w, range] {
                w->setPixelsPerFrame(6.0, 0);
                w->scrollToFrame(double(range->first) - 60);
            });
        }
    } else if (base == QLatin1String("retime")) {
        // Retime controls of the first media video clip, turned into three speed segments
        // (50 %, 100 % with a smooth ramp into it, 200 %) when it has no retime curve yet.
        // "retime:curve" also opens its retime curve in the inline curve editor.
        auto* timelinePanel = window.findChild<TimelinePanel*>();
        const ClipData* clip = nullptr;
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            for (const ClipData& c : track.clips) {
                if (!clip && c.kind == ClipKind::Media && c.duration >= 30)
                    clip = &c;
            }
        }
        if (!timelinePanel || !clip) {
            std::fprintf(stderr, "retime: needs a project with a video clip of 30+ frames\n");
            return 2;
        }
        const quint64 id = clip->id;
        if (!retime::hasCurve(*clip)) {
            const int64_t start = clip->start;
            const int64_t third = clip->duration / 3;
            ctx.project->beginEdit(QStringLiteral("Retime"));
            TimelineData& data = ctx.project->timeline()->mutableData();
            retimeops::addSpeedPoint(data, id, start + third);
            retimeops::addSpeedPoint(data, id, start + 2 * third);
            retimeops::setSegmentSpeed(data, id, 0, 0.5);
            retimeops::setSegmentSpeed(data, id, 2, 2.0);
            retimeops::setPointRamp(data, id, 2 * third, Interp::Smooth);
            ctx.project->endEdit();
        }
        // Demo edits only: closing must not ask to save them.
        auto markClean = [&ctx] { ctx.project->undoStack()->setClean(); };
        clip = editops::clipById(ctx.project->timeline()->data(), id);
        TimelineController* tc = timelinePanel->controller();
        ctx.selection->selectClip(id);
        tc->setRetimeControlsShown(id, true);
        ctx.project->timeline()->setPlayhead(clip->start + clip->duration / 2);
        if (section == QLatin1String("curve"))
            tc->showRetimeCurve();
        markClean();
        TimelineWidget* w = timelinePanel->timelineWidget();
        const int64_t start = clip->start;
        const int64_t duration = clip->duration;
        QTimer::singleShot(0, w, [w, start, duration] {
            // The clip fills about three quarters of the track area.
            const double width = std::max(200, w->viewport()->width() - 220);
            w->setPixelsPerFrame(width * 0.75 / double(duration), 0);
            w->scrollToFrame(double(start) - double(duration) * 0.12);
        });
        panel = timelinePanel;
    } else if (base == QLatin1String("masks")) {
        // The first video clip with masks, its first mask active (on-screen controls shown)
        // and the Inspector scrolled to the Masks section. "masks:inspector" / "masks:viewer"
        // save only that panel.
        const ClipData* masked = nullptr;
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            for (const ClipData& c : track.clips) {
                if (!masked && !c.masks.isEmpty())
                    masked = &c;
            }
        }
        if (!masked) {
            std::fprintf(stderr, "masks: needs a project with a masked video clip\n");
            return 2;
        }
        const int64_t playhead = ctx.project->timeline()->playhead();
        if (!masked->contains(playhead))
            ctx.project->timeline()->setPlayhead(masked->start);
        ctx.selection->selectClip(masked->id);
        const int activeIndex = std::clamp(
            qEnvironmentVariableIntValue("KK_SCREENSHOT_MASK"), 0, int(masked->masks.size()) - 1);
        ctx.selection->setActiveMask(masked->masks[activeIndex].id);
        if (ctx.actions->contains(QStringLiteral("view.onscreen_masks")))
            ctx.actions->action(QStringLiteral("view.onscreen_masks"))->setChecked(true);
        auto* inspector = window.findChild<InspectorPanel*>();
        if (inspector) {
            inspector->tabs()->setCurrentIndex(InspectorPanel::VideoTab);
            QTimer::singleShot(0, inspector, [inspector] {
                // Collapse Transform and Cropping so the Masks section is in view.
                for (ParamGroupBox* g : inspector->groups()) {
                    if (!g->objectName().startsWith(QLatin1String("mask")))
                        g->setCollapsed(true);
                }
            });
        }
        if (section == QLatin1String("inspector"))
            panel = inspector;
        else if (section == QLatin1String("viewer")) {
            for (ViewerPanel* viewer : ViewerPanel::allViewers()) {
                if (viewer->mode() == ViewerPanel::Mode::Timeline && viewer->isVisible())
                    panel = viewer;
            }
        }
    } else if (base == QLatin1String("multicam")) {
        QString error;
        panel = setupMulticamShot(window, section, &error);
        if (!error.isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 2;
        }
    } else if (base == QLatin1String("subtitles")) {
        // The caption of ST1 under the playhead (or its first caption) selected, the Inspector
        // on its Captions tab. "subtitles:inspector" saves only the Inspector.
        const TimelineData& data = ctx.project->timeline()->data();
        if (data.subtitleTracks.empty() || data.subtitleTracks.front().clips.empty()) {
            std::fprintf(stderr, "subtitles: needs a project with captions on ST1\n");
            return 2;
        }
        const TrackData& st = data.subtitleTracks.front();
        const int64_t playhead = ctx.project->timeline()->playhead();
        const ClipData* caption = st.clipAt(playhead);
        if (!caption) {
            caption = &st.clips.front();
            ctx.project->timeline()->setPlayhead(caption->start);
        }
        ctx.selection->selectClip(caption->id);
        auto* inspector = window.findChild<InspectorPanel*>();
        if (inspector)
            inspector->tabs()->setCurrentIndex(InspectorPanel::TitleTab);
        if (section == QLatin1String("inspector"))
            panel = inspector;
    } else if (base == QLatin1String("levels") || base == QLatin1String("stacked")
        || base == QLatin1String("clip-attributes")) {
        // Spec section 32. The first media item with video and audio is put on the timeline
        // (when it is empty); "levels": volume keys on its audio clip and fades on both parts;
        // "stacked": a second timeline with the clip, View > Stacked Timelines on;
        // "clip-attributes[:audio|timecode]": the Clip Attributes dialog of the item.
        auto* timelinePanel = window.findChild<TimelinePanel*>();
        const MediaItem* item = nullptr;
        for (const MediaItem& m : ctx.project->mediaPool()->items()) {
            if (!item && m.kind == MediaItemKind::File && m.info.hasVideo && m.info.hasAudio)
                item = &m;
        }
        if (!timelinePanel || !item) {
            std::fprintf(
                stderr, "%s: needs a project with a video file with sound\n", qPrintable(base));
            return 2;
        }
        const QString mediaId = item->id;
        const int64_t lengthUs = std::min<int64_t>(item->info.durationUs, 8'000'000);
        TimelineController* tc = timelinePanel->controller();
        auto placeClip = [&] {
            if (ctx.project->timeline()->data().duration() > 0)
                return;
            ctx.project->timeline()->setPlayhead(0);
            tc->insertSource(mediaId, 0, lengthUs, SourceEditMode::Overwrite);
        };
        if (base == QLatin1String("clip-attributes")) {
            auto d = std::make_unique<ClipAttributesDialog>(*item, 1, &window);
            if (section == QLatin1String("audio"))
                d->tabs()->setCurrentIndex(1);
            else if (section == QLatin1String("timecode"))
                d->tabs()->setCurrentIndex(2);
            d->adjustSize();
            dialog = std::move(d);
            delayMs = kDialogSettleDelayMs;
        } else {
            placeClip();
            ctx.project->beginEdit(QStringLiteral("Demo"));
            TimelineData& d = ctx.project->timeline()->mutableData();
            if (base == QLatin1String("levels") && !d.audioTracks.empty())
                d.audioTracks.front().height = 120; // room for the level line
            const TimelineData& data = d;
            for (const auto* tracks : {&data.videoTracks, &data.audioTracks}) {
                for (const TrackData& t : *tracks) {
                    for (const ClipData& c : t.clips) {
                        levelops::setFade(d, c.id, true, c.duration / 6);
                        levelops::setFade(d, c.id, false, c.duration / 4);
                        if (&t == &data.audioTracks.front() && base == QLatin1String("levels")) {
                            levelops::addVolumeKey(d, c.id, c.duration / 3);
                            levelops::addVolumeKey(d, c.id, c.duration / 2);
                            levelops::addVolumeKey(d, c.id, c.duration * 2 / 3);
                            levelops::offsetVolume(d, c.id, c.duration / 2, -12.0);
                        }
                    }
                }
            }
            ctx.project->endEdit();
            if (base == QLatin1String("stacked")) {
                const quint64 first = ctx.project->activeTimelineUid();
                const quint64 second = ctx.project->createTimeline(QStringLiteral("Timeline B"));
                ctx.project->setActiveTimeline(second);
                ctx.project->timeline()->setPlayhead(0);
                tc->insertSource(mediaId, 0, lengthUs / 2, SourceEditMode::Overwrite);
                ctx.project->timeline()->setPlayhead(25);
                ctx.project->setActiveTimeline(first);
                ctx.project->closeTab(second); // Timeline B lives in the upper pane only
                ctx.project->timeline()->setPlayhead(60);
                timelinePanel->setStackedTimelines(true);
            }
            ctx.project->undoStack()->setClean(); // demo edits: closing must not ask
            TimelineWidget* w = timelinePanel->timelineWidget();
            QTimer::singleShot(0, w, [w] { w->zoomToFit(); });
            panel = timelinePanel;
        }
    } else if (base == QLatin1String("clip")) {
        // "clip:<track>[:video|audio|effects|title]" (user guide): the clip of that track
        // ("V2", "A1", "ST1") under the playhead, else its first clip, selected with the
        // Inspector on that tab. KK_SCREENSHOT_ONSCREEN=transform|crop|masks turns on those
        // on-screen controls of the timeline viewer.
        const QString trackName = section.section(QLatin1Char(':'), 0, 0);
        const QString tab = section.section(QLatin1Char(':'), 1);
        const TimelineData& data = ctx.project->timeline()->data();
        const QHash<QChar, TrackType> types {{QLatin1Char('V'), TrackType::Video},
            {QLatin1Char('A'), TrackType::Audio}, {QLatin1Char('S'), TrackType::Subtitle}};
        const int digits = trackName.startsWith(QLatin1String("ST")) ? 2 : 1;
        const int index = trackName.mid(digits).toInt() - 1;
        const TrackData* track = trackName.isEmpty() || index < 0
            ? nullptr
            : data.track(TrackRef {types.value(trackName.front(), TrackType::Video), index});
        if (!track || track->clips.empty()) {
            std::fprintf(stderr, "clip: no clip on track '%s'\n", qPrintable(trackName));
            return 2;
        }
        const ClipData* clip = track->clipAt(ctx.project->timeline()->playhead());
        if (!clip)
            clip = &track->clips.front();
        ctx.selection->selectClip(clip->id);
        if (auto* inspector = window.findChild<InspectorPanel*>()) {
            const QHash<QString, int> tabs {{QStringLiteral("video"), InspectorPanel::VideoTab},
                {QStringLiteral("audio"), InspectorPanel::AudioTab},
                {QStringLiteral("effects"), InspectorPanel::EffectsTab},
                {QStringLiteral("title"), InspectorPanel::TitleTab}};
            if (tabs.contains(tab))
                inspector->tabs()->setCurrentIndex(tabs.value(tab));
        }
        const QString onscreen = qEnvironmentVariable("KK_SCREENSHOT_ONSCREEN");
        const QString onscreenId = QStringLiteral("view.onscreen_") + onscreen;
        if (!onscreen.isEmpty() && ctx.actions->contains(onscreenId))
            ctx.actions->action(onscreenId)->setChecked(true);
    } else if (base == QLatin1String("menu")) {
        // "menu:<category>[:<submenu category>]" (user guide): that menu of the menu bar open
        // (and its submenu), KK_SCREENSHOT_MENU_ITEM=<action id> highlighted in the last one.
        const QString category = section.section(QLatin1Char(':'), 0, 0);
        const QString subCategory = section.section(QLatin1Char(':'), 1);
        QMenu* top = findMenu(&window, category);
        QMenu* sub = subCategory.isEmpty() ? nullptr : findMenu(&window, subCategory);
        if (!top || (!subCategory.isEmpty() && !sub)) {
            std::fprintf(stderr, "menu: no menu '%s'\n", qPrintable(section));
            return 2;
        }
        QMenuBar* bar = window.menuBar();
        const QRect barItem = bar->actionGeometry(top->menuAction());
        top->popup(bar->mapToGlobal(barItem.bottomLeft()));
        menus = {top};
        if (sub) {
            top->setActiveAction(sub->menuAction());
            const QRect subItem = top->actionGeometry(sub->menuAction());
            sub->popup(top->mapToGlobal(subItem.topRight()));
            menus.append(sub);
        }
        const QString itemId = qEnvironmentVariable("KK_SCREENSHOT_MENU_ITEM");
        if (!itemId.isEmpty() && ctx.actions->contains(itemId))
            menus.last()->setActiveAction(ctx.actions->action(itemId));
    } else if (target == QLatin1String("normalize-audio")) {
        // Clip > Normalize Audio Levels of every audio clip of the timeline (user guide).
        QVector<quint64> ids;
        for (const TrackData& track : ctx.project->timeline()->data().audioTracks) {
            for (const ClipData& c : track.clips)
                ids.append(c.id);
        }
        if (ids.isEmpty()) {
            std::fprintf(stderr, "normalize-audio: needs a project with audio clips\n");
            return 2;
        }
        ctx.selection->setClips(ids);
        auto d = std::make_unique<NormalizeAudioDialog>(
            ctx.project, normalizeTargets(*ctx.project, *ctx.selection), &window);
        d->adjustSize();
        dialog = std::move(d);
        delayMs = kDialogSettleDelayMs;
    } else if (target == QLatin1String("mediapool")) {
        panel = window.findChild<MediaPoolPanel*>();
    } else if (base == QLatin1String("effects")) {
        // "effects:<descriptor id>" also selects that item (its folders expanded, scrolled to).
        if (Page* edit = window.page(QStringLiteral("edit")))
            edit->setPanelVisible(PanelId::Effects, true);
        auto* library = window.findChild<EffectsLibraryPanel*>();
        if (library && !section.isEmpty() && !selectLibraryItem(library, section)) {
            std::fprintf(stderr, "effects: no library item '%s'\n", qPrintable(section));
            return 2;
        }
        panel = library;
    } else if (base == QLatin1String("fx")) {
        // "fx:<effect id>[:inspector|viewer]" (user guide): the top-most video clip carrying
        // that effect selected, the playhead in its middle, the Inspector on its Effects tab.
        // KK_SCREENSHOT_PICKER=1 also checks the first eyedropper of the Inspector.
        const QString effectId = section.section(QLatin1Char(':'), 0, 0);
        const QString part = section.section(QLatin1Char(':'), 1);
        const ClipData* found = nullptr;
        const auto& tracks = ctx.project->timeline()->data().videoTracks;
        for (auto t = tracks.rbegin(); t != tracks.rend() && !found; ++t) {
            for (const ClipData& c : t->clips) {
                const bool has = std::any_of(c.effects.begin(), c.effects.end(),
                    [&](const EffectInstance& fx) { return fx.type == effectId; });
                if (!found && has)
                    found = &c;
            }
        }
        if (!found) {
            std::fprintf(stderr, "fx: no video clip with effect '%s'\n", qPrintable(effectId));
            return 2;
        }
        ctx.selection->selectClip(found->id);
        if (!found->contains(ctx.project->timeline()->playhead()))
            ctx.project->timeline()->setPlayhead(found->start + found->duration / 2);
        auto* inspector = window.findChild<InspectorPanel*>();
        if (inspector) {
            inspector->tabs()->setCurrentIndex(InspectorPanel::EffectsTab);
            if (qEnvironmentVariableIntValue("KK_SCREENSHOT_PICKER") == 1) {
                QTimer::singleShot(0, inspector, [inspector] {
                    if (auto* picker = inspector->findChild<QAbstractButton*>(
                            QStringLiteral("ColorPickerButton")))
                        picker->click();
                });
            }
        }
        if (part == QLatin1String("inspector"))
            panel = inspector;
        else if (part == QLatin1String("viewer"))
            panel = timelineViewer();
    } else if (target == QLatin1String("change-speed")) {
        // Clip > Change Clip Speed (R) of the first media video clip (user guide).
        const ClipData* clip = nullptr;
        for (const TrackData& track : ctx.project->timeline()->data().videoTracks) {
            for (const ClipData& c : track.clips) {
                if (!clip && c.kind == ClipKind::Media)
                    clip = &c;
            }
        }
        if (!clip) {
            std::fprintf(stderr, "change-speed: needs a project with a video clip\n");
            return 2;
        }
        ctx.selection->selectClip(clip->id);
        Rational sourceFps;
        if (const MediaItem* item = ctx.project->mediaPool()->item(clip->mediaId))
            sourceFps = item->info.frameRate;
        auto d = std::make_unique<ChangeSpeedDialog>(
            *clip, ctx.project->timeline()->data().fps, sourceFps, &window);
        d->adjustSize();
        dialog = std::move(d);
        delayMs = kDialogSettleDelayMs;
    } else if (target == QLatin1String("timeline")) {
        // The timeline panel alone, zoomed to fit (user guide).
        auto* timelinePanel = window.findChild<TimelinePanel*>();
        if (timelinePanel) {
            TimelineWidget* w = timelinePanel->timelineWidget();
            QTimer::singleShot(0, w, [w] { w->zoomToFit(); });
        }
        panel = timelinePanel;
    } else if (base == QLatin1String("deliver")) {
        // "deliver:inout": the Deliver page with the YouTube 1080p preset (KK_SCREENSHOT_PRESET
        // = another preset id) and Render: In/Out Range; "deliver:queue" also adds the job to
        // the render queue (not rendered).
        window.setCurrentPage(QStringLiteral("deliver"));
        const QString preset = qEnvironmentVariableIsSet("KK_SCREENSHOT_PRESET")
            ? qEnvironmentVariable("KK_SCREENSHOT_PRESET")
            : QStringLiteral("youtube_1080p");
        auto* tile = window.findChild<QAbstractButton*>(QStringLiteral("PresetTile_") + preset);
        auto* range = window.findChild<QComboBox*>(QStringLiteral("RenderRange"));
        if (!tile || !range) {
            std::fprintf(stderr, "deliver: no preset '%s'\n", qPrintable(preset));
            return 2;
        }
        tile->click();
        range->setCurrentIndex(1); // In/Out Range
        // KK_SCREENSHOT_LOCATION: the folder shown in Location (after a queued job took the
        // real one), so pictures do not show the temporary folder the shot runs in.
        const QString location = qEnvironmentVariable("KK_SCREENSHOT_LOCATION");
        const bool queue = section == QLatin1String("queue");
        QTimer::singleShot(0, &window, [&window, location, queue] {
            if (queue) {
                const auto buttons = window.findChildren<QPushButton*>();
                for (QPushButton* b : buttons) {
                    if (b->isVisible()
                        && b->text() == RenderSettingsPanel::tr("Add to Render Queue"))
                        b->click();
                }
            }
            auto* field = window.findChild<QLineEdit*>(QStringLiteral("RenderLocation"));
            if (field && !location.isEmpty())
                field->setText(location);
        });
        // User guide: KK_SCREENSHOT_RENDER_RESOLUTION=<index> picks a Resolution entry,
        // KK_SCREENSHOT_RENDER_TAB=<index> a settings tab (0 Video, 1 Audio, 2 File),
        // KK_SCREENSHOT_RENDER_SUBTITLES=<format index> ticks Export subtitle with that format.
        if (qEnvironmentVariableIsSet("KK_SCREENSHOT_RENDER_RESOLUTION")) {
            if (auto* res = window.findChild<QComboBox*>(QStringLiteral("RenderResolution")))
                res->setCurrentIndex(
                    qEnvironmentVariableIntValue("KK_SCREENSHOT_RENDER_RESOLUTION"));
        }
        if (qEnvironmentVariableIsSet("KK_SCREENSHOT_RENDER_SUBTITLES")) {
            if (auto* box = window.findChild<QCheckBox*>(QStringLiteral("RenderSubtitleExport")))
                box->setChecked(true);
            if (auto* mode = window.findChild<QComboBox*>(QStringLiteral("RenderSubtitleMode")))
                mode->setCurrentIndex(
                    qEnvironmentVariableIntValue("KK_SCREENSHOT_RENDER_SUBTITLES"));
        }
        if (qEnvironmentVariableIsSet("KK_SCREENSHOT_RENDER_TAB")) {
            auto* settings = window.findChild<RenderSettingsPanel*>();
            if (auto* tabs = settings ? settings->findChild<QTabWidget*>() : nullptr)
                tabs->setCurrentIndex(qEnvironmentVariableIntValue("KK_SCREENSHOT_RENDER_TAB"));
        }
    } else if (base == QLatin1String("cut")) {
        // Cut page (spec 35): files in KK_SCREENSHOT_IMPORT are imported; an empty timeline gets
        // every file of the media pool appended
        // (Append, one undo step) and the playhead in its middle. "cut:tape" shows the source
        // tape of the bin, "cut:clip" the first clip in Source Clip mode, "cut:strip" the media
        // pool's strip view.
        window.setCurrentPage(QStringLiteral("cut"));
        CutPage* cut = window.cutPage();
        // KK_SCREENSHOT_IMPORT=a.mp4:b.mp4 imports media first (colon-separated).
        const QStringList importPaths = qEnvironmentVariable("KK_SCREENSHOT_IMPORT")
                                            .split(QLatin1Char(':'), Qt::SkipEmptyParts);
        if (!importPaths.isEmpty())
            ctx.project->importMedia(importPaths);
        QVector<SourceSelection> clips;
        for (const MediaItem& m : ctx.project->mediaPool()->items()) {
            if (m.kind == MediaItemKind::File && (m.info.hasVideo || m.info.hasAudio))
                clips.append(SourceSelection {m.id, 0, 0});
        }
        if (ctx.project->timeline()->data().duration() == 0 && !clips.isEmpty()) {
            cut->editor()->run(CutEdit::Append, clips);
            ctx.selection->clear();
        }
        ctx.project->timeline()->setPlayhead(ctx.project->timeline()->data().duration() / 2);
        ctx.project->undoStack()->setClean(); // demo edits: closing must not ask
        if (section == QLatin1String("tape")) {
            cut->viewer()->setMode(CutViewer::Mode::SourceTape);
            if (clips.size() > 1)
                cut->viewer()->showTapeClip(clips[1].mediaId);
        } else if (section == QLatin1String("clip") && !clips.isEmpty()) {
            cut->viewer()->clipViewer()->setSourceMedia(clips.front().mediaId);
            cut->viewer()->setMode(CutViewer::Mode::SourceClip);
        } else if (section == QLatin1String("strip")) {
            cut->mediaPool()->setViewMode(MediaPoolPanel::ViewMode::Strip);
        }
    } else if (base == QLatin1String("audio")) {
        // Audio page (spec 37): demo mix, meters filled offline; "audio:eq" / "audio:dynamics"
        // open A1's editor window, composited into the grab.
        QString why;
        toolWindow = prepareAudioScreenshot(window, section, &why);
        if (!why.isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(why));
            return 2;
        }
    } else if (base == QLatin1String("color")) {
        // Color page (user guide): "color:<palette>" shows primaries | curves | qualifier |
        // windows | luts, and "color:key" the Key tab beside it. KK_SCREENSHOT_COLOR_NODE=<n>
        // selects node n (its number in the node editor), KK_SCREENSHOT_COLOR_HIGHLIGHT=1
        // turns Highlight on, KK_SCREENSHOT_COLOR_PICK=1 arms the qualifier's Pick,
        // KK_SCREENSHOT_SCOPE=<waveform|parade|...> picks the scope, KK_SCREENSHOT_COLOR_SELECT
        // =<n,m> selects those clips of the clips strip (1-based) and grades the first,
        // KK_SCREENSHOT_COLOR_GRAB=1 grabs a still into the gallery.
        ColorPage* color = window.colorPage();
        if (!color || !window.setCurrentPage(QStringLiteral("color"))) {
            std::fprintf(stderr, "color: no Color page\n");
            return 2;
        }
        const QStringList palettes {QStringLiteral("primaries"), QStringLiteral("curves"),
            QStringLiteral("qualifier"), QStringLiteral("windows"), QStringLiteral("luts")};
        if (const int p = int(palettes.indexOf(section)); p >= 0)
            color->showPalette(ColorPage::Palette(p));
        if (section == QLatin1String("key")) {
            if (auto* tab = color->findChild<QAbstractButton*>(QStringLiteral("ColorRightTab_1")))
                tab->click();
        }
        const QString scope = qEnvironmentVariable("KK_SCREENSHOT_SCOPE");
        if (!scope.isEmpty())
            color->scopes()->setType(scopes::scopeTypeFromId(scope));
        QTimer::singleShot(0, color, [color] {
            ColorSession* session = color->session();
            const QStringList rows = qEnvironmentVariable("KK_SCREENSHOT_COLOR_SELECT")
                                         .split(QLatin1Char(','), Qt::SkipEmptyParts);
            QVector<quint64> clips;
            const QVector<quint64> strip = session->stripClips();
            for (const QString& r : rows) {
                const int row = r.toInt() - 1;
                if (row >= 0 && row < strip.size())
                    clips.append(strip[row]);
            }
            if (!clips.isEmpty()) {
                session->gradeClip(clips.front());
                session->setSelectedClips(clips);
            }
            const int node = qEnvironmentVariableIntValue("KK_SCREENSHOT_COLOR_NODE") - 1;
            if (node >= 0 && node < int(session->grade().nodes.size()))
                session->selectNode(session->grade().nodes[node].id);
            if (qEnvironmentVariableIntValue("KK_SCREENSHOT_COLOR_PICK") == 1)
                color->qualifier()->setPickMode(qualifierpick::Mode::Pick);
            if (qEnvironmentVariableIntValue("KK_SCREENSHOT_COLOR_HIGHLIGHT") == 1)
                color->setHighlight(true);
        });
        // KK_SCREENSHOT_COLOR_GRAB=1: Grab Still once the picture is there (the gallery shot).
        if (qEnvironmentVariableIntValue("KK_SCREENSHOT_COLOR_GRAB") == 1) {
            QTimer::singleShot(1500, color, [&ctx] {
                if (QAction* grab = ctx.actions->action(QStringLiteral("color.grab_still")))
                    grab->trigger();
            });
        }
        delayMs = std::max(delayMs, kCompositingSettleDelayMs);
    } else if (target == QLatin1String("import-dialog")) {
        return grabImportDialog(window, pngPath);
    } else if (!isPage) {
        std::fprintf(stderr,
            "unknown screenshot target '%s' (use media, cut[:tape|clip|strip], edit, "
            "audio[:eq|dynamics], deliver, "
            "preferences[:section], "
            "project-settings[:section], keyboard, inspector, keyframes[:curves|narrow], "
            "keyframes-window, masks[:inspector|viewer], subtitles[:inspector], "
            "multicam[:viewer|timeline], "
            "keyframes-panel[:dope|curves|absolute|float], "
            "mediapool, levels, stacked, clip-attributes[:audio|timecode], "
            "effects[:<id>], fx:<id>[:inspector|viewer], change-speed, timeline, "
            "deliver:inout|queue, import-dialog, project-manager, new-project, menu-color-theme "
            "or "
            "menu-style, menu-playback-resolution, clip:<track>[:<tab>], "
            "menu:<category>[:<submenu>], normalize-audio)\n",
            qPrintable(target));
        return 2;
    }
    if (dialog)
        dialog->show();

    int exitCode = 1;
    QElapsedTimer waited;
    auto grab = [&] {
        // Viewer guides, safe areas and grid are working aids, never part of an output
        // picture (spec 41.12): hidden for the grab whatever the user's settings say.
        GuideOverlay::setSuppressed(true);
        // KK_SCREENSHOT_WINDOW=1: panel targets save the whole window (the panel's state stays).
        if (panel && qEnvironmentVariableIntValue("KK_SCREENSHOT_WINDOW") == 1)
            panel = nullptr;
        // KK_SCREENSHOT_GEOMETRY=<file.json>: where the widgets are in the picture (user guide
        // annotations; not written for menu shots, which are cropped).
        const QString geometryPath = qEnvironmentVariable("KK_SCREENSHOT_GEOMETRY");
        QWidget* root = dialog ? dialog.get() : panel ? panel : &window;
        if (!geometryPath.isEmpty() && menus.isEmpty()
            && !writeScreenshotGeometry(root, geometryPath, {}, &ctx.project->timeline()->data(),
                ctx.project->timeline()->playhead()))
            std::fprintf(stderr, "cannot write %s\n", qPrintable(geometryPath));
        if (!geometryPath.isEmpty() && !menus.isEmpty()
            && !writeMenuGeometry(window, menus, geometryPath))
            std::fprintf(stderr, "cannot write %s\n", qPrintable(geometryPath));
        const QPixmap pixmap = dialog ? dialog->grab()
            : toolWindow              ? grabWithToolWindow(window, toolWindow)
            : panel                   ? panel->grab()
            : menus.isEmpty()         ? window.grab()
                                      : grabWithMenus(window, menus);
        if (pixmap.save(pngPath, "PNG")) {
            exitCode = 0;
        } else {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(pngPath));
        }
        // exit(), not quit(): quit() first closes the window, which would ask to save a project
        // the shot changed (a grabbed still) and wait for an answer forever.
        QCoreApplication::exit(0);
    };
    QTimer poll;
    poll.setInterval(kFramePollMs);
    // KK_SCREENSHOT_PLAY_MS=n: play the first viewer with content for n ms before the grab
    // (headless check of playback, e.g. that KK_PLAYBACK_STATS lines appear in a bundle).
    const int playMs = qEnvironmentVariableIntValue("KK_SCREENSHOT_PLAY_MS");
    auto playThenGrab = [&] {
        PlaybackEngine* engine = nullptr;
        for (ViewerPanel* viewer : ViewerPanel::allViewers()) {
            if (viewer->isVisible() && viewer->engine() && viewer->engine()->duration() > 0) {
                engine = viewer->engine();
                break;
            }
        }
        if (playMs <= 0 || !engine) {
            if (playMs > 0)
                std::fprintf(stderr, "KK_SCREENSHOT_PLAY_MS: no viewer with content to play\n");
            QTimer::singleShot(kFramePollMs, &window, grab); // one more paint
            return;
        }
        engine->play();
        QTimer::singleShot(playMs, &window, [&, engine] {
            engine->stop();
            QTimer::singleShot(kFramePollMs, &window, grab);
        });
    };
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        if (viewersSettled() || waited.elapsed() > kFrameTimeoutMs) {
            poll.stop();
            playThenGrab();
        }
    });
    // The Compositing page viewers render their nodes asynchronously (slow on software GL).
    if (base == QLatin1String("compositing")) {
        delayMs = std::max(delayMs, kCompositingSettleDelayMs);
        // KK_SCREENSHOT_NODE=<node name> (user guide): that node selected (Inspector, handles).
        const QString nodeName = qEnvironmentVariable("KK_SCREENSHOT_NODE");
        CompositingPage* comp = window.compositingPage();
        if (!nodeName.isEmpty() && comp) {
            QTimer::singleShot(0, comp, [comp, nodeName] {
                CompSession* session = comp->session();
                for (const CompNode& node : session->composition().nodes) {
                    if (node.name == nodeName)
                        session->setSelection({node.id}, {}, node.id);
                }
            });
        }
    }
    // KK_SCREENSHOT_DELAY_MS: a longer settle time (thumbnails and posters are made in the
    // background; the user guide waits for them; slow machines, software GL).
    if (qEnvironmentVariableIsSet("KK_SCREENSHOT_DELAY_MS"))
        delayMs = std::max(delayMs, qEnvironmentVariableIntValue("KK_SCREENSHOT_DELAY_MS"));
    QTimer::singleShot(delayMs, &window, [&] {
        waited.start();
        poll.start();
    });
    QApplication::exec();
    return exitCode;
}

} // namespace kk
