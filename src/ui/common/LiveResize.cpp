// SPDX-License-Identifier: GPL-3.0-or-later
// LiveResize — the Wayland swap interval that lets the window content keep up while resizing.
#include "ui/common/LiveResize.h"

#include <QGuiApplication>
#include <QSurfaceFormat>

namespace kk::liveresize {

void adjustSwapInterval()
{
    const bool overridden = qEnvironmentVariableIsSet("KK_SWAP_INTERVAL");
    if (!overridden && !QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        return;
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(overridden ? qEnvironmentVariableIntValue("KK_SWAP_INTERVAL") : 0);
    QSurfaceFormat::setDefaultFormat(format);
}

} // namespace kk::liveresize
