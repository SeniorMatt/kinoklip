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
                p.mixer = MixerValues::at(n, frame);
                p.mixes = !p.mixer.isIdentity();
                p.agx = AgxValues::at(n, frame);
                p.agxOn = p.agx.on;
                p.hdr = HdrValues::at(n, frame);
                p.hdrOn = !p.hdr.isIdentity();
                if (!n.warp.isIdentity())
                    p.warp = std::make_shared<const WarpTables>(WarpTables::bake(n.warp));
                if (!n.curves.isIdentity())
                    p.curves = std::make_shared<const CurveTables>(CurveTables::bake(n.curves));
                if (!n.lutPath.isEmpty()) {
                    QString why;
                    p.lut = loadCubeLut(n.lutPath, &why);
                    if (!p.lut)
                        warnLut(n.lutPath, why);
                }
                p.softClip = SoftClipValues::at(n, frame);
                p.softClips = !p.softClip.isIdentity();
                p.tetrahedral = n.lutInterpolation == LutInterpolation::Tetrahedral;
                p.corrects = p.mix > 0.0f
                    && (!p.primariesIdentity || p.mixes || p.agxOn || p.hdrOn || p.curves
                        || p.softClips || p.warp || p.lut);
                p.blur = BlurValues::at(n, frame, geometry.height);
                p.blurs = p.mix > 0.0f && p.blur.active();
                p.qualifier = n.qualifier;
                p.windows = evaluateWindows(n, frame, geometry);
                p.keyed = n.qualifier.enabled || !p.windows.empty();
                p.finesse = MatteFinesse::at(n.qualifier, geometry.height);
                p.finesses = n.qualifier.enabled && p.finesse.active();
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
    // A matte nobody reads (identity node, no Layer mixer above) is not cleaned.
    for (PreparedGradeNode& p : out.nodes)
        p.finesses = p.finesses && (p.corrects || p.keyNeeded || p.blurs);
    out.output = int(out.nodes.size()) - 1;
    out.identity
        = std::all_of(out.nodes.cbegin(), out.nodes.cend(), [](const PreparedGradeNode& p) {
              return p.type == GradeNodeType::Corrector && !p.corrects && !p.blurs;
          });
    // Mixers over identity correctors still mix identical pictures, except a Layer mixer
    // whose upper input is bypassed (alpha 0: the lower input shows) - also identical. So the
    // test above (every node a non-correcting corrector) stays conservative: any mixer renders.
    return out;
}

void prependInputLut(PreparedGrade& g, const ClipAttributes& attributes)
{
    if (!attributes.hasInputLut())
        return;
    QString why;
    std::shared_ptr<const CubeLut> lut = loadCubeLut(attributes.inputLut, &why);
    if (!lut) {
        warnLut(attributes.inputLut, why);
        return;
    }
    PreparedGradeNode first;
    first.type = GradeNodeType::Corrector;
    first.inputs = {-1};
    first.lut = std::move(lut);
    first.tetrahedral = true;
    first.corrects = true;
    for (PreparedGradeNode& n : g.nodes) {
        for (int& in : n.inputs)
            in = in < 0 ? 0 : in + 1;
    }
    g.nodes.insert(g.nodes.begin(), std::move(first));
    // output -1 (empty or disconnected grade): the picture leaves with just the LUT.
    g.output = g.output < 0 ? 0 : g.output + 1;
    g.identity = false;
}

PreparedGrade prepareClipGrade(const ClipGrade& grade, const ClipAttributes& attributes,
    int64_t frame, const PictureGeometry& geometry)
{
    PreparedGrade out = prepareGrade(grade, frame, geometry);
    prependInputLut(out, attributes);
    return out;
}

void applyInputLut(const ClipAttributes& attributes, QImage& image)
{
    if (!attributes.hasInputLut() || image.isNull())
        return;
    PreparedGrade g;
    prependInputLut(g, attributes);
    applyGrade(g, image);
}

