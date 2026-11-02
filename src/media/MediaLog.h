// SPDX-License-Identifier: GPL-3.0-or-later
// MediaLog — the logging category of the media engine ("kk.media").
#pragma once

#include <QLoggingCategory>

namespace kk::media {

/// Logging category used by everything in `src/media`.
/// Info and above are shown by default; verbose output (seeks, FFmpeg warnings, captured
/// hardware probe messages) with `QT_LOGGING_RULES="kk.media.debug=true"`.
/// FFmpeg's own log messages are routed into this category as well (see FFmpegUtil.cpp).
Q_DECLARE_LOGGING_CATEGORY(lcMedia)

} // namespace kk::media
