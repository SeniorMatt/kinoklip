// SPDX-License-Identifier: GPL-3.0-or-later
// GlVideo — plane texture management and the YUV → RGB conversion shader.
#include "render/GlVideo.h"

#include "media/HdrToSdr.h"
#include "render/YuvMath.h"

#include <string>

namespace kk::render {

namespace {

// uLayout: 0 = one RGBA plane (BGRA frames, already premultiplied), 1 = semi-planar
// (Y + interleaved UV: NV12/P010), 2 = planar (Y, U, V). A uniform branch is coherent for the
// whole draw, so one program serves every format.
const char* const kYuvFragment = R"(
uniform sampler2D uPlane0;
uniform sampler2D uPlane1;
uniform sampler2D uPlane2;
uniform int uLayout;
uniform mat3 uYuvMatrix;
uniform vec3 uYuvOffset;
uniform float uSampleScale;
uniform vec4 uCrop;      // visible rect in quad coordinates: x0, y0, x1, y1
uniform float uOpacity;
uniform int uKeepField;  // deinterlace: -1 off, 0 keep the top field (even rows), 1 the bottom
uniform int uHdr;        // media::HdrMapping: 0 SDR, 1 HLG, 2 PQ, 3 BT.2020 SDR (hdrToSdr())
// Deinterlacing by field interpolation (Clip Attributes): only the rows of the kept field are
// sampled, vertically interpolated between two rows of that field (2 rows apart), so the
// other field's motion (combing) never shows. Rows are texel centres: row r at (r + 0.5) / h.
vec4 fieldSample(sampler2D tex, vec2 uv)
{
    if (uKeepField < 0)
        return texture(tex, uv);
    float h = float(textureSize(tex, 0).y);
    float f = (uv.y * h - 0.5 - float(uKeepField)) * 0.5; // position in field rows
    float r0 = floor(f);
    float y0 = (r0 * 2.0 + float(uKeepField) + 0.5) / h;
    return mix(texture(tex, vec2(uv.x, y0)), texture(tex, vec2(uv.x, y0 + 2.0 / h)), f - r0);
}
void main()
{
    vec4 c;
    if (uLayout == 0) {
        c = fieldSample(uPlane0, vUv);
    } else {
        vec3 yuv;
        yuv.x = fieldSample(uPlane0, vUv).r;
        if (uLayout == 1)
            yuv.yz = fieldSample(uPlane1, vUv).rg;
        else
            yuv.yz = vec2(fieldSample(uPlane1, vUv).r, fieldSample(uPlane2, vUv).r);
        c = vec4(hdrToSdr(clamp(uYuvMatrix * (yuv * uSampleScale - uYuvOffset), 0.0, 1.0), uHdr),
            1.0);
    }
    vec2 inside = step(uCrop.xy, vQuad) * step(vQuad, uCrop.zw);
    fragColor = c * (uOpacity * inside.x * inside.y * edgeCoverage());
}
)";

/// Texture format of plane @p plane of @p format: internal format, pixel format, type.
struct PlaneFormat
{
    GLenum internal;
    GLenum format;
    GLenum type;
};

PlaneFormat planeFormat(media::PixelFormat format, int plane)
{
    using PF = media::PixelFormat;
    switch (format) {
    case PF::Nv12:
        return plane == 0 ? PlaneFormat {GL_R8, GL_RED, GL_UNSIGNED_BYTE}
                          : PlaneFormat {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case PF::P010:
        return plane == 0 ? PlaneFormat {GL_R16, GL_RED, GL_UNSIGNED_SHORT}
                          : PlaneFormat {GL_RG16, GL_RG, GL_UNSIGNED_SHORT};
    case PF::Yuv420p10:
    case PF::Yuv422p10:
    case PF::Yuv444p10:
        return {GL_R16, GL_RED, GL_UNSIGNED_SHORT};
    case PF::Bgra:
        return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE};
    default: // 8-bit planar
        return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    }
}

int layoutOf(media::PixelFormat format)
{
    if (format == media::PixelFormat::Bgra)
        return 0;
    if (format == media::PixelFormat::Nv12 || format == media::PixelFormat::P010)
        return 1;
    return 2;
}

} // namespace

bool GlVideo::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    // HDR / BT.2020 frames are mapped to SDR BT.709 in the same pass (media/HdrToSdr.h).
    static const std::string source = std::string(media::hdrToSdrGlsl()) + kYuvFragment;
    if (!m_program.build(gl, "yuv", source.c_str(), why))
        return false;
    m_layout = m_program.uniform(gl, "uLayout");
    m_yuvMatrix = m_program.uniform(gl, "uYuvMatrix");
    m_yuvOffset = m_program.uniform(gl, "uYuvOffset");
    m_sampleScale = m_program.uniform(gl, "uSampleScale");
    m_crop = m_program.uniform(gl, "uCrop");
    m_opacity = m_program.uniform(gl, "uOpacity");
    m_keepField = m_program.uniform(gl, "uKeepField");
    m_hdr = m_program.uniform(gl, "uHdr");
    gl->glUseProgram(m_program.id);
    gl->glUniform1i(m_program.uniform(gl, "uPlane0"), 0);
    gl->glUniform1i(m_program.uniform(gl, "uPlane1"), 1);
    gl->glUniform1i(m_program.uniform(gl, "uPlane2"), 2);
    gl->glUseProgram(0);
    return true;
}

