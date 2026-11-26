// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/GraphicsCheck.h"

#include "app/AppInfo.h"
#include "media/HwAccel.h"
#include "media/VideoDecoder.h"
#include "render/GlApi.h"

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QScreen>
#include <QSurfaceFormat>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace kk::graphicscheck {

namespace {

constexpr int kTolerance = 4; ///< levels of difference allowed per channel

/// Fullscreen triangle from the vertex index; the fragment colour is the picture's formula.
const char* kVertexBody = R"(
out vec2 uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentBody = R"(
in vec2 uv;
out vec4 color;
void main()
{
    color = vec4(uv.x, uv.y, 0.5, 1.0);
}
)";

QString line(const QString& key, const QString& value)
{
    return QStringLiteral("  %1 %2\n").arg(key + QLatin1Char(':'), -22).arg(value);
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

QString environment(const char* name)
{
    const QString value = qEnvironmentVariable(name);
    return value.isEmpty() ? QStringLiteral("(not set)") : value;
}

/// The GPU render nodes ("/dev/dri/renderD128") and whether this user may open them.
QString renderNodes(QStringList* problems)
{
    const QDir dri(QStringLiteral("/dev/dri"));
    const QStringList nodes = dri.entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Files);
    if (nodes.isEmpty()) {
        problems->append(QStringLiteral("There is no GPU render node (/dev/dri/renderD*): no "
                                        "graphics driver is loaded for a card, or this is a "
                                        "container or virtual machine without GPU access."));
        return QStringLiteral("none");
    }
    QStringList parts;
    for (const QString& node : nodes) {
        const QFileInfo info(dri.absoluteFilePath(node));
        const bool usable = info.isReadable() && info.isWritable();
        parts.append(node + (usable ? QStringLiteral(" (usable)") : QStringLiteral(" (no access)")));
        if (!usable)
            problems->append(QStringLiteral("%1 exists but this user may not open it: add the "
                                            "user to the group that owns it (usually \"render\" "
                                            "or \"video\") and log in again.")
                                 .arg(info.absoluteFilePath()));
    }
    return parts.join(QStringLiteral(", "));
}

QString backendLine(const media::HwDeviceInfo& info)
{
    QString text;
    if (info.available) {
        text = QStringLiteral("opens");
        if (!info.device.isEmpty())
            text += QStringLiteral(", device ") + info.device;
        if (!info.driver.isEmpty())
            text += QStringLiteral(", driver ") + info.driver;
        text += info.codecs.isEmpty() ? QStringLiteral(", no codecs listed")
                                      : QStringLiteral(", codecs: ") + info.codecs.join(QStringLiteral(", "));
    } else {
        text = QStringLiteral("not available");
        if (!info.error.isEmpty())
            text += QStringLiteral(": ") + info.error;
    }
    return text;
}

} // namespace

bool isSoftwareRenderer(const QString& renderer)
{
    const QString r = renderer.toLower();
    for (const char* name : {"llvmpipe", "softpipe", "swrast", "software", "swiftshader", "lavapipe",
             "microsoft basic render"}) {
        if (r.contains(QLatin1String(name)))
            return true;
    }
    return false;
}

QColor expectedPictureColor(int x, int y)
{
    const auto level = [](double v) { return int(std::lround(v * 255.0)); };
    return QColor(level((x + 0.5) / kPictureSize), level((y + 0.5) / kPictureSize), level(0.5));
}

bool pictureMatches(const QImage& image, QString* problem)
{
    if (image.width() != kPictureSize || image.height() != kPictureSize) {
        if (problem)
            *problem = QStringLiteral("the picture came back %1x%2, not %3x%3")
                           .arg(image.width())
                           .arg(image.height())
                           .arg(kPictureSize);
        return false;
    }
    // A grid of points, the corners and the middle included. OpenGL's origin is the bottom left,
    // the image's the top left: a flipped read-back shows as a difference.
    for (const int gy : {1, 16, 32, 47, 62}) {
        for (const int gx : {1, 16, 32, 47, 62}) {
            const QColor expected = expectedPictureColor(gx, kPictureSize - 1 - gy);
            const QColor got = image.pixelColor(gx, gy);
            if (std::abs(got.red() - expected.red()) > kTolerance
                || std::abs(got.green() - expected.green()) > kTolerance
                || std::abs(got.blue() - expected.blue()) > kTolerance) {
                if (problem)
                    *problem = QStringLiteral("pixel (%1, %2) is %3, expected about %4")
                                   .arg(gx)
                                   .arg(gy)
                                   .arg(got.name(), expected.name());
                return false;
            }
        }
    }
    return true;
}

