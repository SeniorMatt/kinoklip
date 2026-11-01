// SPDX-License-Identifier: GPL-3.0-or-later
// GlOutput — output slots (zero-copy textures for the viewer) and PBO readback (export).
#include "render/GlOutput.h"

#include "render/RenderLog.h"

#include <cstring>

namespace kk::render {

namespace {

/// Upper bound for waiting on a readback fence (a hung GPU must not hang the app forever).
constexpr GLuint64 kReadbackTimeoutNs = 5'000'000'000ull;

GLsync toSync(void* p)
{
    return static_cast<GLsync>(p);
}

} // namespace

GlOutput::GlOutput(GlKit& kit)
    : m_kit(kit)
    , m_slots(std::make_shared<SlotTable>())
{ }

GlOutput::~GlOutput() = default;

int GlOutput::acquireSlot(QSize size)
{
    GlFunctions* gl = m_kit.gl();
    int index = -1;
    bool needsStorage = false;
    {
        std::lock_guard lock(m_slots->mutex);
        auto& entries = m_slots->entries;
        int anyFree = -1;
        for (int i = 0; i < int(entries.size()); ++i) {
            if (entries[size_t(i)].inUse)
                continue;
            if (entries[size_t(i)].size == size) {
                index = i;
                break;
            }
            if (anyFree < 0)
                anyFree = i;
        }
        if (index < 0) {
            needsStorage = true;
            if (anyFree >= 0) {
                index = anyFree;
            } else {
                entries.push_back({});
                index = int(entries.size()) - 1;
                if (index == kSlotWarning)
                    qCWarning(lcRender)
                        << kSlotWarning
                        << "output frames held at once; are RenderedFramePtrs leaking?";
            }
        }
        entries[size_t(index)].inUse = true;
    }
    // GL work outside the lock (deleters only ever touch `inUse`, under the lock).
    Slot& slot = m_slots->entries[size_t(index)];
    if (slot.fence) {
        gl->glDeleteSync(toSync(slot.fence)); // the consumer released the frame: fence unused
        slot.fence = nullptr;
    }
    if (needsStorage) {
        if (!slot.texture) {
            gl->glGenTextures(1, &slot.texture);
            gl->glGenFramebuffers(1, &slot.fbo);
        }
        gl->glBindTexture(GL_TEXTURE_2D, slot.texture);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA,
            GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
        gl->glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
        slot.size = size;
    }
    return index;
}

std::shared_ptr<RenderedFrame> GlOutput::produce(
    const RenderTarget& accumulator, bool wantTexture, PooledTarget& scratch, GLuint* readFbo)
{
    GlFunctions* gl = m_kit.gl();
    const QSize size = accumulator.size;
    int slotIndex = -1;
    GLuint fbo = 0;
    if (wantTexture) {
        slotIndex = acquireSlot(size);
        fbo = m_slots->entries[size_t(slotIndex)].fbo;
    } else {
        scratch = m_kit.acquire(size, GL_RGBA8);
        fbo = scratch->fbo;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glViewport(0, 0, size.width(), size.height());
    m_kit.setBlendOver(false);
    m_kit.blit(accumulator.texture, QTransform(2, 0, 0, 2, -1, -1)); // RGBA16F → RGBA8

    // The deleter recycles the slot when the last reference goes away (any thread).
    std::shared_ptr<RenderedFrame> frame;
    if (slotIndex >= 0) {
        std::weak_ptr<SlotTable> table = m_slots;
        frame.reset(new RenderedFrame, [table, slotIndex](RenderedFrame* f) {
            if (auto t = table.lock()) {
                std::lock_guard lock(t->mutex);
                t->entries[size_t(slotIndex)].inUse = false;
            }
            delete f;
        });
        Slot& slot = m_slots->entries[size_t(slotIndex)];
        slot.fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        frame->texture = slot.texture;
        frame->fence = slot.fence;
        // Flush so the fence (and the commands before it) reach the GPU; another context
        // waiting on an unflushed fence could wait forever.
        gl->glFlush();
    } else {
        frame = std::make_shared<RenderedFrame>();
    }
    frame->size = size;
    frame->ok = true;
    *readFbo = fbo;
    return frame;
}

GlOutput::Pbo GlOutput::takePbo(qsizetype bytes)
{
    GlFunctions* gl = m_kit.gl();
    Pbo pbo;
    if (!m_freePbos.empty()) {
        pbo = m_freePbos.back();
        m_freePbos.pop_back();
    } else {
        gl->glGenBuffers(1, &pbo.id);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo.id);
    if (pbo.bytes != bytes) {
        gl->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        pbo.bytes = bytes;
    }
    return pbo;
}

void GlOutput::startReadback(
    RenderJob& job, std::shared_ptr<RenderedFrame> frame, GLuint fbo, const QElapsedTimer& timer)
{
    GlFunctions* gl = m_kit.gl();
    const QSize size = frame->size;
    const qsizetype bytes = qsizetype(size.width()) * size.height() * 4;
    const Pbo pbo = takePbo(bytes);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    // BGRA bytes = QImage::Format_ARGB32_Premultiplied on little-endian; framebuffer row 0 is
    // the top of the picture (GlKit convention), so the rows arrive in QImage order.
    gl->glReadPixels(0, 0, size.width(), size.height(), GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    Pending p;
    p.job = std::move(job);
    p.frame = std::move(frame);
    p.pbo = pbo;
    p.fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl->glFlush();
    p.submitMs = double(timer.nsecsElapsed()) / 1e6;
    m_pending.push_back(std::move(p));
}

void GlOutput::finishPending(bool keepNewest)
{
    GlFunctions* gl = m_kit.gl();
    while (m_pending.size() > (keepNewest ? 1u : 0u)) {
        Pending p = std::move(m_pending.front());
        m_pending.pop_front();
        QElapsedTimer wait;
        wait.start();
        gl->glClientWaitSync(toSync(p.fence), GL_SYNC_FLUSH_COMMANDS_BIT, kReadbackTimeoutNs);
        gl->glDeleteSync(toSync(p.fence));
        const QSize size = p.frame->size;
        const qsizetype bytes = qsizetype(size.width()) * size.height() * 4;
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, p.pbo.id);
        const void* data = gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (data) {
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            // bytesPerLine == width·4 for 32-bit images, so the PBO is one contiguous copy.
            std::memcpy(image.bits(), data, size_t(bytes));
            p.frame->image = std::move(image);
            gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        } else {
            qCWarning(lcRender) << "readback mapping failed";
        }
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_freePbos.push_back(p.pbo);
        p.frame->renderMs = p.submitMs + double(wait.nsecsElapsed()) / 1e6;
        p.job.promise.set_value(std::move(p.frame));
    }
}

void GlOutput::destroy()
{
    finishPending(false);
    GlFunctions* gl = m_kit.gl();
    for (const Pbo& pbo : m_freePbos)
        gl->glDeleteBuffers(1, &pbo.id);
    m_freePbos.clear();
    std::lock_guard lock(m_slots->mutex);
    for (Slot& slot : m_slots->entries) {
        if (slot.fence)
            gl->glDeleteSync(toSync(slot.fence));
        if (slot.fbo)
            gl->glDeleteFramebuffers(1, &slot.fbo);
        if (slot.texture)
            gl->glDeleteTextures(1, &slot.texture);
        slot = Slot {};
        slot.inUse = true; // never hand out again (frames still alive keep a dead texture id)
    }
}

} // namespace kk::render
