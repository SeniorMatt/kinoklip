// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation: pages, dialogs and open menus rendered to PNG.
#include "app/Screenshot.h"

#include "app/MainWindow.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace kk {

namespace {
const QSize kScreenshotSize(1920, 1080);
constexpr int kSettleDelayMs = 400; // let layouts, file system model and GL widgets settle
constexpr int kDialogSettleDelayMs = 1500; // Preferences probes the GPU on a worker thread
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
    } else if (target == QLatin1String("keyframes")
        || target == QLatin1String("keyframes-window")) {
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
        lanes->setKeyframeEditorOpen(id, true);
        lanes->setCurveEditorOpen(id, true);
        if (target == QLatin1String("keyframes"))
            panel = timelinePanel;
    } else if (target == QLatin1String("mediapool")) {
        panel = window.findChild<MediaPoolPanel*>();
    } else if (target == QLatin1String("effects")) {
        if (Page* edit = window.page(QStringLiteral("edit")))
            edit->setPanelVisible(PanelId::Effects, true);
        panel = window.findChild<EffectsLibraryPanel*>();
    } else if (!isPage) {
        std::fprintf(stderr,
            "unknown screenshot target '%s' (use media, edit, deliver, preferences[:section], "
            "project-settings[:section], keyboard, inspector, keyframes, keyframes-window, "
            "mediapool, "
            "effects, menu-color-theme "
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
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        if (viewersSettled() || waited.elapsed() > kFrameTimeoutMs) {
            poll.stop();
            QTimer::singleShot(kFramePollMs, &window, grab); // one more paint
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
