// SPDX-License-Identifier: GPL-3.0-or-later
// Logging categories of the core module (enable with QT_LOGGING_RULES="cw.core.debug=true").
#pragma once

#include <QLoggingCategory>

namespace cw {

/// Logging category for the data model, edit operations, undo and serialization ("cw.core").
Q_DECLARE_LOGGING_CATEGORY(lcCore)

} // namespace cw
