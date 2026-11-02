// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation: pages, dialogs and open menus rendered to PNG.
#include "app/Screenshot.h"

#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/RetimeOps.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace kk {

namespace {
const QSize kScreenshotSize(1920, 1080);
constexpr int kSettleDelayMs = 400; // let layouts, file system model and GL widgets settle
constexpr int kDialogSettleDelayMs = 1500;
constexpr int kImportDialogDelayMs = 6000; // the portal and its dialog process start up //
                                           // Preferences probes the GPU on a worker thread
constexpr int kMenuMargin = 24; // px of window around an open menu in menu shots

QMenu* findMenu(QWidget* root, const QString& category)
{
    return root->findChild<QMenu*>(QStringLiteral("menu:") + category);
}

/// Window grab with the open popup menus painted on top (popups are separate windows, so
/// QWidget::grab() of the main window does not contain them), cropped around the menus.
QPixmap grabWithMenus(QWidget& window, const QList<QMenu*>& menus)
{
    QPixmap shot = window.grab();
    QPainter painter(&shot);
    QRect area;
    for (QMenu* menu : menus) {
        const QRect r(window.mapFromGlobal(menu->mapToGlobal(QPoint(0, 0))), menu->size());
        painter.drawPixmap(r.topLeft(), menu->grab());
        area |= r;
    }
    painter.end();
    area = area.adjusted(-kMenuMargin, -kMenuMargin, kMenuMargin, kMenuMargin);
    area.setTop(0); // keep the menu bar in the picture
    area.setLeft(0);
    return shot.copy(area & shot.rect());
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

} // namespace

int runScreenshot(MainWindow& window, const QString& target, const QString& pngPath)
{
    const bool isPage = window.setCurrentPage(target);
    std::unique_ptr<QWidget> dialog;
    QWidget* panel = nullptr; // panel targets: only this widget is saved
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
    } else if (target == QLatin1String("keyboard")) {
        dialog = std::make_unique<KeyboardCustomizationDialog>(*ctx.actions, &window);
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
    } else if (target == QLatin1String("mediapool")) {
        panel = window.findChild<MediaPoolPanel*>();
    } else if (target == QLatin1String("effects")) {
        if (Page* edit = window.page(QStringLiteral("edit")))
            edit->setPanelVisible(PanelId::Effects, true);
        panel = window.findChild<EffectsLibraryPanel*>();
    } else if (target == QLatin1String("import-dialog")) {
        return grabImportDialog(window, pngPath);
    } else if (!isPage) {
        std::fprintf(stderr,
            "unknown screenshot target '%s' (use media, edit, deliver, preferences[:section], "
            "project-settings[:section], keyboard, inspector, keyframes[:curves|narrow], "
            "keyframes-window, masks[:inspector|viewer], subtitles[:inspector], "
            "keyframes-panel[:dope|curves|absolute|float], "
            "mediapool, "
            "effects, import-dialog, menu-color-theme "
            "or "
            "menu-style, menu-playback-resolution)\n",
            qPrintable(target));
        return 2;
    }
    if (dialog)
        dialog->show();

    int exitCode = 1;
    QElapsedTimer waited;
    auto grab = [&] {
        const QPixmap pixmap = dialog ? dialog->grab()
            : panel                   ? panel->grab()
            : menus.isEmpty()         ? window.grab()
                                      : grabWithMenus(window, menus);
        if (pixmap.save(pngPath, "PNG")) {
            exitCode = 0;
        } else {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(pngPath));
        }
        QApplication::quit();
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
    QTimer::singleShot(delayMs, &window, [&] {
        waited.start();
        poll.start();
    });
    QApplication::exec();
    return exitCode;
}

} // namespace kk
