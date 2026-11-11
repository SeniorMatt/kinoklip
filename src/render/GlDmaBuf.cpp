// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GlDmaBuf.h"

#include "media/DecodeStats.h"

#include <QGuiApplication>
#include <QOpenGLContext>

#ifdef Q_OS_UNIX
#include <dlfcn.h>
#else
static void* dlsym(void*, const char*)
{
    return nullptr;
}
#define RTLD_DEFAULT nullptr
#endif

namespace kk::render {

namespace {

// EGL constants (EGL 1.4, EGL_KHR_image_base, EGL_EXT_image_dma_buf_import[_modifiers]); defined
// here so no EGL header is needed to build.
using EglInt = int32_t;
constexpr EglInt kEglNone = 0x3038;
constexpr EglInt kEglWidth = 0x3057;
constexpr EglInt kEglHeight = 0x3056;
constexpr EglInt kEglExtensions = 0x3055;
constexpr EglInt kEglLinuxDmaBuf = 0x3270;
constexpr EglInt kEglDrmFourcc = 0x3271;
constexpr EglInt kEglPlane0Fd = 0x3272;
constexpr EglInt kEglPlane0Offset = 0x3273;
constexpr EglInt kEglPlane0Pitch = 0x3274;
constexpr EglInt kEglPlane0ModifierLo = 0x3443;
constexpr EglInt kEglPlane0ModifierHi = 0x3444;
constexpr uint64_t kModInvalid = 0x00ffffffffffffffULL; // DRM_FORMAT_MOD_INVALID

using CreateImageFn
    = void* (*)(void* display, void* context, unsigned target, void* buffer, const EglInt* attribs);
using DestroyImageFn = unsigned (*)(void* display, void* image);
using GetErrorFn = EglInt (*)();
using QueryStringFn = const char* (*)(void* display, EglInt name);
using TargetTextureFn = void (*)(unsigned target, void* image);

} // namespace

std::vector<int32_t> dmaBufImageAttributes(
    const media::DmaBufFrame::Plane& plane, bool withModifiers)
{
    std::vector<int32_t> a = {kEglWidth, plane.width, kEglHeight, plane.height, kEglDrmFourcc,
        EglInt(plane.drmFormat), kEglPlane0Fd, plane.fd, kEglPlane0Offset, EglInt(plane.offset),
        kEglPlane0Pitch, EglInt(plane.pitch)};
    if (withModifiers && plane.modifier != kModInvalid) {
        a.insert(a.end(),
            {kEglPlane0ModifierLo, EglInt(uint32_t(plane.modifier & 0xffffffffu)),
                kEglPlane0ModifierHi, EglInt(uint32_t(plane.modifier >> 32))});
    }
    a.push_back(kEglNone);
    return a;
}

bool hasExtension(const QString& extensions, const char* name)
{
    const QString wanted = QString::fromLatin1(name);
    const auto words = QStringView(extensions).split(u' ', Qt::SkipEmptyParts);
    for (const QStringView word : words) {
        if (word == wanted) {
            return true;
        }
    }
    return false;
}

QString drmFormatName(uint32_t fourcc)
{
    switch (fourcc) {
    case 0x20203852:
        return QStringLiteral("R8");
    case 0x38385247:
        return QStringLiteral("GR88");
    case 0x20363152:
        return QStringLiteral("R16");
    case 0x32335247:
        return QStringLiteral("GR1616");
    default:
        return QStringLiteral("0x%1").arg(fourcc, 8, 16, QLatin1Char('0'));
    }
}

bool GlDmaBuf::probe(QOpenGLContext* context, QString* why)
{
    // EGL 1.4 entry points are not always returned by eglGetProcAddress (it may only know
    // extensions): libEGL is loaded already (Qt uses it for this context), ask it directly.
    auto lookup = [&](const char* name) -> void* {
        if (void* p = reinterpret_cast<void*>(context->getProcAddress(name)))
            return p;
        return dlsym(RTLD_DEFAULT, name);
    };
    m_usable = false;
    auto fail = [&](const QString& reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (!context) {
        return fail(QStringLiteral("no GL context"));
    }
    const auto* egl = context->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl || !egl->display()) {
        return fail(
            QStringLiteral("the GL context is not an EGL context (Qt platform \"%1\"%2)")
                .arg(QGuiApplication::platformName(),
                    QGuiApplication::platformName() == QLatin1String("xcb")
                        ? QStringLiteral(": X11 uses GLX; set QT_XCB_GL_INTEGRATION=xcb_egl to "
                                         "use EGL")
                        : QString()));
    }
    m_display = egl->display();
    m_context = context;
    auto queryString = reinterpret_cast<QueryStringFn>(lookup("eglQueryString"));
    m_createImage = lookup("eglCreateImageKHR");
    m_destroyImage = lookup("eglDestroyImageKHR");
    m_getError = lookup("eglGetError");
    m_targetTexture
        = reinterpret_cast<void*>(context->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!queryString || !m_createImage || !m_destroyImage || !m_getError || !m_targetTexture) {
        return fail(
            QStringLiteral("EGL image functions missing (%1%2%3%4%5)")
                .arg(queryString ? "" : "eglQueryString ",
                    m_createImage ? "" : "eglCreateImageKHR ",
                    m_destroyImage ? "" : "eglDestroyImageKHR ", m_getError ? "" : "eglGetError ",
                    m_targetTexture ? "" : "glEGLImageTargetTexture2DOES"));
    }
    const QString extensions = QString::fromLatin1(queryString(m_display, kEglExtensions));
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import")) {
        return fail(QStringLiteral("EGL_EXT_image_dma_buf_import is not supported"));
    }
    if (!context->hasExtension("GL_OES_EGL_image")) {
        return fail(QStringLiteral("GL_OES_EGL_image is not supported"));
    }
    m_modifiers = hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    m_usable = true;
    return true;
}

