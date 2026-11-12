// SPDX-License-Identifier: GPL-3.0-or-later
#include "matting/MatteMath.h"

#include <algorithm>
#include <cmath>

namespace kk::matting {

void birefnetInput(const QImage& rgbIn, int size, std::vector<float>& out)
{
    QImage rgb = rgbIn.format() == QImage::Format_RGB888
        ? rgbIn
        : rgbIn.convertToFormat(QImage::Format_RGB888);
    if (rgb.width() != size || rgb.height() != size)
        rgb = rgb.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const size_t plane = size_t(size) * size_t(size);
    out.resize(plane * 3);
    static constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
    static constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};
    float scale[3], offset[3];
    for (int c = 0; c < 3; ++c) {
        scale[c] = 1.f / (255.f * kStd[c]);
        offset[c] = -kMean[c] / kStd[c];
    }
    for (int y = 0; y < size; ++y) {
        const uchar* line = rgb.constScanLine(y);
        float* r = out.data() + size_t(y) * size_t(size);
        float* g = r + plane;
        float* b = g + plane;
        for (int x = 0; x < size; ++x) {
            r[x] = float(line[3 * x]) * scale[0] + offset[0];
            g[x] = float(line[3 * x + 1]) * scale[1] + offset[1];
            b[x] = float(line[3 * x + 2]) * scale[2] + offset[2];
        }
    }
}

float sigmoid(float x)
{
    return 1.f / (1.f + std::exp(-x));
}

QImage matteFromOutput(const float* values, int size, bool logits)
{
    QImage matte(size, size, QImage::Format_Grayscale8);
    for (int y = 0; y < size; ++y) {
        uchar* line = matte.scanLine(y);
        const float* src = values + size_t(y) * size_t(size);
        for (int x = 0; x < size; ++x) {
            const float v = logits ? sigmoid(src[x]) : src[x];
            line[x] = uchar(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
        }
    }
    return matte;
}

QImage resizeMatte(const QImage& matte, int width, int height)
{
    if (matte.width() == width && matte.height() == height)
        return matte;
    return matte.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

int refineRadius(int edge)
{
    return std::clamp(edge / 400, 2, 8);
}

namespace {

// Mean over a (2r+1) square, edges clamped to the pixels that exist (running sums, O(n)).
void boxMean(const std::vector<float>& src, std::vector<float>& dst, std::vector<float>& tmp, int w,
    int h, int r)
{
    tmp.resize(src.size());
    dst.resize(src.size());
    for (int y = 0; y < h; ++y) {
        const float* s = src.data() + size_t(y) * w;
        float* t = tmp.data() + size_t(y) * w;
        double sum = 0;
        const int hi0 = std::min(r, w - 1);
        for (int x = 0; x <= hi0; ++x)
            sum += s[x];
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - r), hi = std::min(w - 1, x + r);
            t[x] = float(sum / double(hi - lo + 1));
            if (x + r + 1 <= w - 1)
                sum += s[x + r + 1];
            if (x - r >= 0)
                sum -= s[x - r];
        }
    }
    for (int x = 0; x < w; ++x) {
        double sum = 0;
        const int hi0 = std::min(r, h - 1);
        for (int y = 0; y <= hi0; ++y)
            sum += tmp[size_t(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0, y - r), hi = std::min(h - 1, y + r);
            dst[size_t(y) * w + x] = float(sum / double(hi - lo + 1));
            if (y + r + 1 <= h - 1)
                sum += tmp[size_t(y + r + 1) * w + x];
            if (y - r >= 0)
                sum -= tmp[size_t(y - r) * w + x];
        }
    }
}

} // namespace

QImage refineMatte(const QImage& matte, const QImage& guide, int radius, float eps)
{
    if (matte.isNull() || guide.isNull() || matte.size() != guide.size() || radius < 1)
        return matte;
    const int w = matte.width(), h = matte.height();
    const QImage g = guide.format() == QImage::Format_RGB888
        ? guide
        : guide.convertToFormat(QImage::Format_RGB888);
    const QImage m = matte.format() == QImage::Format_Grayscale8
        ? matte
        : matte.convertToFormat(QImage::Format_Grayscale8);
    const size_t n = size_t(w) * size_t(h);
    std::vector<float> I(n), p(n), Ip(n), II(n);
    for (int y = 0; y < h; ++y) {
        const uchar* gl = g.constScanLine(y);
        const uchar* ml = m.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            I[i] = (0.299f * gl[3 * x] + 0.587f * gl[3 * x + 1] + 0.114f * gl[3 * x + 2]) / 255.f;
            p[i] = float(ml[x]) / 255.f;
            Ip[i] = I[i] * p[i];
            II[i] = I[i] * I[i];
        }
    }
    std::vector<float> mI, mp, mIp, mII, tmp;
    boxMean(I, mI, tmp, w, h, radius);
    boxMean(p, mp, tmp, w, h, radius);
    boxMean(Ip, mIp, tmp, w, h, radius);
    boxMean(II, mII, tmp, w, h, radius);
    std::vector<float>& a = Ip; // reuse
    std::vector<float>& b = II;
    for (size_t i = 0; i < n; ++i) {
        const float var = mII[i] - mI[i] * mI[i];
        const float cov = mIp[i] - mI[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> ma, mb;
    boxMean(a, ma, tmp, w, h, radius);
    boxMean(b, mb, tmp, w, h, radius);
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* line = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            line[x] = uchar(std::clamp(ma[i] * I[i] + mb[i], 0.f, 1.f) * 255.f + 0.5f);
        }
    }
    return out;
}

} // namespace kk::matting
