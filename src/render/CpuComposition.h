// SPDX-License-Identifier: GPL-3.0-or-later
// CpuComposition — the QPainter / per-pixel fallback evaluation of a Compositing page node
// graph (used by CpuRenderer when no OpenGL 3.3 context exists). Same geometry and colour
// formulas as the GPU path (render/CompMath.h); gaps are listed in src/render/readme.md.
#pragma once

#include "core/Composition.h"

#include <QImage>
#include <QSet>
#include <QSize>
#include <QSizeF>

#include <functional>

namespace kk::render {

/// One CPU evaluation request (the twin of CompEval).
struct CpuCompEval
{
    const Composition* comp = nullptr;
    int64_t compFrame = 0;
    QSizeF frame; ///< timeline size
    QSize size; ///< image pixels
    double scale = 1.0; ///< image pixels per timeline pixel
    double fps = 25.0;
    /// The picture of a MediaIn node (an image of `size`, the source fitted into the frame);
    /// a null image = nothing.
    std::function<QImage(const CompNode& node, int64_t compFrame)> mediaIn;
    /// Node types already reported as not supported here (one warning each); may be null.
    QSet<QString>* warned = nullptr;
};

/// The image of node @p nodeId (ARGB32_Premultiplied, `ev.size`, transparent where the node
/// draws nothing). Supported: every built-in node (masks with hard edges: soft edge ignored)
/// and the pointwise effect nodes (fxmath::applyPointEffect()); other effect nodes pass
/// their input through.
[[nodiscard]] QImage evaluateCompositionCpu(const CpuCompEval& ev, quint64 nodeId);

} // namespace kk::render