namespace {

/// Evaluates node @p i of @p g for one pixel into scratch[4i .. 4i+3] (colour, layer alpha);
/// @p rgb is the grade's input colour, scratch holds the outputs of the nodes before @p i.
void evalNode(const PreparedGrade& g, size_t i, const float rgb[3], float px, float py,
    std::vector<float>& scratch)
{
    auto colour
        = [&](int index) -> const float* { return index < 0 ? rgb : &scratch[size_t(index) * 4]; };
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
        if (n.mixes)
            applyMixer(n.mixer, c);
        if (n.hdrOn)
            applyHdr(n.hdr, c);
        if (n.curves)
            applyCurves(*n.curves, c);
        if (n.softClips)
            applySoftClip(n.softClip, c);
        if (n.warp)
            applyWarp(*n.warp, c);
        if (n.lut) {
            if (n.tetrahedral)
                n.lut->applyTetrahedral(c);
            else
                n.lut->apply(c);
        }
        if (n.agxOn)
            applyAgx(n.agx, c);
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

} // namespace

void gradePixel(
    const PreparedGrade& g, float rgb[3], float px, float py, std::vector<float>& scratch)
{
    if (g.output < 0)
        return;
    const size_t count = g.nodes.size();
    if (scratch.size() < count * 4)
        scratch.resize(count * 4);
    // Node i: scratch[4i .. 4i+2] = output colour, scratch[4i+3] = layer alpha.
    for (size_t i = 0; i < count; ++i)
        evalNode(g, i, rgb, px, py, scratch);
    const float* result = &scratch[size_t(g.output) * 4];
    rgb[0] = result[0], rgb[1] = result[1], rgb[2] = result[2];
}

namespace {

/// Runs @p fn(y0, y1) over row bands of an image @p w x @p h, on several threads when large.
template<typename Fn>
void forRowBands(int w, int h, Fn&& fn)
{
    if (qint64(w) * h < kParallelPixels) {
        fn(0, h);
        return;
    }
    std::vector<int> starts;
    for (int y = 0; y < h; y += kRowsPerTask)
        starts.push_back(y);
    QtConcurrent::blockingMap(starts, [&](int y0) { fn(y0, std::min(h, y0 + kRowsPerTask)); });
}

} // namespace

std::vector<float> qualifierMatte(const GradeQualifier& q, const MatteFinesse& finesse,
    const std::vector<float>& rgb, int w, int h)
{
    std::vector<float> matte(size_t(w) * size_t(h));
    forRowBands(w, h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * size_t(w); i < size_t(y1) * size_t(w); ++i)
            matte[i] = qualifierKey(q, &rgb[i * 3]);
    });
    applyMatteFinesse(finesse, matte, w, h);
    return matte;
}

void gaussianBlurRgba(const std::vector<float>& src, std::vector<float>& dst, int w, int h,
    float sigmaH, float sigmaV)
{
    const std::vector<float> kh = blurKernel(sigmaH);
    const std::vector<float> kv = blurKernel(sigmaV);
    const int rh = int(kh.size() / 2);
    const int rv = int(kv.size() / 2);
    std::vector<float> tmp(src.size());
    forRowBands(w, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* row = &src[size_t(y) * size_t(w) * 4];
            float* out = &tmp[size_t(y) * size_t(w) * 4];
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int k = -rh; k <= rh; ++k) {
                    const float* p = row + size_t(std::clamp(x + k, 0, w - 1)) * 4;
                    const float wt = kh[size_t(k + rh)];
                    acc[0] += wt * p[0], acc[1] += wt * p[1], acc[2] += wt * p[2];
                    acc[3] += wt * p[3];
                }
                std::copy(acc, acc + 4, out + size_t(x) * 4);
            }
        }
    });
    dst.assign(src.size(), 0.0f);
    forRowBands(w, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* out = &dst[size_t(y) * size_t(w) * 4];
            for (int k = -rv; k <= rv; ++k) {
                const float* row = &tmp[size_t(std::clamp(y + k, 0, h - 1)) * size_t(w) * 4];
                const float wt = kv[size_t(k + rv)];
                for (size_t i = 0; i < size_t(w) * 4; ++i)
                    out[i] += wt * row[i];
            }
        }
    });
}

