// SPDX-License-Identifier: GPL-3.0-or-later
// MatteBackend — one model session that turns a picture into a matte (spec 78). The real one
// (RvmBackend.cpp) runs Robust Video Matting with ONNX Runtime; tests plug in a stub. A backend
// is used by one thread only. Recurrent models keep their state between process() calls.
#pragma once

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

namespace kk::matting {

/// How much of the picture the model looks at.
enum class MatteQuality {
    Preview = 0, ///< small input: fast enough to follow playback in the background
    Full = 1 ///< large input: finer edges, slower; for the final render
};

/// The two models Background Removal can run (spec 78.9). Both are separate downloads.
enum class MatteModel {
    Video = 0, ///< Robust Video Matting: people in video, keeps its state from frame to frame
    Image = 1 ///< BiRefNet: any subject on any background, every frame on its own
};

/// Longest edge of the picture handed to the model.
[[nodiscard]] int inputEdge(MatteQuality quality, MatteModel model = MatteModel::Video);
/// True if @p path is a still picture (png, jpg, ...), judged by its suffix.
[[nodiscard]] bool isStillImagePath(const QString& path);
/// The model a clip uses: @p choice is the effect's Model parameter ("auto", "video", "image"; any
/// other text means "auto"); automatic picks the image model for a still picture and the video
/// model for everything else.
[[nodiscard]] MatteModel resolveModel(const QString& choice, bool still);
/// RVM's downsample_ratio for a picture of @p edge pixels on its long side: the model's inner
/// work happens at about 256 (preview) or 512 (full) pixels.
[[nodiscard]] float downsampleRatio(MatteQuality quality, int edge);

class MatteBackend
{
public:
    virtual ~MatteBackend() = default;
    /// @p rgb: QImage::Format_RGB888, width and height multiples of 4. @p reset: forget the
    /// recurrent state (a seek, a cut, the first frame; models without state ignore it). @return
    /// the alpha matte, a Format_Grayscale8 image of the same size (255 = person); a null image on
    /// failure.
    [[nodiscard]] virtual QImage process(const QImage& rgb, MatteQuality quality, bool reset) = 0;
};

using BackendFactory
    = std::function<std::unique_ptr<MatteBackend>(const QString& modelPath, QString* why)>;

/// True if this build has an inference runtime (ONNX Runtime).
[[nodiscard]] bool runtimeCompiledIn();
/// Runtime name and version for Preferences and the log ("ONNX Runtime 1.29.0"); empty if none.
[[nodiscard]] QString runtimeVersion();
/// The Robust Video Matting session of @p modelPath, or nullptr with @p why (no runtime in this
/// build, file unreadable, not the expected model).
[[nodiscard]] std::unique_ptr<MatteBackend> createRvmBackend(
    const QString& modelPath, QString* why);

/// The BiRefNet session of @p modelPath (any-subject model, fixed 1024 x 1024 input), or nullptr
/// with @p why.
[[nodiscard]] std::unique_ptr<MatteBackend> createBirefnetBackend(
    const QString& modelPath, QString* why);
/// The session of @p model's file.
[[nodiscard]] std::unique_ptr<MatteBackend> createBackend(
    MatteModel model, const QString& modelPath, QString* why);

} // namespace kk::matting
