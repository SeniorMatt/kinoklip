// SPDX-License-Identifier: GPL-3.0-or-later
// ExportFFmpeg — one-time FFmpeg initialisation for the export engine.
#include "export/ExportFFmpeg.h"

// The media engine owns the process-wide FFmpeg log routing; reuse it instead of installing a
// second (conflicting) av_log callback. This is the only internal media header used here.
#include "media/FFmpegUtil.h"

namespace kk::exporter {

void initExportFfmpeg()
{
    media::detail::initFfmpeg();
}

} // namespace kk::exporter
