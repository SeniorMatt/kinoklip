// SPDX-License-Identifier: GPL-3.0-or-later
// GlDmaBuf — imports media::DmaBufFrame planes (VAAPI surfaces exported as DMA-BUF) into the
// plane textures of GlVideo through EGLImage: the decoded picture never travels through system
// memory. The EGL calls are looked up at run time (no libEGL or header dependency); everything
// that needs a GPU is behind probe() / import(), the rest is pure and tested.
//
// The approach (one EGLImage per plane, R8 + GR88 for NV12, R16 + GR1616 for P010, sampled
// by the same YUV -> RGB shader as uploaded frames) is the one VLC (modules/video_output/opengl,
// interop_vaapi.c) and mpv (video/out/opengl/hwdec_vaapi.c) use; only the idea is shared, the
// code is our own.
#pragma once

#include "media/VideoFrame.h"
#include "render/GlKit.h"
#include "render/GlVideo.h"

#include <QString>

#include <cstdint>
#include <vector>

class QOpenGLContext;

namespace kk::render {

/// EGL_EXT_image_dma_buf_import attribute list (EGL_NONE terminated) for plane @p plane of
/// @p frame. Modifiers are given only when @p withModifiers (EGL_EXT_image_dma_buf_import_
/// modifiers) and the modifier is not DRM_FORMAT_MOD_INVALID. Pure.
[[nodiscard]] std::vector<int32_t> dmaBufImageAttributes(
    const media::DmaBufFrame::Plane& plane, bool withModifiers);

/// True if @p extensions (the space-separated EGL_EXTENSIONS string) lists @p name. Pure.
[[nodiscard]] bool hasExtension(const QString& extensions, const char* name);

/// "R8", "GR88", "R16", "GR1616" for a DRM fourcc (log lines); the hex value otherwise.
[[nodiscard]] QString drmFormatName(uint32_t fourcc);

/// DMA-BUF import into GL textures; one per GL renderer (render thread, its context current).
class GlDmaBuf
{
public:
    /// Checks the current context: an EGL context (Wayland, or X11 with QT_XCB_GL_INTEGRATION=
    /// xcb_egl), EGL_EXT_image_dma_buf_import and GL_OES_EGL_image. @return false with the
    /// reason in @p why (always a sentence for the log).
    bool probe(QOpenGLContext* context, QString* why);

    /// True after a successful probe() and until an import fails.
    [[nodiscard]] bool usable() const { return m_usable; }

    /// Makes @p textures show the planes of @p frame (frame.dmabuf set). On failure nothing is
    /// changed, usable() turns false and @p why says what the driver answered.
    [[nodiscard]] bool import(
        GlKit& kit, PlaneTextures& textures, const media::VideoFrame& frame, QString* why);

private:
    bool m_usable = false;
    bool m_modifiers = false;
    QOpenGLContext* m_context = nullptr;
    void* m_display = nullptr; ///< EGLDisplay
    void* m_createImage = nullptr; ///< eglCreateImageKHR
    void* m_destroyImage = nullptr; ///< eglDestroyImageKHR
    void* m_getError = nullptr; ///< eglGetError
    void* m_targetTexture = nullptr; ///< glEGLImageTargetTexture2DOES
};

} // namespace kk::render
