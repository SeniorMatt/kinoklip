// SPDX-License-Identifier: GPL-3.0-or-later
// Logging categories of the core module (enable with QT_LOGGING_RULES="kk.core.debug=true").
#pragma once

#include <QLoggingCategory>

namespace kk {

/// Logging category for the data model, edit operations, undo and serialization ("kk.core").
Q_DECLARE_LOGGING_CATEGORY(lcCore)

} // namespace kk
