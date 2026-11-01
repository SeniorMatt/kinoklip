// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode implementation.
#include "app/Screenshot.h"

#include "app/MainWindow.h"

#include <QApplication>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

namespace kk {

namespace {
const QSize kScreenshotSize(1920, 1080);
constexpr int kSettleDelayMs = 400; // let layouts, file system model and GL widgets settle
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
    QTimer::singleShot(kSettleDelayMs, &window, [&] {
        const QPixmap pixmap = window.grab();
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
