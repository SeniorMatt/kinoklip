// SPDX-License-Identifier: GPL-3.0-or-later
// RenderFrameCommand — implementation of the --render-frame command-line mode.
#include "render/RenderFrameCommand.h"

#include "core/Project.h"
#include "render/Compositor.h"

#include <cstdio>

namespace kk {

int runRenderFrame(const QString& projectPath, int64_t frame, const QString& outPath, QSize outSize,
    Quality quality)
{
    Project project;
    QString error;
    if (!project.load(projectPath, &error)) {
        std::fprintf(stderr, "render-frame: %s\n", qPrintable(error));
        return 1;
    }
    Compositor compositor;
    compositor.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
    const RenderedFramePtr result
        = compositor.render(project.timeline()->snapshot(), frame, outSize, quality, OutputImage);
    if (!result || !result->ok || result->image.isNull()) {
        std::fprintf(stderr, "render-frame: rendering failed\n");
        return 1;
    }
    if (!result->image.save(outPath)) {
        std::fprintf(stderr, "render-frame: cannot write %s\n", qPrintable(outPath));
        return 1;
    }
    std::printf("render-frame: frame %lld %dx%d in %.1f ms [%s] -> %s\n",
        static_cast<long long>(frame), result->size.width(), result->size.height(),
        result->renderMs, qPrintable(compositor.backendDescription()), qPrintable(outPath));
    return 0;
}

} // namespace kk
