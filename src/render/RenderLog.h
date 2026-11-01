// SPDX-License-Identifier: GPL-3.0-or-later
// RenderLog — the logging category of the render module ("kk.render").
#pragma once

#include <QLoggingCategory>

namespace kk {

/// Logging category used by everything in `src/render`.
/// Enable verbose output with `QT_LOGGING_RULES="kk.render.debug=true"`.
Q_DECLARE_LOGGING_CATEGORY(lcRender)

} // namespace kk
