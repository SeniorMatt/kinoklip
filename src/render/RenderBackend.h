// SPDX-License-Identifier: GPL-3.0-or-later
// RenderBackend — internal interface between the Compositor's render thread and the code that
// produces pixels (GlRenderer on the GPU, CpuRenderer as the fallback).
#pragma once

#include "render/DecoderPool.h"
#include "render/RenderTypes.h"

#include <QString>

#include <future>
#include <memory>

class QOffscreenSurface;

namespace kk::render {

/// One queued request plus the promise its result goes to.
struct RenderJob
{
    RenderRequest request;
    std::shared_ptr<const MediaPathMap> media; ///< resolved (request.media or the default)
    bool fromCache = false; ///< the request was rewritten to a render cache chunk
    std::promise<RenderedFramePtr> promise;
};

/// What a backend holds that costs memory between frames (GPU objects of an integrated GPU are
/// system memory no process meter shows).
struct ResourceStats
{
    int decoders = 0; ///< open video decoders (each with its frame pool)
    int titleTextures = 0; ///< cached title/caption textures
    int64_t targetBytes = 0; ///< this backend's pooled render targets
    int outputSlots = 0; ///< output textures handed to consumers (viewer, scopes...)
    int outputSlotsInUse = 0; ///< of those, frames still referenced somewhere
    int64_t outputSlotBytes = 0;
    int outputSlotsStolen = 0; ///< slots taken back from a consumer holding too many frames
};

/// Produces frames on the render thread. Every method is called on that one thread.
class RenderBackend
{
public:
    virtual ~RenderBackend() = default;

    /// Creates resources (GL context, shaders...). @return false with a reason if unusable.
    virtual bool initialize(QString* why) = 0;
    /// Human-readable description ("OpenGL 4.6 — Mesa Intel(R) Iris(R) Xe", "CPU (QPainter)").
    [[nodiscard]] virtual QString description() const = 0;
    /// True for the GPU backend (textures available).
    [[nodiscard]] virtual bool isGpu() const = 0;

    /// Renders @p job. Either fulfils `job.promise` before returning or takes it over and
    /// fulfils it later from finishPending() (asynchronous PBO readback).
    virtual void render(RenderJob& job) = 0;
    /// The decoders behind render(), for the look-ahead (RenderRequest::prepare).
    [[nodiscard]] virtual DecoderPool& decoders() = 0;
    /// Completes deferred work. @p keepNewest: leave the most recent pending frame in flight
    /// (more requests are queued, so its readback can overlap the next render).
    virtual void finishPending(bool keepNewest) { Q_UNUSED(keepNewest); }
    /// True if finishPending() has work to do.
    [[nodiscard]] virtual bool hasPending() const { return false; }
    /// Called when no request has come for a while: gives back what only speeds up the next
    /// frames (open decoders and their frame pools, idle render targets, title textures). GPU
    /// memory of an integrated GPU is system memory that no process meter shows.
    virtual void releaseIdle() { }
    /// What is held right now.
    [[nodiscard]] virtual ResourceStats resources() const { return {}; }
    /// Releases everything (still on the render thread, before it exits).
    virtual void shutdown() { }
};

/// GPU backend using @p surface (created on the GUI thread, outlives the backend).
[[nodiscard]] std::unique_ptr<RenderBackend> createGlBackend(QOffscreenSurface* surface);
/// QPainter fallback backend.
[[nodiscard]] std::unique_ptr<RenderBackend> createCpuBackend();

} // namespace kk::render
