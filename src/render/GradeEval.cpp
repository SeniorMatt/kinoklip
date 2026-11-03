// SPDX-License-Identifier: GPL-3.0-or-later
// GradeEval — preparing a grade for one frame and the CPU per-pixel evaluation (the
// reference of render/GlGrade.cpp; keep both in step).
#include "render/GradeEval.h"

#include "render/RenderLog.h"

#include <QSet>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace kk::render {

namespace {

using namespace grademath;

/// Rows per CPU work item (small images run on the calling thread).
constexpr int kRowsPerTask = 16;
/// Images with fewer pixels than this are graded on the calling thread.
constexpr int kParallelPixels = 64 * 64;

/// Logs an unreadable LUT once per path.
void warnLut(const QString& path, const QString& why)
{
    static std::mutex mutex;
    static QSet<QString> warned;
    const std::lock_guard lock(mutex);
    if (warned.contains(path))
        return;
    warned.insert(path);
    qCWarning(lcRender) << "grade LUT" << path << "skipped:" << why;
}

} // namespace

PreparedGrade prepareGrade(const ClipGrade& grade, int64_t frame, const PictureGeometry& geometry)
{
    PreparedGrade out;
    const std::vector<int> order = gradeops::evaluationOrder(grade);
    if (order.empty())
        return out;
    // Node id -> position in the evaluation order.
    QHash<quint64, int> slot;
    for (size_t i = 0; i < order.size(); ++i)
        slot.insert(grade.nodes[order[i]].id, int(i));
    out.nodes.reserve(order.size());
    for (const int index : order) {
        const GradeNode& n = grade.nodes[index];
        PreparedGradeNode p;
        p.id = n.id;
        p.type = n.type;
        const int maxInputs = n.isMixer() ? kMaxMixerInputs : 1;
        for (const quint64 in : n.inputs) {
            if (int(p.inputs.size()) >= maxInputs)
                break;
            p.inputs.push_back(in == 0 ? -1 : slot.value(in, -1));
        }
        if (p.inputs.empty())
            p.inputs.push_back(-1);
        if (!n.isMixer()) {
            p.bypassed = !n.enabled;
            p.mix = float(std::clamp(n.value(gradekeys::Mix, frame), 0.0, 1.0));
            if (!p.bypassed) {
                p.primaries = PrimaryValues::at(n, frame);
                p.primariesIdentity = p.primaries.isIdentity();
                if (!n.curves.isIdentity())
                    p.curves = std::make_shared<const CurveTables>(CurveTables::bake(n.curves));
                if (!n.lutPath.isEmpty()) {
                    QString why;
                    p.lut = loadCubeLut(n.lutPath, &why);
                    if (!p.lut)
                        warnLut(n.lutPath, why);
                }
                p.tetrahedral = n.lutInterpolation == LutInterpolation::Tetrahedral;
                p.corrects = p.mix > 0.0f && (!p.primariesIdentity || p.curves || p.lut);
                p.qualifier = n.qualifier;
                p.windows = evaluateWindows(n, frame, geometry);
                p.keyed = n.qualifier.enabled || !p.windows.empty();
            }
        }
        out.nodes.push_back(std::move(p));
    }
    // Layer mixers read the keys of their upper inputs.
    for (const PreparedGradeNode& p : out.nodes) {
        if (p.type != GradeNodeType::Layer)
            continue;
        for (size_t i = 1; i < p.inputs.size(); ++i) {
            if (p.inputs[i] >= 0)
                out.nodes[size_t(p.inputs[i])].keyNeeded = true;
        }
    }
    out.output = int(out.nodes.size()) - 1;
    out.identity
        = std::all_of(out.nodes.cbegin(), out.nodes.cend(), [](const PreparedGradeNode& p) {
              return p.type == GradeNodeType::Corrector && !p.corrects;
          });
    // Mixers over identity correctors still mix identical pictures, except a Layer mixer
    // whose upper input is bypassed (alpha 0: the lower input shows) - also identical. So the
    // test above (every node a non-correcting corrector) stays conservative: any mixer renders.
    return out;
}

