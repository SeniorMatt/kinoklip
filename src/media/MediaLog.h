// SPDX-License-Identifier: GPL-3.0-or-later
// MediaLog — the logging category of the media engine ("cw.media").
#pragma once

#include <QLoggingCategory>

namespace cw::media {

/// Logging category used by everything in `src/media`.
/// Enable verbose output with `QT_LOGGING_RULES="cw.media.debug=true"`.
/// FFmpeg's own log messages are routed into this category as well (see FFmpegUtil.cpp).
Q_DECLARE_LOGGING_CATEGORY(lcMedia)

} // namespace cw::media
