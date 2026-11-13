// SPDX-License-Identifier: GPL-3.0-or-later
// IndividualClips - Deliver "Individual clips": the render range split into one job per clip.
#pragma once

#include "export/ExportSettings.h"
#include "render/RenderTypes.h"

#include <QList>
#include <QString>

namespace kk {

struct TimelineData;

/// One file of an "Individual clips" render.
struct ClipRenderJob
{
    QString name; ///< shown in the queue ("Timeline - clip")
    ExportSettings settings; ///< custom range [clip - handles, clip + handles), own output path
};

/// The clips of the render range of @p settings (settings.frameRange()), one job each.
///
/// The clips are the cuts of the picture: at every frame the topmost enabled video-track
/// clip counts, and a run of frames of the same clip is one clip (clips on lower tracks that
/// are covered are not rendered, a gap renders nothing). A clip cut by the range edge is
/// rendered from the edge. Every job renders the clip's frames plus settings.clipHandles frames
/// before and after, limited to the timeline (0 .. duration); the handle frames show what
/// the timeline has there. Files go to the folder of settings.outputPath, named by
/// settings.clipNaming with the extension of outputPath; equal names get "_2", "_3" ...
/// @p media maps media ids to files (the "source name").
[[nodiscard]] QList<ClipRenderJob> individualClipJobs(
    const TimelineData& timeline, const ExportSettings& settings, const MediaPathMap& media);

} // namespace kk
