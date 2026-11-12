// SPDX-License-Identifier: GPL-3.0-or-later
// BirefnetBackend — BiRefNet (Zheng et al., MIT, github.com/ZhengPeng7/BiRefNet; official ONNX
// export of the general model with the Swin-T backbone) run with ONNX Runtime (MIT) on the CPU.
// One input "input_image" (1 x 3 x 1024 x 1024, ImageNet-normalised RGB), one output of logits of
// the same size. Pre- and post-processing follows BiRefNet's own inference script and rembg's
// session (MIT): squash to the square, normalise, sigmoid. The matte is then carried up to the
// picture's resolution and, for large pictures, snapped to its edges with a guided filter
// (MatteMath.h). No state between pictures.
#include "matting/MatteBackend.h"
#include "matting/MatteMath.h"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>

#ifdef KK_HAVE_ONNXRUNTIME
#include <array>
#include <onnxruntime_cxx_api.h>
#include <vector>
#endif

namespace kk::matting {

#ifdef KK_HAVE_ONNXRUNTIME

namespace {

constexpr int kSide = 1024;

Ort::Env& environment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "kinoklip-birefnet");
    return env;
}

class BirefnetBackend final : public MatteBackend
{
public:
    explicit BirefnetBackend(const QString& path)
    {
        m_options.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount() / 2));
        m_options.SetInterOpNumThreads(1);
        m_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // The arena keeps every temporary of the 1024 px attention layers alive: about 2x the
        // memory.
        m_options.DisableCpuMemArena();
        m_options.DisableMemPattern();
#ifdef _WIN32
        m_session
            = std::make_unique<Ort::Session>(environment(), path.toStdWString().c_str(), m_options);
#else
        m_session
            = std::make_unique<Ort::Session>(environment(), path.toStdString().c_str(), m_options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        if (m_session->GetInputCount() != 1 || m_session->GetOutputCount() < 1
            || std::string(m_session->GetInputNameAllocated(0, allocator).get()) != "input_image")
            throw std::runtime_error("the file is not the BiRefNet model (no input_image)");
        m_output = m_session->GetOutputNameAllocated(0, allocator).get();
    }

    QImage process(const QImage& rgb, MatteQuality quality, bool) override
    {
        if (rgb.format() != QImage::Format_RGB888 || rgb.isNull())
            return {};
        try {
            birefnetInput(rgb, kSide, m_input);
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 4> shape {1, 3, kSide, kSide};
            Ort::Value input = Ort::Value::CreateTensor<float>(
                mem, m_input.data(), m_input.size(), shape.data(), shape.size());
            const char* inNames[] = {"input_image"};
            const char* outNames[] = {m_output.c_str()};
            auto outputs
                = m_session->Run(Ort::RunOptions {nullptr}, inNames, &input, 1, outNames, 1);
            const auto info = outputs[0].GetTensorTypeAndShapeInfo();
            if (info.GetElementCount() != size_t(kSide) * size_t(kSide))
                return {};
            QImage matte = matteFromOutput(outputs[0].GetTensorData<float>(), kSide, true);
            matte = resizeMatte(matte, rgb.width(), rgb.height());
            if (quality == MatteQuality::Full || std::max(rgb.width(), rgb.height()) > kSide)
                matte = refineMatte(
                    matte, rgb, refineRadius(std::max(rgb.width(), rgb.height())), 0.001f);
            return matte;
        } catch (const std::exception&) {
            return {};
        }
    }

private:
    Ort::SessionOptions m_options;
    std::unique_ptr<Ort::Session> m_session;
    std::string m_output;
    std::vector<float> m_input;
};

} // namespace

std::unique_ptr<MatteBackend> createBirefnetBackend(const QString& modelPath, QString* why)
{
    try {
        return std::make_unique<BirefnetBackend>(modelPath);
    } catch (const std::exception& e) {
        if (why)
            *why = QCoreApplication::translate("kk::matting", "The model could not be loaded: %1")
                       .arg(QString::fromUtf8(e.what()));
        return nullptr;
    }
}

#else

std::unique_ptr<MatteBackend> createBirefnetBackend(const QString&, QString* why)
{
    if (why)
        *why = QCoreApplication::translate(
            "kk::matting", "This build of Kinoklip has no AI runtime (ONNX Runtime).");
    return nullptr;
}

#endif

std::unique_ptr<MatteBackend> createBackend(
    MatteModel model, const QString& modelPath, QString* why)
{
    return model == MatteModel::Image ? createBirefnetBackend(modelPath, why)
                                      : createRvmBackend(modelPath, why);
}

} // namespace kk::matting
