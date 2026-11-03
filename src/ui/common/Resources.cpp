// SPDX-License-Identifier: GPL-3.0-or-later
// Resource initialisation (Q_INIT_RESOURCE must be used outside any namespace).
#include "ui/common/Resources.h"

#include <QtGlobal>

// Global scope on purpose: Q_INIT_RESOURCE declares `extern int qInitResources_resources()` at
// block scope, which would get internal linkage inside an anonymous namespace.
static void initKinoklipResources()
{
    Q_INIT_RESOURCE(resources);
    Q_INIT_RESOURCE(guide); // resources/guide.qrc: the user guide's index (src/ui/help)
}

namespace kk {

void initResources()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    initKinoklipResources();
}

} // namespace kk
