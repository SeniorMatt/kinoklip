// SPDX-License-Identifier: GPL-3.0-or-later
// Stabilizer — measures the camera movement of a clip for Stabilization (core/Stabilization.h).
// Every pair of neighbouring source frames goes through the PlanarTracker (a grid of patches over
// the whole picture, matched with the PointTracker's matcher, RANSAC against moving objects); the
// patches that agree are then fitted with the model of the chosen Mode (a move, a move + turn +
// scale, or the homography itself). The pair motions are chained into the camera path. Standard
// methods (the same idea as FFmpeg's vidstab and Blender's 2D stabilization, which also chain
// frame-to-frame estimates); written for Kinoklip, no code taken from other projects.
// Pure maths on grey images: no widgets, no threads of its own, no project types.
#pragma once

#include "compositing/tracking/PointTracker.h"
#include "core/Rational.h"
#include "core/Stabilization.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace kk::tracking {

/// Motion of the scene between two frames, in normalised picture coordinates (origin at the
/// picture centre, one unit = the picture width): a point at p in the first frame is at m * p in
/// the second.
struct FrameMotion
{
    bool ok = false; ///< false: the frames could not be related (the motion is the identity)
    stabilize::Mat3 m;
    int inliers = 0; ///< patches that agree
};

/// Estimates the motion between two frames with the model of @p mode.
[[nodiscard]] FrameMotion estimateFrameMotion(
    const GrayImage& previous, const GrayImage& next, stabilize::Mode mode);

/// What to measure. Frames are numbered at @p fps (the source's rate).
struct StabilizeRequest
{
    stabilize::Mode mode = stabilize::Mode::Perspective;
    Rational fps {25, 1};
    int64_t firstFrame = 0;
    int64_t lastFrame = 0; ///< inclusive
};

struct StabilizeOutcome
{
    std::shared_ptr<const stabilize::Data> data; ///< null on failure or cancel
    QString error; ///< a sentence for the user when data is null and it was not cancelled
    bool cancelled = false;
    int pairs = 0; ///< frame pairs measured
    int lost = 0; ///< ... of which could not be related (treated as no movement)
};

/// Grey picture of source frame @p frame (any size, same for every frame); empty on failure.
using GrayFrameFn = std::function<GrayImage(int64_t frame)>;
/// Called after each frame: @p done of @p total.
using StabilizeProgressFn = std::function<void(int done, int total)>;

/// Measures frames [firstFrame, lastFrame] and returns the camera path. @p cancel (may be null) is
/// checked between frames. Needs at least two frames.
[[nodiscard]] StabilizeOutcome stabilizeAnalyse(const StabilizeRequest& request,
    const GrayFrameFn& frameAt, const StabilizeProgressFn& progress = {},
    const std::atomic<bool>* cancel = nullptr);

} // namespace kk::tracking
