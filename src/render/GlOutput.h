// SPDX-License-Identifier: GPL-3.0-or-later
// GlOutput — final stage of the GPU compositor: 8-bit output textures handed to consumers
// (recycled when the last RenderedFramePtr goes away) and asynchronous PBO readback to QImage.
#pragma once

#include "render/GlKit.h"
#include "render/RenderBackend.h"

#include <QElapsedTimer>

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace kk::render {

/// Output textures + readback. Render thread only, except the slot release that runs from the
/// RenderedFrame deleter on whatever thread drops the last reference (mutex-protected; it only
/// flips a flag, GL objects are touched on the render thread exclusively).
class GlOutput
{
public:
    explicit GlOutput(GlKit& kit);
    ~GlOutput();

    /// Converts the RGBA16F accumulator into the final 8-bit picture and creates the result.
    /// With @p wantTexture the picture goes into a recycled output slot whose texture and
    /// fence are stored in the frame; otherwise into a pooled scratch target kept in
    /// @p scratch. @p readFbo receives the framebuffer holding the 8-bit picture.
    [[nodiscard]] std::shared_ptr<RenderedFrame> produce(
        const RenderTarget& accumulator, bool wantTexture, PooledTarget& scratch, GLuint* readFbo);

    /// Starts an asynchronous readback of @p fbo (size of @p frame) into a pixel buffer object;
    /// the job's promise is fulfilled by finishPending().
    void startReadback(RenderJob& job, std::shared_ptr<RenderedFrame> frame, GLuint fbo,
        const QElapsedTimer& timer);
    /// Maps finished readbacks into QImages and fulfils their promises (see RenderBackend).
    void finishPending(bool keepNewest);
    [[nodiscard]] bool hasPending() const { return !m_pending.empty(); }

    /// Deletes all GL objects (pending readbacks are completed first).
    void destroy();

    /// Output slots right now: allocated, handed out and not yet released, and their GPU bytes
    /// (any thread).
    struct SlotStats
    {
        int allocated = 0;
        int inUse = 0;
        int64_t bytes = 0;
        int stolen = 0; ///< slots taken back from a consumer that held them past the cap
    };
    [[nodiscard]] SlotStats slotStats() const;

    /// Most slots (frames held at once by consumers) before the oldest one is taken back, and
    /// the most GPU memory the slots may use once there are kMinSlots.
    static constexpr int kMaxSlots = 32;
    static constexpr int kMinSlots = 8;
    static constexpr int64_t kMaxSlotBytes = 640ll << 20;

private:
    struct Slot
    {
        GLuint texture = 0;
        GLuint fbo = 0;
        QSize size;
        void* fence = nullptr; ///< GLsync of the last frame rendered into the slot
        bool inUse = false;
        uint64_t owner = 0; ///< serial of the frame that holds the slot (see acquireSlot())
        uint64_t lastUse = 0; ///< acquire counter at the last hand-out
    };
    /// Shared with RenderedFrame deleters, which may outlive this object.
    struct SlotTable
    {
        mutable std::mutex mutex;
        std::vector<Slot> entries;
        uint64_t acquires = 0; ///< hand-outs so far; also the next owner serial
        int stolen = 0;
    };
    struct Pbo
    {
        GLuint id = 0;
        qsizetype bytes = 0;
    };
    struct Pending
    {
        RenderJob job;
        std::shared_ptr<RenderedFrame> frame;
        Pbo pbo;
        void* fence = nullptr;
        double submitMs = 0; ///< render-thread time until the readback was queued
    };

    /// Index of a free slot of @p size (allocating or resizing one if needed), marked in use;
    /// the serial of the new owner goes to @p owner.
    int acquireSlot(QSize size, uint64_t* owner);
    /// Frees the GL objects of free slots whose size has not been asked for lately (the viewer
    /// changed size, e.g. full screen): they would otherwise stay at the old size for good.
    void trimStale(QSize size);
    void freeStorage(Slot& slot);
    Pbo takePbo(qsizetype bytes);

    /// Above this many simultaneously held frames something is leaking RenderedFramePtrs.
    static constexpr int kSlotWarning = 24;
    /// A free slot of another size than the one asked for is freed after this many hand-outs
    /// without one of its size (about 2 s of playback).
    static constexpr uint64_t kStaleAcquires = 60;

    GlKit& m_kit;
    std::shared_ptr<SlotTable> m_slots;
    std::deque<Pending> m_pending;
    std::vector<Pbo> m_freePbos;
};

} // namespace kk::render