bool drawTestPicture(Report* report)
{
    QOffscreenSurface surface;
    surface.create();
    if (!surface.isValid()) {
        report->pictureProblem = QStringLiteral("no drawing surface could be created");
        return false;
    }
    // Desktop OpenGL 3.3 core first (what Kinoklip asks for), then OpenGL ES 3.0 (Raspberry Pi 5).
    QOpenGLContext context;
    render::GlApi used = render::GlApi::Desktop;
    bool made = false;
    for (const render::GlApi api : {render::GlApi::Desktop, render::GlApi::Gles}) {
        QOpenGLContext attempt;
        attempt.setFormat(render::glFormat(api));
        if (!attempt.create())
            continue;
        context.setFormat(attempt.format());
        if (context.create()) {
            used = api;
            made = true;
            break;
        }
    }
    if (!made || !context.makeCurrent(&surface)) {
        report->pictureProblem = QStringLiteral("no OpenGL 3.3 or OpenGL ES 3.0 context could be made");
        return false;
    }
    report->glContext = true;
    report->glApi = render::glApiName(used);
    QOpenGLFunctions* gl = context.functions();
    const auto text = [gl](GLenum name) {
        const auto* s = gl->glGetString(name);
        return s ? QString::fromLatin1(reinterpret_cast<const char*>(s)) : QString();
    };
    report->vendor = text(GL_VENDOR);
    report->renderer = text(GL_RENDERER);
    report->version = text(GL_VERSION);
    report->softwareRenderer = isSoftwareRenderer(report->renderer);

    bool drawn = false;
    {
        QOpenGLFramebufferObject fbo(kPictureSize, kPictureSize);
        QOpenGLShaderProgram program;
        const bool built
            = fbo.isValid()
            && program.addShaderFromSourceCode(QOpenGLShader::Vertex,
                render::shaderHeader(used, render::ShaderStage::Vertex) + QByteArray(kVertexBody))
            && program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                render::shaderHeader(used, render::ShaderStage::Fragment) + QByteArray(kFragmentBody))
            && program.link();
        if (!built) {
            report->pictureProblem = fbo.isValid()
                ? QStringLiteral("the test shader did not build: ") + program.log().trimmed()
                : QStringLiteral("a drawing buffer could not be created");
        } else {
            QOpenGLVertexArrayObject vao;
            vao.create();
            vao.bind();
            fbo.bind();
            gl->glViewport(0, 0, kPictureSize, kPictureSize);
            gl->glDisable(GL_BLEND);
            gl->glClearColor(1.0f, 0.0f, 1.0f, 1.0f); // a leftover magenta shows a missed draw
            gl->glClear(GL_COLOR_BUFFER_BIT);
            program.bind();
            gl->glDrawArrays(GL_TRIANGLES, 0, 3);
            gl->glFinish();
            const QImage image = fbo.toImage(false); // read back from the card
            fbo.release();
            vao.release();
            drawn = !image.isNull();
            if (drawn) {
                report->pictureCorrect = pictureMatches(image, &report->pictureProblem);
            } else {
                report->pictureProblem = QStringLiteral("the picture could not be read back");
            }
        }
    }
    report->pictureDrawn = drawn;
    context.doneCurrent();
    return true;
}

void checkVideoDecoding(const QString& videoFile, Report* report)
{
    QString out;
    out += QStringLiteral("Hardware video decoding\n");
    QStringList problems;
    out += line(QStringLiteral("GPU render nodes"), renderNodes(&problems));
    for (const media::HwDeviceInfo& info : media::hwCapabilities()) {
        out += line(info.name, backendLine(info));
        if (info.available) {
            report->hardwareDecoding = true;
            report->hardwareBackends.append(info.name);
        }
    }
    if (!media::nvidiaDriverPresent())
        problems.append(QStringLiteral("No NVIDIA driver is loaded (/dev/nvidiactl is missing): "
                                       "CUDA / NVDEC is not possible."));
    if (!media::qsvRuntimePresent())
        problems.append(QStringLiteral("No Intel Quick Sync runtime (oneVPL / libmfx-gen) was found: "
                                       "Quick Sync is not used."));
    if (report->softwareRenderer)
        problems.append(QStringLiteral("OpenGL runs on the CPU (%1): pictures are not shown by a "
                                       "graphics card, and zero-copy hardware decoding is off.")
                            .arg(report->renderer));
    if (report->hardwareDecoding) {
        out += QStringLiteral("  Result: video can be decoded on the card with %1 (whether a "
                              "given format works depends on the codec list above).\n")
                   .arg(report->hardwareBackends.join(QStringLiteral(" / ")));
    } else {
        out += QStringLiteral("  Result: no hardware decoding backend opens; video is decoded on "
                              "the CPU.\n");
    }
    if (!problems.isEmpty()) {
        out += QStringLiteral("  Why not / notes:\n");
        for (const QString& p : std::as_const(problems))
            out += QStringLiteral("   - %1\n").arg(p);
    }

    if (!videoFile.isEmpty()) {
        out += QStringLiteral("\nDecoding %1\n").arg(QFileInfo(videoFile).fileName());
        const struct
        {
            media::HwAccelMode mode;
            const char* name;
        } modes[] = {{media::HwAccelMode::Auto, "Auto"}, {media::HwAccelMode::Vaapi, "VAAPI"},
            {media::HwAccelMode::Cuda, "CUDA"}, {media::HwAccelMode::Vulkan, "Vulkan"},
            {media::HwAccelMode::Off, "Off (software)"}};
        for (const auto& m : modes) {
            media::VideoDecoder decoder;
            decoder.setHwAccel(m.mode);
            QString error;
            QString result;
            if (!decoder.open(videoFile, &error)) {
                result = QStringLiteral("cannot open: ") + error;
            } else {
                const auto frame = decoder.decodeFrame(0);
                const media::HwAccelMode active = decoder.activeHwAccel();
                const QString how = active == media::HwAccelMode::Off
                    ? QStringLiteral("software")
                    : QStringLiteral("hardware (%1)").arg(media::hwAccelName(active));
                result = frame ? how : how + QStringLiteral(", but the first frame did not decode");
                if (m.mode == media::HwAccelMode::Auto)
                    report->videoResult = how;
                if (m.mode != media::HwAccelMode::Auto && m.mode != media::HwAccelMode::Off
                    && active == media::HwAccelMode::Off)
                    result += QStringLiteral(" (this backend cannot decode this file here; "
                                             "software was used instead)");
            }
            out += line(QString::fromLatin1(m.name), result);
        }
    } else {
        out += QStringLiteral("\n  To test a real file: kinoklip --graphics-check clip.mp4\n");
    }
    report->text += out;
}