void gradePixel(
    const PreparedGrade& g, float rgb[3], float px, float py, std::vector<float>& scratch)
{
    if (g.output < 0)
        return;
    const size_t count = g.nodes.size();
    if (scratch.size() < count * 4)
        scratch.resize(count * 4);
    // Node i: scratch[4i .. 4i+2] = output colour, scratch[4i+3] = layer alpha.
    auto colour
        = [&](int index) -> const float* { return index < 0 ? rgb : &scratch[size_t(index) * 4]; };
    for (size_t i = 0; i < count; ++i) {
        const PreparedGradeNode& n = g.nodes[i];
        float* o = &scratch[i * 4];
        switch (n.type) {
        case GradeNodeType::Corrector: {
            const float* in = colour(n.inputs[0]);
            o[0] = in[0], o[1] = in[1], o[2] = in[2];
            if (n.bypassed) {
                o[3] = 0.0f;
                break;
            }
            float key = n.mix;
            if (n.keyed && (n.corrects || n.keyNeeded)) {
                key *= qualifierKey(n.qualifier, in);
                key *= windowsMatte(n.windows, px, py);
            }
            o[3] = key;
            if (!n.corrects || key <= 0.0f)
                break;
            float c[3] = {in[0], in[1], in[2]};
            if (!n.primariesIdentity)
                applyPrimaries(n.primaries, c);
            if (n.curves)
                applyCurves(*n.curves, c);
            if (n.lut) {
                if (n.tetrahedral)
                    n.lut->applyTetrahedral(c);
                else
                    n.lut->apply(c);
            }
            for (int ch = 0; ch < 3; ++ch)
                o[ch] = in[ch] + (c[ch] - in[ch]) * key;
            break;
        }
        case GradeNodeType::Parallel: {
            float sum[3] = {0, 0, 0};
            for (const int in : n.inputs) {
                const float* c = colour(in);
                sum[0] += c[0], sum[1] += c[1], sum[2] += c[2];
            }
            const float w = 1.0f / float(n.inputs.size());
            o[0] = sum[0] * w, o[1] = sum[1] * w, o[2] = sum[2] * w;
            o[3] = 1.0f;
            break;
        }
        case GradeNodeType::Layer: {
            const float* bottom = colour(n.inputs[0]);
            o[0] = bottom[0], o[1] = bottom[1], o[2] = bottom[2];
            for (size_t k = 1; k < n.inputs.size(); ++k) {
                const int in = n.inputs[k];
                const PreparedGradeNode* top = in >= 0 ? &g.nodes[size_t(in)] : nullptr;
                if (top && top->type == GradeNodeType::Corrector) {
                    layerOver(o, colour(top->inputs[0]), colour(in), scratch[size_t(in) * 4 + 3]);
                } else {
                    const float* c = colour(in); // a mixer or the input picture: opaque
                    o[0] = c[0], o[1] = c[1], o[2] = c[2];
                }
            }
            o[3] = 1.0f;
            break;
        }
        }
    }
    const float* result = colour(g.output);
    rgb[0] = result[0], rgb[1] = result[1], rgb[2] = result[2];
}

void applyGrade(const PreparedGrade& grade, QImage& image)
{
    if (grade.identity || image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();
    // Detach once here: scanLine() on a shared image detaches, and from several threads at
    // once that races (each copies, the rows land in different buffers).
    uchar* bits = image.bits();
    const qsizetype stride = image.bytesPerLine();
    auto rows = [&](int y0, int y1) {
        std::vector<float> scratch;
        for (int y = y0; y < y1; ++y) {
            auto* line = reinterpret_cast<QRgb*>(bits + y * stride);
            for (int x = 0; x < w; ++x) {
                const QRgb p = line[x];
                const int a = qAlpha(p);
                if (a == 0)
                    continue;
                const float inv = 1.0f / float(a);
                float c[3] = {float(qRed(p)) * inv, float(qGreen(p)) * inv, float(qBlue(p)) * inv};
                gradePixel(grade, c, float(x) + 0.5f, float(y) + 0.5f, scratch);
                const float fa = float(a);
                auto q = [fa](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * fa)); };
                line[x] = qRgba(q(c[0]), q(c[1]), q(c[2]), a);
            }
        }
    };
    if (qint64(w) * h < kParallelPixels) {
        rows(0, h);
        return;
    }
    std::vector<int> starts;
    for (int y = 0; y < h; y += kRowsPerTask)
        starts.push_back(y);
    QtConcurrent::blockingMap(starts, [&](int y0) { rows(y0, std::min(h, y0 + kRowsPerTask)); });
}

void applyGrade(const ClipGrade& grade, int64_t frame, QImage& image)
{
    PictureGeometry g;
    g.width = image.width();
    g.height = image.height();
    applyGrade(prepareGrade(grade, frame, g), image);
}

} // namespace kk::render
