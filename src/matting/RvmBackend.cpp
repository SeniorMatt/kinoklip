// SPDX-License-Identifier: GPL-3.0-or-later
// RvmBackend — Robust Video Matting (Lin et al., GPL-3.0, github.com/PeterL1n/RobustVideoMatting)
// run with ONNX Runtime (MIT) on the CPU. The model takes the picture (RGB, 0..1, NCHW), four
// recurrent states and downsample_ratio, and returns the foreground, the alpha and the new states;
// the states are fed back for the next frame, which keeps the edges steady (spec 78).
#include "matting/MatteBackend.h"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <cmath>

#ifdef KK_HAVE_ONNXRUNTIME
#include <array>
#include <onnxruntime_cxx_api.h>
#include <vector>
#endif

namespace kk::matting {

int inputEdge(MatteQuality quality)
{
    return quality == MatteQuality::Full ? 1280 : 512;
}

float downsampleRatio(MatteQuality quality, int edge)
{
    const float inner = quality == MatteQuality::Full ? 512.f : 256.f;
    return std::clamp(inner / float(std::max(1, edge)), 0.125f, 1.0f);
}

#ifdef KK_HAVE_ONNXRUNTIME

namespace {

Ort::Env& environment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "kinoklip");
    return env;
}

class RvmBackend final : public MatteBackend
{
public:
    explicit RvmBackend(const QString& path)
        : m_options()
    {
        const int threads = std::max(1, QThread::idealThreadCount() / 2);
        m_options.SetIntraOpNumThreads(threads);
        m_options.SetInterOpNumThreads(1);
        m_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        m_session
            = std::make_unique<Ort::Session>(environment(), path.toStdWString().c_str(), m_options);
#else
        m_session
            = std::make_unique<Ort::Session>(environment(), path.toStdString().c_str(), m_options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        auto names = [&](bool inputs) {
            std::vector<std::string> out;
            const size_t n = inputs ? m_session->GetInputCount() : m_session->GetOutputCount();
            for (size_t i = 0; i < n; ++i) {
                auto name = inputs ? m_session->GetInputNameAllocated(i, allocator)
                                   : m_session->GetOutputNameAllocated(i, allocator);
                out.emplace_back(name.get());
            }
            return out;
        };
        const auto in = names(true);
        const auto out = names(false);
        const char* wantIn[] = {"src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"};
        const char* wantOut[] = {"pha", "r1o", "r2o", "r3o", "r4o"};
        for (const char* w : wantIn) {
            if (std::find(in.begin(), in.end(), w) == in.end())
                throw std::runtime_error(std::string("the model has no input ") + w);
        }
        for (const char* w : wantOut) {
            if (std::find(out.begin(), out.end(), w) == out.end())
                throw std::runtime_error(std::string("the model has no output ") + w);
        }
        resetState();
    }

    QImage process(const QImage& rgb, MatteQuality quality, bool reset) override
    {
        if (rgb.format() != QImage::Format_RGB888 || rgb.isNull())
            return {};
        const int w = rgb.width(), h = rgb.height();
        if (reset) {
            resetState();
            // The first frames of a sequence settle the recurrent state: the picture is
            // shown to the model twice so a single frame already gets a stable matte.
            run(rgb, quality, w, h);
        }
        return run(rgb, quality, w, h);
    }

private:
    struct State
    {
        std::vector<float> data {0.f};
        std::array<int64_t, 4> shape {1, 1, 1, 1};
    };

    void resetState()
    {
        for (State& s : m_state)
            s = State();
    }

    QImage run(const QImage& rgb, MatteQuality quality, int w, int h)
    {
        try {
            const size_t plane = size_t(w) * size_t(h);
            m_input.resize(plane * 3);
            for (int y = 0; y < h; ++y) {
                const uchar* line = rgb.constScanLine(y);
                float* r = m_input.data() + size_t(y) * size_t(w);
                float* g = r + plane;
                float* b = g + plane;
                for (int x = 0; x < w; ++x) {
                    r[x] = float(line[3 * x]) * (1.f / 255.f);
                    g[x] = float(line[3 * x + 1]) * (1.f / 255.f);
                    b[x] = float(line[3 * x + 2]) * (1.f / 255.f);
                }
            }
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 4> srcShape {1, 3, h, w};
            float ratio = downsampleRatio(quality, std::max(w, h));
            const std::array<int64_t, 1> ratioShape {1};
            std::vector<Ort::Value> inputs;
            inputs.push_back(Ort::Value::CreateTensor<float>(
                mem, m_input.data(), m_input.size(), srcShape.data(), srcShape.size()));
            for (State& s : m_state) {
                inputs.push_back(Ort::Value::CreateTensor<float>(
                    mem, s.data.data(), s.data.size(), s.shape.data(), s.shape.size()));
            }
            inputs.push_back(Ort::Value::CreateTensor<float>(mem, &ratio, 1, ratioShape.data(), 1));
            static const char* kInNames[] = {"src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"};
            static const char* kOutNames[] = {"pha", "r1o", "r2o", "r3o", "r4o"};
            auto outputs = m_session->Run(
                Ort::RunOptions {nullptr}, kInNames, inputs.data(), inputs.size(), kOutNames, 5);
            const float* pha = outputs[0].GetTensorData<float>();
            QImage matte(w, h, QImage::Format_Grayscale8);
            for (int y = 0; y < h; ++y) {
                uchar* line = matte.scanLine(y);
                const float* src = pha + size_t(y) * size_t(w);
                for (int x = 0; x < w; ++x)
                    line[x] = uchar(std::clamp(src[x], 0.f, 1.f) * 255.f + 0.5f);
            }
            for (int i = 0; i < 4; ++i) {
                const auto info = outputs[size_t(i) + 1].GetTensorTypeAndShapeInfo();
                const auto shape = info.GetShape();
                if (shape.size() != 4)
                    throw std::runtime_error("unexpected recurrent state shape");
                const size_t count = info.GetElementCount();
                const float* data = outputs[size_t(i) + 1].GetTensorData<float>();
                State& s = m_state[size_t(i)];
                s.data.assign(data, data + count);
                for (size_t d = 0; d < 4; ++d)
                    s.shape[d] = shape[d];
            }
            return matte;
        } catch (const std::exception&) {
            resetState();
            return {};
        }
    }

    Ort::SessionOptions m_options;
    std::unique_ptr<Ort::Session> m_session;
    std::array<State, 4> m_state;
    std::vector<float> m_input;
};

} // namespace

bool runtimeCompiledIn()
{
    return true;
}

QString runtimeVersion()
{
    return QStringLiteral("ONNX Runtime %1")
        .arg(QString::fromLatin1(Ort::GetVersionString().c_str()));
}

std::unique_ptr<MatteBackend> createRvmBackend(const QString& modelPath, QString* why)
{
    try {
        return std::make_unique<RvmBackend>(modelPath);
    } catch (const std::exception& e) {
        if (why)
            *why = QCoreApplication::translate("kk::matting", "The model could not be loaded: %1")
                       .arg(QString::fromUtf8(e.what()));
        return nullptr;
    }
}

#else // no ONNX Runtime in this build

bool runtimeCompiledIn()
{
    return false;
}

QString runtimeVersion()
{
    return {};
}

std::unique_ptr<MatteBackend> createRvmBackend(const QString&, QString* why)
{
    if (why)
        *why = QCoreApplication::translate(
            "kk::matting", "This build of Kinoklip has no AI runtime (ONNX Runtime).");
    return nullptr;
}

#endif

} // namespace kk::matting