Report run(const QString& videoFile)
{
    Report report;
    QString& t = report.text;
    t += QStringLiteral("Kinoklip graphics check\n=======================\n");
    t += line(QStringLiteral("Kinoklip"),
        QString::fromLatin1(appinfo::name) + QLatin1Char(' ') + appinfo::displayVersion());
    t += line(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    t += line(QStringLiteral("Platform plugin"), QGuiApplication::platformName());
    t += line(QStringLiteral("Session"),
        QStringLiteral("%1, desktop %2")
            .arg(environment("XDG_SESSION_TYPE"), environment("XDG_CURRENT_DESKTOP")));
    t += line(QStringLiteral("DISPLAY / WAYLAND"),
        environment("DISPLAY") + QStringLiteral(" / ") + environment("WAYLAND_DISPLAY"));
    for (const QScreen* screen : QGuiApplication::screens())
        t += line(QStringLiteral("Screen"),
            QStringLiteral("%1 %2x%3 at %4 Hz, scale %5")
                .arg(screen->name())
                .arg(screen->size().width())
                .arg(screen->size().height())
                .arg(screen->refreshRate(), 0, 'f', 0)
                .arg(screen->devicePixelRatio(), 0, 'g', 3));
    t += QStringLiteral("\nOpenGL\n");
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
        t += QStringLiteral("  Note: no desktop session (platform \"offscreen\"): the picture is "
                            "drawn without a window, which can differ from what the window sees.\n");
    if (!drawTestPicture(&report)) {
        t += QStringLiteral("  No OpenGL context: %1\n").arg(report.pictureProblem);
    } else {
        t += line(QStringLiteral("API"), report.glApi);
        t += line(QStringLiteral("Vendor"), report.vendor);
        t += line(QStringLiteral("Renderer"), report.renderer);
        t += line(QStringLiteral("Version"), report.version);
        t += line(QStringLiteral("On the CPU"),
            report.softwareRenderer ? QStringLiteral("YES (software rendering, not a graphics card)")
                                    : QStringLiteral("no"));
        t += line(QStringLiteral("Test picture"),
            QStringLiteral("%1x%1 drawn with the card and read back: %2")
                .arg(kPictureSize)
                .arg(report.pictureCorrect
                        ? QStringLiteral("correct")
                        : QStringLiteral("WRONG (%1)").arg(report.pictureProblem)));
    }
    t += QLatin1Char('\n');
    checkVideoDecoding(videoFile, &report);

    report.ok = report.glContext && report.pictureCorrect;
    t += QStringLiteral("\nVerdict: %1\n")
             .arg(report.ok ? (report.softwareRenderer
                                  ? QStringLiteral("OpenGL works, but on the CPU (no graphics card is "
                                                   "used for drawing).")
                                  : QStringLiteral("the graphics card draws correctly."))
                            : QStringLiteral("the graphics card did NOT draw the test picture "
                                             "correctly; please attach this report to the bug "
                                             "report."));
    return report;
}

int runCommandLine(const QString& videoFile)
{
    const Report report = run(videoFile);
    std::fputs(qPrintable(report.text), stdout);
    std::fflush(stdout);
    return report.ok ? 0 : 1;
}

} // namespace kk::graphicscheck
