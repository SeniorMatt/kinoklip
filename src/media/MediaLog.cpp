// SPDX-License-Identifier: GPL-3.0-or-later
// MediaLog — definition of the "kk.media" logging category.
#include "media/MediaLog.h"

namespace kk::media {

// Info and above by default: debug output (seeks, FFmpeg warnings) needs
// QT_LOGGING_RULES="kk.media.debug=true".
Q_LOGGING_CATEGORY(lcMedia, "kk.media", QtInfoMsg)

} // namespace kk::media
