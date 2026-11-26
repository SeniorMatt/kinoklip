// SPDX-License-Identifier: GPL-3.0-or-later
// GraphicsCheck — `kinoklip --graphics-check [video file]` (spec 177): draws a test picture with
// the graphics card, reads it back and compares it, says which card and driver are in use and
// whether hardware video decoding works on it and why not, and prints a report that can be
// pasted into a bug report. Needs a QGuiApplication on the real platform (not offscreen) to test
// the card; without a display it says so.
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

namespace kk::graphicscheck {

/// Size of the test picture (pixels).
constexpr int kPictureSize = 64;

/// What the check found. Everything the report says is also here, for the tests.
struct Report
{
    QString text; ///< the whole report, plain text
    bool glContext = false; ///< an OpenGL context could be made
    QString glApi; ///< "OpenGL 3.3 core" / "OpenGL ES 3.0" of the context ("" = none)
    QString vendor, renderer, version; ///< GL_VENDOR, GL_RENDERER, GL_VERSION
    bool softwareRenderer = false; ///< the "graphics card" is a CPU rasteriser (llvmpipe, ...)
    bool pictureDrawn = false; ///< the test picture was drawn and read back
    bool pictureCorrect = false; ///< the read-back picture has the expected colours
    QString pictureProblem; ///< what differed (when !pictureCorrect)
    bool hardwareDecoding = false; ///< a hardware backend opened (VAAPI, CUDA or Vulkan)
    QStringList hardwareBackends; ///< names of the backends that opened
    QString videoResult; ///< "software", "hardware (VAAPI)", ... for the video file ("" = none given)
    bool ok = false; ///< picture correct on a real card (or no problem found)
};

/// True when @p renderer names a CPU rasteriser (llvmpipe, softpipe, swrast, "Software", ...).
[[nodiscard]] bool isSoftwareRenderer(const QString& renderer);

/// The colour the test picture must have at pixel (@p x, @p y): red grows with x, green with y,
/// blue is one half.
[[nodiscard]] QColor expectedPictureColor(int x, int y);

/// Compares @p image (read back from the card) with expectedPictureColor() at a grid of points;
/// @p problem tells the first difference. A tolerance of a few levels allows for rounding.
[[nodiscard]] bool pictureMatches(const QImage& image, QString* problem = nullptr);

/// Draws the test picture on an offscreen OpenGL context of the running QGuiApplication and fills
/// the OpenGL fields of @p report. False when no context can be made.
bool drawTestPicture(Report* report);

/// Opens the hardware decoding backends (VAAPI, CUDA, Vulkan) and, with @p videoFile, decodes its
/// first frames with each backend; fills the video fields of @p report and the report lines.
void checkVideoDecoding(const QString& videoFile, Report* report);

/// Runs the whole check and returns the report (QGuiApplication required).
[[nodiscard]] Report run(const QString& videoFile = {});

/// The `--graphics-check` mode of main(): runs the check, prints the report to stdout and returns
/// the exit code (0 = the card drew the picture correctly, 1 = it did not or none was found).
int runCommandLine(const QString& videoFile);

} // namespace kk::graphicscheck
