// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation: pages, dialogs and open menus rendered to PNG.
#include "app/Screenshot.h"

#include "app/MainWindow.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
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

} // namespace

int runScreenshot(MainWindow& window, const QString& target, const QString& pngPath)
{
    const bool isPage = window.setCurrentPage(target);
    std::unique_ptr<QWidget> dialog;
    QWidget* panel = nullptr; // panel targets: only this widget is saved
    QList<QMenu*> menus;
    if (!isPage)
        window.setCurrentPage(QStringLiteral("edit"));
    window.resize(kScreenshotSize);
    window.show();

    AppContext& ctx = window.context();
    int delayMs = kSettleDelayMs;
    if (target == QLatin1String("preferences")) {
        dialog = std::make_unique<PreferencesDialog>(ctx.settingsPages, &window);
        delayMs = kDialogSettleDelayMs;
    } else if (target == QLatin1String("keyboard")) {
        dialog = std::make_unique<KeyboardCustomizationDialog>(*ctx.actions, &window);
    } else if (target == QLatin1String("menu-color-theme")
        || target == QLatin1String("menu-style")) {
        QMenu* settings = findMenu(&window, QStringLiteral("settings"));
        QMenu* sub = findMenu(&window,
            target == QLatin1String("menu-style") ? QStringLiteral("settings.style")
                                                  : QStringLiteral("settings.color_scheme"));
        if (!settings || !sub) {
            std::fprintf(stderr, "no Settings menu\n");
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
    } else if (target == QLatin1String("mediapool")) {
        panel = window.findChild<MediaPoolPanel*>();
    } else if (!isPage) {
        std::fprintf(stderr,
            "unknown screenshot target '%s' (use media, edit, deliver, preferences, keyboard, "
            "inspector, mediapool, menu-color-theme or menu-style)\n",
            qPrintable(target));
        return 2;
    }
    if (dialog)
        dialog->show();

    int exitCode = 1;
    QTimer::singleShot(delayMs, &window, [&] {
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
    });
    QApplication::exec();
    return exitCode;
}

} // namespace kk
