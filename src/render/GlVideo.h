// SPDX-License-Identifier: GPL-3.0-or-later
// GlVideo — uploads media::VideoFrame planes into persistent textures and draws them with the
// YUV → RGB conversion shader (BT.601/709/2020, limited/full range, 8/10-bit, BGRA; HLG / PQ
// / BT.2020 frames mapped to SDR BT.709, media/HdrToSdr.h).
#pragma once

#include "core/ClipAttributes.h"
#include "media/VideoFrame.h"
#include "render/GlKit.h"

#include <QRectF>
#include <QTransform>

#include <memory>

namespace kk::render {

class GlDmaBuf;

/// Persistent textures of one decoder (one per plane). Reallocated only when the frame size or
/// pixel format changes; otherwise every frame is a glTexSubImage2D straight from the
/// decoder's memory (no CPU conversion). The last uploaded frame is kept so re-showing the same
/// frame (stills, paused playback, the same frame at two quality levels) skips the upload.
struct PlaneTextures
{
    GLuint texture[3] = {0, 0, 0};
    QSize size[3];
    GLenum internalFormat[3] = {0, 0, 0};
    std::shared_ptr<const media::VideoFrame> uploaded;
};

/// How a frame is shown: the frame's own metadata with the clip's Clip Attributes applied
/// (GlVideo::lookFor()).
struct VideoLook
{
    int rotation = 0; ///< clockwise display rotation (file + attribute), 0/90/180/270
    media::ColorRange range = media::ColorRange::Limited; ///< data levels used for YUV -> RGB
    int alphaMode = 0; ///< AlphaMode of the Clip Attributes (0 Auto, 1 Premultiplied, 2 Straight, 3 None)
    int keepField = -1; ///< deinterlacing: -1 off, 0 keep the top field, 1 the bottom field
};

/// Converts and draws video frames. One instance per GL renderer (render thread).
class GlVideo
{
public:
    /// Builds the conversion program. @return false with a reason on failure.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// Lets frames with a DMA-BUF (zero-copy decoding) be imported through @p importer (owned by
    /// the caller, must outlive this object); without one, or when the import fails, they are
    /// downloaded through `frame.dmabuf->download()` instead.
    void setDmaBufImporter(GlDmaBuf* importer) { m_dmabuf = importer; }
    /// Frames shown through an import / through the download fallback since the start.
    [[nodiscard]] int64_t importedFrames() const { return m_imported; }
    [[nodiscard]] int64_t fallbackFrames() const { return m_fallbacks; }
    /// The reason of the last failed import ("" if none failed).
    [[nodiscard]] QString lastImportError() const { return m_importError; }

    /// Uploads @p frame into @p textures (no-op if it is already the uploaded frame).
    void upload(
        GlKit& kit, PlaneTextures& textures, const std::shared_ptr<const media::VideoFrame>& frame);
    /// Deletes the textures of @p textures.
    static void release(GlKit& kit, PlaneTextures& textures);

    /// Draws the uploaded frame into the bound target.
    /// @p quadToNdc places the unit quad (= the whole picture in DISPLAY orientation, i.e. after
    /// rotation); @p crop is the visible part of the picture in quad coordinates (x0, y0, x1, y1
    /// as a rect), @p opacity multiplies the premultiplied result. Blending as currently set.
    /// @p look: rotation, data levels and deinterlacing to use (lookFor()).
    void draw(GlKit& kit, const PlaneTextures& textures, const media::VideoFrame& frame,
        const QTransform& quadToNdc, const QRectF& crop, float opacity, const VideoLook& look);
    /// The look of @p frame with Clip Attributes @p attributes applied.
    [[nodiscard]] static VideoLook lookFor(
        const media::VideoFrame& frame, const ClipAttributes& attributes);

    /// Texture-coordinate transform (quad in display orientation → coded texture uv) for a
    /// clockwise display @p rotation of 0/90/180/270 degrees.
    [[nodiscard]] static QTransform rotationUv(int rotation);

private:
    void uploadPlanes(GlKit& kit, PlaneTextures& textures, const media::VideoFrame& frame);

    GlDmaBuf* m_dmabuf = nullptr;
    int64_t m_imported = 0;
    int64_t m_fallbacks = 0;
    QString m_importError;
    GlProgram m_program;
    GLint m_layout = -1;
    GLint m_yuvMatrix = -1;
    GLint m_yuvOffset = -1;
    GLint m_sampleScale = -1;
    GLint m_crop = -1;
    GLint m_opacity = -1;
    GLint m_keepField = -1;
    GLint m_alphaMode = -1;
    GLint m_hdr = -1;
};

} // namespace kk::render