bool GlDmaBuf::import(GlKit& kit, PlaneTextures& t, const media::VideoFrame& frame, QString* why)
{
    auto fail = [&](const QString& reason) {
        m_usable = false;
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (!m_usable || !frame.dmabuf || frame.dmabuf->planeCount != 2) {
        return fail(QStringLiteral("no importable frame"));
    }
    if (QOpenGLContext::currentContext() != m_context) {
        return fail(QStringLiteral("not called with the compositor's context current"));
    }
    GlFunctions* gl = kit.gl();
    auto create = reinterpret_cast<CreateImageFn>(m_createImage);
    auto destroy = reinterpret_cast<DestroyImageFn>(m_destroyImage);
    auto getError = reinterpret_cast<GetErrorFn>(m_getError);
    auto target = reinterpret_cast<TargetTextureFn>(m_targetTexture);
    while (gl->glGetError() != GL_NO_ERROR) { }
    GLuint fresh[2] = {0, 0};
    bool ok = true;
    QString problem;
    for (int p = 0; p < 2 && ok; ++p) {
        const media::DmaBufFrame::Plane& plane = frame.dmabuf->planes[size_t(p)];
        const std::vector<int32_t> attribs = dmaBufImageAttributes(plane, m_modifiers);
        void* image
            = create(m_display, nullptr, unsigned(kEglLinuxDmaBuf), nullptr, attribs.data());
        if (!image) {
            problem = QStringLiteral("eglCreateImage failed for plane %1 (%2 %3x%4, modifier "
                                     "0x%5): EGL error 0x%6")
                          .arg(p)
                          .arg(drmFormatName(plane.drmFormat))
                          .arg(plane.width)
                          .arg(plane.height)
                          .arg(plane.modifier, 0, 16)
                          .arg(getError(), 0, 16);
            ok = false;
            break;
        }
        gl->glGenTextures(1, &fresh[p]);
        gl->glBindTexture(GL_TEXTURE_2D, fresh[p]);
        target(GL_TEXTURE_2D, image);
        // The texture keeps the buffer alive; the image object is not needed any more.
        destroy(m_display, image);
        if (const GLenum error = gl->glGetError(); error != GL_NO_ERROR) {
            problem = QStringLiteral("glEGLImageTargetTexture2DOES failed for plane %1: GL error "
                                     "0x%2")
                          .arg(p)
                          .arg(error, 0, 16);
            ok = false;
        }
    }
    if (!ok) {
        for (GLuint texture : fresh) {
            if (texture) {
                gl->glDeleteTextures(1, &texture);
            }
        }
        media::countImportFailure();
        return fail(problem);
    }
    for (int p = 0; p < 2; ++p) {
        gl->glBindTexture(GL_TEXTURE_2D, fresh[p]);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (t.texture[p]) {
            gl->glDeleteTextures(1, &t.texture[p]);
        }
        t.texture[p] = fresh[p];
        // Not a texture GlVideo::upload() may update in place: the next CPU upload re-specifies.
        t.size[p] = QSize();
        t.internalFormat[p] = 0;
    }
    return true;
}

} // namespace kk::render
