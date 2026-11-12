// SPDX-License-Identifier: GPL-3.0-or-later
// MatteMath — the picture maths around the any-subject model (spec 78.9), free of the inference
// runtime so tests can check it without a model: the input tensor (squashed to the model's square,
// ImageNet normalisation), logits to alpha, and the edge refinement that carries the model's
// 1024-pixel matte up to the picture's own resolution.
#pragma once

#include <QImage>

#include <vector>

namespace kk::matting {

/// Fills @p out (3 * size * size floats, planar R, G, B) with @p rgb (Format_RGB888) resized to
/// size x size (smooth, aspect ratio not kept, as BiRefNet's own inference does) and normalised
/// with the ImageNet mean (0.485, 0.456, 0.406) and standard deviation (0.229, 0.224, 0.225).
void birefnetInput(const QImage& rgb, int size, std::vector<float>& out);

/// 1 / (1 + e^-x).
[[nodiscard]] float sigmoid(float x);

/// A Format_Grayscale8 matte of @p size x size from the model's raw output. @p logits true: apply
/// the sigmoid first (BiRefNet's ONNX files return logits).
[[nodiscard]] QImage matteFromOutput(const float* values, int size, bool logits);

/// Resizes @p matte to @p width x @p height (smooth).
[[nodiscard]] QImage resizeMatte(const QImage& matte, int width, int height);

/// Guided filter (K. He et al., "Guided Image Filtering", ECCV 2010; own implementation): the
/// matte follows the edges of @p guide (Format_RGB888, same size) within @p radius pixels. @p eps
/// is the smoothness in 0..1 luma units squared (0.001 default: a bigger value blurs more).
/// Returns a Format_Grayscale8 matte of the same size.
[[nodiscard]] QImage refineMatte(const QImage& matte, const QImage& guide, int radius, float eps);

/// Radius of the refinement for a picture whose long side is @p edge pixels.
[[nodiscard]] int refineRadius(int edge);

} // namespace kk::matting
