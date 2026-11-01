// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL viewer surface: persistent texture + one textured quad.
#include "ui/viewer/GlViewerSurface.h"

#include <QLabel>
#include <QLoggingCategory>
#include <QOpenGLContext>

namespace cw {

namespace {

Q_LOGGING_CATEGORY(lcGlViewer, "cw.ui.viewer.gl")

constexpr float kSurroundGrey = 0x0b / 255.0f; // slightly lifted black around the picture

// Unit quad as a triangle strip; the vertex shader maps it onto the picture rectangle.
constexpr GLfloat kQuad[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 position;
uniform vec4 rect; // x, y, width, height in normalised device coordinates
out vec2 uv;
void main()
{
    uv = vec2(position.x, 1.0 - position.y); // QImage rows are top-down
    gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec2 uv;
uniform sampler2D frame;
out vec4 color;
void main()
{
    // Premultiplied colour over the opaque black letterbox = the colour itself.
    color = vec4(texture(frame, uv).rgb, 1.0);
}
)";

} // namespace

GlViewerSurface::GlViewerSurface(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("ViewerSurface"));
    setMinimumSize(160, 90);
    m_emptyText = new QLabel(this);
    m_emptyText->setObjectName(QStringLiteral("ViewerEmptyText"));
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyText->setStyleSheet(QStringLiteral("color: #5a5a60; background: transparent;"));
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
    m_picture.setFrame(m_frame);
    updateEmptyText();
    update();
}

void GlViewerSurface::clear(const QString& text)
{
    m_frame = QImage();
    m_frameDirty = false;
    m_picture.clear(text);
    m_emptyText->setText(text);
    updateEmptyText();
    update();
}

void GlViewerSurface::setAspect(QSize aspect)
{
    if (m_picture.setAspect(aspect)) {
        updateEmptyText();
        update();
    }
}

QRect GlViewerSurface::pictureRect() const
{
    return m_picture.pictureRect(size());
}

void GlViewerSurface::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);
    updateEmptyText();
}

void GlViewerSurface::updateEmptyText()
{
    m_emptyText->setGeometry(pictureRect());
    m_emptyText->setVisible(m_frame.isNull() && !m_emptyText->text().isEmpty());
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
    glClearColor(kSurroundGrey, kSurroundGrey, kSurroundGrey, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_glReady)
        return;

    // Picture rectangle in framebuffer pixels (GL's origin is bottom-left).
    const qreal dpr = devicePixelRatioF();
    const QRect logical = pictureRect();
    const int fbHeight = int(height() * dpr);
    const QRect r(int(logical.x() * dpr), fbHeight - int((logical.y() + logical.height()) * dpr),
        int(logical.width() * dpr), int(logical.height() * dpr));

    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x(), r.y(), r.width(), r.height());
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    if (m_frame.isNull())
        return;
    if (m_frameDirty)
        uploadFrame();

    const float fbWidth = float(width() * dpr);
    glUseProgram(m_program);
    glUniform4f(m_rectUniform, 2.f * r.x() / fbWidth - 1.f, 2.f * r.y() / float(fbHeight) - 1.f,
        2.f * r.width() / fbWidth, 2.f * r.height() / float(fbHeight));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

void GlViewerSurface::releaseGl()
{
    if (!m_glReady || !context())
        return;
    makeCurrent();
    glDeleteTextures(1, &m_texture);
    glDeleteBuffers(1, &m_vbo);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteProgram(m_program);
    m_texture = m_vbo = m_vao = m_program = 0;
    m_glReady = false;
    doneCurrent();
}

} // namespace cw
