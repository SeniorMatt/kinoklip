// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL viewer surface: compositor texture (zero copy) or uploaded image, one textured quad.
#include "ui/viewer/GlViewerSurface.h"

#include "ui/common/SchemeColors.h"

#include <QCoreApplication>
#include <QLabel>
#include <QLoggingCategory>
#include <QOpenGLContext>

#include <cmath>

namespace kk {

namespace {

Q_LOGGING_CATEGORY(lcGlViewer, "kk.ui.viewer.gl", QtInfoMsg)


// Unit quad as a triangle strip; the vertex shader maps it onto the picture rectangle.
constexpr GLfloat kQuad[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 position;
uniform vec4 rect; // x, y, width, height in normalised device coordinates
out vec2 uv;
void main()
{
    // Both QImage uploads and compositor textures store row 0 = top of the picture.
    uv = vec2(position.x, 1.0 - position.y);
    gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec2 uv;
uniform sampler2D frame;
out vec4 color;
void main()
{
    // Premultiplied colour over the opaque black picture area = the colour itself.
    color = vec4(texture(frame, uv).rgb, 1.0);
}
)";

} // namespace

GlViewerSurface::GlViewerSurface(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("ViewerSurface"));
    setMinimumSize(160, 90);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("ViewerEmptyText"));
    m_text->setAlignment(Qt::AlignCenter);
    m_text->setAttribute(Qt::WA_TransparentForMouseEvents);
    // Same colour as the raster surface's empty text (SchemeColors viewer roles).
    const auto styleText = [label = m_text] {
        const SchemeColors& sc = schemeColors();
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(mixColors(sc.viewerText, sc.viewerSurround, 0.45)
                                         .name(QColor::HexRgb)));
    };
    styleText();
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, m_text, styleText);
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        [this] { update(); }); // surround colour
}

GlViewerSurface::~GlViewerSurface()
{
    releaseGl();
}

void GlViewerSurface::setFrame(const QImage& frame)
{
    // ARGB32 variants are uploaded as-is (BGRA); anything else is converted once here.
    const bool direct = frame.format() == QImage::Format_ARGB32_Premultiplied
        || frame.format() == QImage::Format_ARGB32 || frame.format() == QImage::Format_RGB32;
    m_frame = direct ? frame : frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_frameDirty = true;
    m_pending.reset();
    m_picture.setFrame(m_frame);
    updateText();
    update();
}

void GlViewerSurface::setRenderedFrame(const RenderedFramePtr& frame)
{
    if (!frame || !frame->ok) {
        return;
    }
    if (frame->texture == 0) {
        setFrame(frame->image); // CPU compositor, or image output requested
        return;
    }
    m_pending = frame;
    m_frame = QImage();
    m_frameDirty = false;
    m_picture.setExternalFrame(true);
    updateText();
    update();
}

bool GlViewerSurface::canDrawTextures() const
{
    return QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts);
}

void GlViewerSurface::clear(const QString& text)
{
    m_frame = QImage();
    m_frameDirty = false;
    m_pending.reset();
    m_picture.clear(text);
    updateText();
    update();
}

void GlViewerSurface::setAspect(QSize aspect)
{
    if (m_picture.setAspect(aspect)) {
        updateText();
        update();
    }
}

void GlViewerSurface::setZoom(double zoom)
{
    if (m_picture.setZoom(zoom)) {
        updateText();
        update();
    }
}

void GlViewerSurface::setOverlayText(const QString& text)
{
    if (text != m_picture.overlayText()) {
        m_picture.setOverlayText(text);
        updateText();
    }
}

QRect GlViewerSurface::pictureRect() const
{
    return m_picture.pictureRect(size(), devicePixelRatioF());
}

void GlViewerSurface::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);
    updateText();
}

void GlViewerSurface::updateText()
{
    const QString text = m_picture.hasFrame() ? m_picture.overlayText() : m_picture.emptyText();
    m_text->setText(text);
    m_text->setGeometry(pictureRect().intersected(rect()));
    m_text->setVisible(!text.isEmpty());
}