void GlVideo::destroy(GlKit& kit)
{
    m_program.destroy(kit.gl());
}

void GlVideo::upload(
    GlKit& kit, PlaneTextures& t, const std::shared_ptr<const media::VideoFrame>& frame)
{
    if (t.uploaded == frame)
        return; // same decoded frame as last time: textures are current
    GlFunctions* gl = kit.gl();
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int p = 0; p < frame->planeCount && p < 3; ++p) {
        const PlaneFormat pf = planeFormat(frame->format, p);
        const QSize size(frame->planeWidth(p), frame->planeHeight(p));
        if (!t.texture[p]) {
            gl->glGenTextures(1, &t.texture[p]);
            gl->glBindTexture(GL_TEXTURE_2D, t.texture[p]);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            gl->glBindTexture(GL_TEXTURE_2D, t.texture[p]);
        }
        // Rows are padded (64-byte aligned linesize): GL_UNPACK_ROW_LENGTH skips the padding,
        // so the decoder's memory is uploaded as-is without repacking.
        gl->glPixelStorei(
            GL_UNPACK_ROW_LENGTH, frame->linesize[size_t(p)] / frame->bytesPerPixel(p));
        if (t.size[p] != size || t.internalFormat[p] != pf.internal) {
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GLint(pf.internal), size.width(), size.height(), 0,
                pf.format, pf.type, frame->data[size_t(p)]);
            t.size[p] = size;
            t.internalFormat[p] = pf.internal;
        } else {
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), pf.format,
                pf.type, frame->data[size_t(p)]);
        }
    }
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    t.uploaded = frame;
}

void GlVideo::release(GlKit& kit, PlaneTextures& t)
{
    for (GLuint& texture : t.texture) {
        if (texture)
            kit.gl()->glDeleteTextures(1, &texture);
        texture = 0;
    }
    t.uploaded.reset();
}

QTransform GlVideo::rotationUv(int rotation)
{
    // Display quad (u, v) → coded texture coordinates. A picture rotated 90° clockwise shows
    // the coded top-left corner at the display top-right: display (u, v) = coded (v, 1 − u).
    switch (rotation) {
    case 90:
        return QTransform(0, -1, 1, 0, 0, 1); // (u, v) → (v, 1 − u)
    case 180:
        return QTransform(-1, 0, 0, -1, 1, 1); // (u, v) → (1 − u, 1 − v)
    case 270:
        return QTransform(0, 1, -1, 0, 1, 0); // (u, v) → (1 − v, u)
    default:
        return QTransform();
    }
}

VideoLook GlVideo::lookFor(const media::VideoFrame& frame, const ClipAttributes& a)
{
    VideoLook look;
    look.rotation = normalizedRotation(frame.rotation + a.rotation);
    look.range = a.levels == DataLevels::Video ? media::ColorRange::Limited
        : a.levels == DataLevels::Full         ? media::ColorRange::Full
                                               : frame.range;
    if (a.deinterlace && a.fieldOrder != FieldOrder::Progressive)
        look.keepField = a.fieldOrder == FieldOrder::LowerFirst ? 1 : 0;
    return look;
}

void GlVideo::draw(GlKit& kit, const PlaneTextures& t, const media::VideoFrame& frame,
    const QTransform& quadToNdc, const QRectF& crop, float opacity, const VideoLook& look)
{
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(m_program.id);
    kit.setMatrix(m_program.posMatrix, quadToNdc);
    kit.setMatrix(m_program.uvMatrix, rotationUv(look.rotation));
    gl->glUniform1i(m_keepField, look.keepField);
    const int layout = layoutOf(frame.format);
    gl->glUniform1i(m_layout, layout);
    if (layout != 0) {
        const YuvCoefficients c = yuvCoefficients(frame.matrix, look.range, frame.format);
        gl->glUniformMatrix3fv(m_yuvMatrix, 1, GL_FALSE, c.matrix.data());
        gl->glUniform3fv(m_yuvOffset, 1, c.offset.data());
        gl->glUniform1f(m_sampleScale, c.sampleScale);
        gl->glUniform1i(m_hdr, int(media::hdrMappingOf(frame)));
    }
    gl->glUniform4f(
        m_crop, float(crop.left()), float(crop.top()), float(crop.right()), float(crop.bottom()));
    gl->glUniform1f(m_opacity, opacity);
    for (int p = 0; p < 3; ++p)
        kit.bindTexture(p, t.texture[p] ? t.texture[p] : kit.transparentTexture());
    kit.drawQuad();
}

} // namespace kk::render