void applyBlurStage(const BlurStage& stage, std::vector<float>& rgb,
    const std::vector<float>& alpha, int w, int h)
{
    if (!stage.active())
        return;
    const size_t count = size_t(w) * size_t(h);
    std::vector<float> premult(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const float a = alpha[i];
        premult[i * 4] = rgb[i * 3] * a;
        premult[i * 4 + 1] = rgb[i * 3 + 1] * a;
        premult[i * 4 + 2] = rgb[i * 3 + 2] * a;
        premult[i * 4 + 3] = a;
    }
    std::vector<float> blurred[3];
    if (stage.uniformSigma()) {
        gaussianBlurRgba(premult, blurred[0], w, h, stage.sigmaH[0], stage.sigmaV[0]);
    } else {
        for (int ch = 0; ch < 3; ++ch)
            gaussianBlurRgba(premult, blurred[ch], w, h, stage.sigmaH[ch], stage.sigmaV[ch]);
    }
    const bool uniform = stage.uniformSigma();
    forRowBands(w, h, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * size_t(w); i < size_t(y1) * size_t(w); ++i) {
            if (alpha[i] <= 0.0f)
                continue;
            const float weight = stage.midWeight ? midtoneWeight(luma(&rgb[i * 3])) : 1.0f;
            float out[3];
            for (int ch = 0; ch < 3; ++ch) {
                const float* b = &(uniform ? blurred[0] : blurred[ch])[i * 4];
                const float c = rgb[i * 3 + size_t(ch)];
                const float bc = b[3] > 0.0f ? b[ch] / b[3] : c;
                out[ch] = blurCombine(c, bc, stage.mix[ch], stage.sharp[ch], stage.coring, weight);
            }
            std::copy(out, out + 3, &rgb[i * 3]);
        }
    });
}