void GlViewerSurface::initializeGL()
{
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &GlViewerSurface::releaseGl,
        Qt::DirectConnection);

    auto compile = [this](GLenum type, const char* source) -> GLuint {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            qCWarning(lcGlViewer) << "shader compile failed:" << log;
        }
        return shader;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        qCWarning(lcGlViewer) << "shader link failed; viewer shows black";
        return;
    }
    m_rectUniform = glGetUniformLocation(m_program, "rect");

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Sampler for both paths: overrides the bound texture's filter state, so the compositor's
    // shared textures are never modified from this context.
    glGenSamplers(1, &m_sampler);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_textureSize = QSize();
    m_frameDirty = !m_frame.isNull();
    m_glReady = true;
}

void GlViewerSurface::uploadFrame()
{
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(m_frame.bytesPerLine() / 4));
    if (m_frame.size() != m_textureSize) {
        // (Re)allocate only when the size changes; same-size frames reuse the storage.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_frame.width(), m_frame.height(), 0, GL_BGRA,
            GL_UNSIGNED_BYTE, m_frame.constBits());
        m_textureSize = m_frame.size();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frame.width(), m_frame.height(), GL_BGRA,
            GL_UNSIGNED_BYTE, m_frame.constBits());
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    m_frameDirty = false;
}

void GlViewerSurface::paintGL()
{
    const QColor surround = schemeColors().viewerSurround;
    glClearColor(surround.redF(), surround.greenF(), surround.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_glReady) {
        return;
    }

    // Picture rectangle in framebuffer pixels (GL's origin is bottom-left).
    const qreal dpr = devicePixelRatioF();
    const QRect logical = pictureRect();
    const int fbHeight = int(height() * dpr);
    const QRect r(int(logical.x() * dpr), fbHeight - int((logical.y() + logical.height()) * dpr),
        int(logical.width() * dpr), int(logical.height() * dpr));
    const QRect fbRect(0, 0, int(width() * dpr), fbHeight);
    const QRect visible = r.intersected(fbRect);

    glEnable(GL_SCISSOR_TEST);
    glScissor(visible.x(), visible.y(), visible.width(), visible.height());
    glClearColor(0.f, 0.f, 0.f, 1.f); // picture area: video content, fixed black
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    GLuint texture = 0;
    if (m_pending && m_pending->texture != 0) {
        // Cross-context synchronisation: the GPU waits for the render thread's commands that
        // produced the texture; the CPU does not block.
        if (m_pending->fence) {
            glWaitSync(static_cast<GLsync>(m_pending->fence), 0, GL_TIMEOUT_IGNORED);
        }
        texture = m_pending->texture;
    } else if (!m_frame.isNull()) {
        if (m_frameDirty) {
            uploadFrame();
        }
        texture = m_texture;
    }
    if (texture == 0) {
        m_drawn.reset();
        return;
    }

    // Exact 1:1 (or 2:1) zoom looks best unfiltered; everything else is scaled smoothly.
    const double zoom = m_picture.zoom();
    const GLint filter
        = zoom >= 1.0 && std::abs(zoom - std::round(zoom)) < 1e-6 ? GL_NEAREST : GL_LINEAR;
    glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, filter);

    const float fbWidth = float(width() * dpr);
    glUseProgram(m_program);
    glUniform4f(m_rectUniform, 2.f * r.x() / fbWidth - 1.f, 2.f * r.y() / float(fbHeight) - 1.f,
        2.f * r.width() / fbWidth, 2.f * r.height() / float(fbHeight));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, m_sampler);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    // The previous frame's draw was submitted before this one: it may be recycled now. The
    // current frame stays referenced until the next paint replaces it.
    m_drawn = m_pending;
}

void GlViewerSurface::releaseGl()
{
    if (!m_glReady || !context()) {
        return;
    }
    makeCurrent();
    glDeleteTextures(1, &m_texture);
    glDeleteSamplers(1, &m_sampler);
    glDeleteBuffers(1, &m_vbo);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteProgram(m_program);
    m_texture = m_sampler = m_vbo = m_vao = m_program = 0;
    m_glReady = false;
    doneCurrent();
}

} // namespace kk
