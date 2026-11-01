// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation.
#include "app/Screenshot.h"

#include "app/MainWindow.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/viewer/ViewerPanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

namespace kk {

namespace {
const QSize kScreenshotSize(1920, 1080);
constexpr int kSettleDelayMs = 400; // let layouts, file system model and GL widgets settle
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

int runScreenshot(MainWindow& window, const QString& pageId, const QString& pngPath)
{
    if (!window.setCurrentPage(pageId)) {
        std::fprintf(
            stderr, "unknown page '%s' (use media, edit or deliver)\n", qPrintable(pageId));
        return 2;
    }
    window.resize(kScreenshotSize);
    window.show();

    int exitCode = 1;
    QElapsedTimer waited;
    auto grab = [&] {
        const QPixmap pixmap = window.grab();
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
    QTimer::singleShot(kSettleDelayMs, &window, [&] {
        waited.start();
        poll.start();
    });
    QApplication::exec();
    return exitCode;
}

} // namespace kk
