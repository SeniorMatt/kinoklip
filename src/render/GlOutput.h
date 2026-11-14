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
    /// Output slots allocated and, of those, held by a live RenderedFrame (any thread; tests,
    /// resource statistics, scripts/check_leaks.sh soak test).
    [[nodiscard]] std::pair<int, int> slotUsage() const
    {
        std::lock_guard lock(m_slots->mutex);
        int used = 0;
        for (const Slot& s : m_slots->entries)
            used += s.inUse ? 1 : 0;
        return {int(m_slots->entries.size()), used};
    }

    /// Deletes all GL objects (pending readbacks are completed first).
    void destroy();

private:
    struct Slot
    {
        GLuint texture = 0;
        GLuint fbo = 0;
        QSize size;
        void* fence = nullptr; ///< GLsync of the last frame rendered into the slot
        bool inUse = false;
    };
    /// Shared with RenderedFrame deleters, which may outlive this object.
    struct SlotTable
    {
        std::mutex mutex;
        std::vector<Slot> entries;
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

    /// Index of a free slot of @p size (allocating or resizing one if needed), marked in use.
    int acquireSlot(QSize size);
    Pbo takePbo(qsizetype bytes);

    /// Above this many simultaneously held frames something is leaking RenderedFramePtrs.
    static constexpr int kSlotWarning = 32;

    GlKit& m_kit;
    std::shared_ptr<SlotTable> m_slots;
    std::deque<Pending> m_pending;
    std::vector<Pbo> m_freePbos;
};

} // namespace kk::render