namespace {

/// applyGrade() for a grade with blurring nodes: whole-picture buffers per node, so a blur
/// reads the neighbours of every pixel (nodes without a blur run the per-pixel maths).
void applyGradeBlurred(const PreparedGrade& g, QImage& image)
{
    const int w = image.width();
    const int h = image.height();
    const size_t count = size_t(w) * size_t(h);
    uchar* bits = image.bits();
    const qsizetype stride = image.bytesPerLine();
    std::vector<float> alpha(count), input(count * 3, 0.0f);
    forRowBands(w, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(bits + y * stride);
            for (int x = 0; x < w; ++x) {
                const QRgb p = line[x];
                const int a = qAlpha(p);
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                alpha[i] = float(a) / 255.0f;
                if (a == 0)
                    continue;
                const float inv = 1.0f / float(a);
                input[i * 3] = float(qRed(p)) * inv;
                input[i * 3 + 1] = float(qGreen(p)) * inv;
                input[i * 3 + 2] = float(qBlue(p)) * inv;
            }
        }
    });
    // Per node: straight colour (3 floats) and layer alpha (1) per pixel.
    const size_t nodes = g.nodes.size();
    std::vector<std::vector<float>> out(nodes);
    auto inputColour = [&](int index, size_t pixel) -> const float* {
        return index < 0 ? &input[pixel * 3] : &out[size_t(index)][pixel * 4];
    };
    for (size_t i = 0; i < nodes; ++i) {
        const PreparedGradeNode& n = g.nodes[i];
        out[i].assign(count * 4, 0.0f);
        if (n.type == GradeNodeType::Corrector && (n.blurs || n.finesses) && !n.bypassed) {
            std::vector<float> graded(count * 3), key(count, 0.0f), matte;
            if (n.finesses) {
                std::vector<float> in(count * 3);
                for (size_t p = 0; p < count; ++p)
                    std::copy_n(inputColour(n.inputs[0], p), 3, &in[p * 3]);
                matte = qualifierMatte(n.qualifier, n.finesse, in, w, h);
            }
            forRowBands(w, h, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const size_t p = size_t(y) * size_t(w) + size_t(x);
                        if (alpha[p] <= 0.0f)
                            continue;
                        const float* in = inputColour(n.inputs[0], p);
                        float k = n.mix;
                        if (n.keyed) {
                            k *= n.finesses ? matte[p] : qualifierKey(n.qualifier, in);
                            k *= windowsMatte(n.windows, float(x) + 0.5f, float(y) + 0.5f);
                        }
                        key[p] = k;
                        float c[3] = {in[0], in[1], in[2]};
                        if (n.corrects) { // colour corrections, ungated (the blur reads them all)
                            if (!n.primariesIdentity)
                                applyPrimaries(n.primaries, c);
                            if (n.mixes)
                                applyMixer(n.mixer, c);
                            if (n.hdrOn)
                                applyHdr(n.hdr, c);
                            if (n.curves)
                                applyCurves(*n.curves, c);
                            if (n.softClips)
                                applySoftClip(n.softClip, c);
                            if (n.warp)
                                applyWarp(*n.warp, c);
                            if (n.lut) {
                                if (n.tetrahedral)
                                    n.lut->applyTetrahedral(c);
                                else
                                    n.lut->apply(c);
                            }
                            if (n.agxOn)
                                applyAgx(n.agx, c);
                        }
                        std::copy(c, c + 3, &graded[p * 3]);
                    }
                }
            });
            for (const BlurStage& stage : n.blur.stages)
                applyBlurStage(stage, graded, alpha, w, h);
            forRowBands(w, h, [&](int y0, int y1) {
                for (size_t p = size_t(y0) * size_t(w); p < size_t(y1) * size_t(w); ++p) {
                    const float* in = inputColour(n.inputs[0], p);
                    float* o = &out[i][p * 4];
                    for (int ch = 0; ch < 3; ++ch)
                        o[ch] = in[ch] + (graded[p * 3 + size_t(ch)] - in[ch]) * key[p];
                    o[3] = key[p];
                }
            });
            continue;
        }
        // Nodes of the per-pixel maths: their input nodes' outputs go into the scratch.
        std::vector<int> need;
        for (const int in : n.inputs) {
            need.push_back(in);
            if (n.type == GradeNodeType::Layer && in >= 0)
                need.push_back(g.nodes[size_t(in)].inputs[0]);
        }
        forRowBands(w, h, [&](int y0, int y1) {
            std::vector<float> scratch(nodes * 4, 0.0f);
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t p = size_t(y) * size_t(w) + size_t(x);
                    if (alpha[p] <= 0.0f)
                        continue;
                    for (const int idx : need) {
                        if (idx >= 0)
                            std::copy_n(&out[size_t(idx)][p * 4], 4, &scratch[size_t(idx) * 4]);
                    }
                    evalNode(g, i, &input[p * 3], float(x) + 0.5f, float(y) + 0.5f, scratch);
                    std::copy_n(&scratch[i * 4], 4, &out[i][p * 4]);
                }
            }
        });
    }
    const std::vector<float>& result = out[size_t(g.output)];
    forRowBands(w, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* line = reinterpret_cast<QRgb*>(bits + y * stride);
            for (int x = 0; x < w; ++x) {
                const int a = qAlpha(line[x]);
                if (a == 0)
                    continue;
                const float* c = &result[(size_t(y) * size_t(w) + size_t(x)) * 4];
                const float fa = float(a);
                auto q = [fa](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * fa)); };
                line[x] = qRgba(q(c[0]), q(c[1]), q(c[2]), a);
            }
        }
    });
}

} // namespace

void applyGrade(const PreparedGrade& grade, QImage& image)
{
    if (grade.identity || image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();
    image.detach();
    if (std::any_of(grade.nodes.cbegin(), grade.nodes.cend(),
            [](const PreparedGradeNode& n) { return n.blurs || n.finesses; })) {
        applyGradeBlurred(grade, image);
        return;
    }
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
