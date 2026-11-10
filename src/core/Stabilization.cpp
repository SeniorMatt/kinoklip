// SPDX-License-Identifier: GPL-3.0-or-later
// Stabilization maths and project-file form (see Stabilization.h).
#include "core/Stabilization.h"

#include "core/TimelineData.h"

#include <QByteArray>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kk::stabilize {

namespace {

/// Most frames a stored path may have (a day at 100 fps is 8.6 million).
constexpr int64_t kMaxFrames = 20'000'000;
constexpr double kEdgeTolerance = 1e-9;
constexpr int kZoomSteps = 24; ///< bisection steps of the cover search
constexpr double kFrameTolerance = 0.01; ///< of a frame, when turning a time into a frame number

/// Identity values of the eight stored homography numbers (row-major, the ninth is 1).
constexpr double kIdentityHomography[8] = {1, 0, 0, 0, 1, 0, 0, 0};

Mat3 similarityMatrix(double tx, double ty, double theta, double logScale)
{
    const double s = std::exp(logScale);
    const double c = s * std::cos(theta);
    const double n = s * std::sin(theta);
    Mat3 r;
    r.m = {c, -n, tx, n, c, ty, 0, 0, 1};
    return r;
}

/// Gaussian low-pass of @p x with the standard deviation @p sigma (frames): a locally linear
/// fit with Gaussian weights, evaluated at each frame. In the middle that is the plain Gaussian
/// average; near the ends, where only one side exists, the fit still follows a steady drift
/// (a plain average of the half window would be pulled towards the middle).
std::vector<double> smoothed(const std::vector<double>& x, double sigma)
{
    const int64_t n = int64_t(x.size());
    if (n < 3 || sigma < 1e-3)
        return x;
    const int64_t radius = std::min<int64_t>(n - 1, int64_t(std::ceil(3.0 * sigma)));
    std::vector<double> weight(static_cast<size_t>(radius) + 1);
    for (int64_t d = 0; d <= radius; ++d)
        weight[static_cast<size_t>(d)] = std::exp(-0.5 * double(d * d) / (sigma * sigma));
    std::vector<double> out(static_cast<size_t>(n));
    for (int64_t t = 0; t < n; ++t) {
        double s0 = 0, s1 = 0, s2 = 0, t0 = 0, t1 = 0;
        for (int64_t i = std::max<int64_t>(0, t - radius); i <= std::min(n - 1, t + radius); ++i) {
            const double d = double(i - t);
            const double w = weight[static_cast<size_t>(std::abs(i - t))];
            const double v = x[static_cast<size_t>(i)];
            s0 += w;
            s1 += w * d;
            s2 += w * d * d;
            t0 += w * v;
            t1 += w * d * v;
        }
        const double det = s0 * s2 - s1 * s1;
        out[static_cast<size_t>(t)] = det > 1e-9 * s0 * s2 ? (t0 * s2 - t1 * s1) / det : t0 / s0;
    }
    return out;
}

/// The smallest scale z in [1, kSearchZoom] for which the frame rectangle, shrunk by z about the
/// centre, lies inside the picture after the correction @p t is undone; kSearchZoom when even that
/// does not cover.
double requiredZoom(const Mat3& t, double aspect)
{
    bool ok = true;
    const Mat3 back = t.inverse(&ok);
    if (!ok)
        return kSearchZoom;
    const double hw = 0.5, hh = 0.5 * aspect;
    const double corners[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    auto covered = [&](double z) {
        for (const auto& c : corners) {
            double x = 0, y = 0;
            back.apply(c[0] / z, c[1] / z, &x, &y);
            if (!(std::abs(x) <= hw + kEdgeTolerance && std::abs(y) <= hh + kEdgeTolerance))
                return false;
        }
        return true;
    };
    if (covered(1.0))
        return 1.0;
    if (!covered(kSearchZoom))
        return kSearchZoom;
    double lo = 1.0, hi = kSearchZoom;
    for (int i = 0; i < kZoomSteps; ++i) {
        const double mid = 0.5 * (lo + hi);
        (covered(mid) ? hi : lo) = mid;
    }
    return hi;
}

} // namespace

// ---- Mat3 ----

Mat3 Mat3::operator*(const Mat3& o) const
{
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k)
                s += m[size_t(i * 3 + k)] * o.m[size_t(k * 3 + j)];
            r.m[size_t(i * 3 + j)] = s;
        }
    }
    return r;
}

Mat3 Mat3::inverse(bool* ok) const
{
    const auto& a = m;
    const double c00 = a[4] * a[8] - a[5] * a[7];
    const double c01 = a[5] * a[6] - a[3] * a[8];
    const double c02 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (!(std::abs(det) > 1e-14)) {
        if (ok)
            *ok = false;
        return Mat3 {};
    }
    if (ok)
        *ok = true;
    const double d = 1.0 / det;
    Mat3 r;
    r.m = {c00 * d, (a[2] * a[7] - a[1] * a[8]) * d, (a[1] * a[5] - a[2] * a[4]) * d, c01 * d,
        (a[0] * a[8] - a[2] * a[6]) * d, (a[2] * a[3] - a[0] * a[5]) * d, c02 * d,
        (a[1] * a[6] - a[0] * a[7]) * d, (a[0] * a[4] - a[1] * a[3]) * d};
    return r;
}

void Mat3::apply(double x, double y, double* ox, double* oy) const
{
    const double w = m[6] * x + m[7] * y + m[8];
    const double px = m[0] * x + m[1] * y + m[2];
    const double py = m[3] * x + m[4] * y + m[5];
    if (w > 1e-9) {
        *ox = px / w;
        *oy = py / w;
    } else {
        *ox = px * 1e9;
        *oy = py * 1e9;
    }
}

Mat3 Mat3::translation(double tx, double ty)
{
    Mat3 r;
    r.m = {1, 0, tx, 0, 1, ty, 0, 0, 1};
    return r;
}

// ---- Mode, settings ----

QString modeId(Mode mode)
{
    switch (mode) {
    case Mode::Perspective:
        return QStringLiteral("perspective");
    case Mode::Similarity:
        return QStringLiteral("similarity");
    case Mode::Translation:
        break;
    }
    return QStringLiteral("translation");
}

Mode modeFromId(const QString& id, Mode fallback)
{
    for (const Mode m : {Mode::Perspective, Mode::Similarity, Mode::Translation}) {
        if (modeId(m) == id)
            return m;
    }
    return fallback;
}

int paramCount(Mode mode)
{
    switch (mode) {
    case Mode::Perspective:
        return 8;
    case Mode::Similarity:
        return 4;
    case Mode::Translation:
        break;
    }
    return 2;
}

Settings clamped(Settings s)
{
    auto fix = [](double v, double lo, double hi, double fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    s.croppingRatio = fix(s.croppingRatio, kMinCroppingRatio, kMaxCroppingRatio, 1.0);
    s.smooth = fix(s.smooth, kMinSmooth, kMaxSmooth, 0.25);
    s.strength = fix(s.strength, kMinStrength, kMaxStrength, 1.0);
    return s;
}

// ---- Data ----

Data::Data(Mode mode, Rational fps, int64_t firstFrame, double aspect, std::vector<float> path)
    : m_mode(mode)
    , m_fps(fps)
    , m_first(firstFrame)
    , m_aspect(aspect > 0 ? aspect : 9.0 / 16.0)
    , m_path(std::move(path))
{
    m_frames = int64_t(m_path.size()) / paramCount(mode);
    m_path.resize(size_t(m_frames * paramCount(mode)));
}

bool operator==(const Data& a, const Data& b)
{
    return a.m_mode == b.m_mode && a.m_fps == b.m_fps && a.m_first == b.m_first
        && a.m_aspect == b.m_aspect && a.m_path == b.m_path;
}

Mat3 Data::matrixOf(const float* p, double strength) const
{
    Mat3 r;
    switch (m_mode) {
    case Mode::Translation:
        return Mat3::translation(strength * p[0], strength * p[1]);
    case Mode::Similarity:
        return similarityMatrix(strength * p[0], strength * p[1], strength * p[2], strength * p[3]);
    case Mode::Perspective:
        break;
    }
    for (int i = 0; i < 8; ++i)
        r.m[size_t(i)]
            = kIdentityHomography[i] + strength * (double(p[i]) - kIdentityHomography[i]);
    r.m[8] = 1.0;
    return r;
}

std::shared_ptr<const Data::Corrections> Data::corrections(double smooth, bool lock) const
{
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_haveCorrections && m_corrLock == lock && (lock || m_corrSmooth == smooth))
            return m_corr;
    }
    const int pc = paramCount(m_mode);
    const size_t n = size_t(m_frames);
    // The smooth path S(t): each stored number filtered on its own (small movements, so the
    // numbers of a similarity or homography can be treated as independent signals).
    std::vector<float> smoothPath(m_path.size());
    for (int c = 0; c < pc; ++c) {
        std::vector<double> channel(n);
        for (size_t t = 0; t < n; ++t)
            channel[t] = double(m_path[t * size_t(pc) + size_t(c)]);
        if (lock) {
            const double ident = m_mode == Mode::Perspective ? kIdentityHomography[c] : 0.0;
            std::fill(channel.begin(), channel.end(), ident);
        } else {
            const double sigma = smooth * kSmoothSeconds * m_fps.toDouble();
            channel = smoothed(channel, sigma);
        }
        for (size_t t = 0; t < n; ++t)
            smoothPath[t * size_t(pc) + size_t(c)] = float(channel[t]);
    }
    auto result = std::make_shared<Corrections>(m_path.size());
    for (size_t t = 0; t < n; ++t) {
        const Mat3 p = matrixOf(&m_path[t * size_t(pc)], 1.0);
        const Mat3 s = matrixOf(&smoothPath[t * size_t(pc)], 1.0);
        const Mat3 m = s * p.inverse();
        float* out = &(*result)[t * size_t(pc)];
        switch (m_mode) {
        case Mode::Translation:
            out[0] = float(m.m[2]);
            out[1] = float(m.m[5]);
            break;
        case Mode::Similarity:
            out[0] = float(m.m[2]);
            out[1] = float(m.m[5]);
            out[2] = float(std::atan2(m.m[3], m.m[0]));
            out[3] = float(std::log(std::max(1e-9, std::hypot(m.m[0], m.m[3]))));
            break;
        case Mode::Perspective: {
            const double w = std::abs(m.m[8]) > 1e-12 ? m.m[8] : 1.0;
            for (int i = 0; i < 8; ++i)
                out[i] = float(m.m[size_t(i)] / w);
            break;
        }
        }
    }
    const std::lock_guard<std::mutex> guard(m_mutex);
    m_haveCorrections = true;
    m_corrSmooth = smooth;
    m_corrLock = lock;
    m_corr = result;
    return result;
}

Mat3 Data::correction(const Settings& s, int64_t sourceFrame) const
{
    if (!covers(sourceFrame))
        return Mat3 {};
    const auto corr = corrections(s.smooth, s.cameraLock);
    return matrixOf(
        &(*corr)[size_t(sourceFrame - m_first) * size_t(paramCount(m_mode))], s.strength);
}

double Data::zoomFactor(const Settings& s) const
{
    if (!s.zoom || m_frames == 0 || s.strength == 0.0)
        return 1.0;
    double needed = 1.0;
    bool cached = false;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_haveZoom && m_zoomLock == s.cameraLock && m_zoomStrength == s.strength
            && (s.cameraLock || m_zoomSmooth == s.smooth)) {
            needed = m_zoomNeeded;
            cached = true;
        }
    }
    if (!cached) {
        const auto corr = corrections(s.smooth, s.cameraLock);
        const int pc = paramCount(m_mode);
        for (int64_t t = 0; t < m_frames; ++t) {
            const Mat3 m = matrixOf(&(*corr)[size_t(t) * size_t(pc)], s.strength);
            needed = std::max(needed, requiredZoom(m, m_aspect));
        }
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_haveZoom = true;
        m_zoomSmooth = s.smooth;
        m_zoomLock = s.cameraLock;
        m_zoomStrength = s.strength;
        m_zoomNeeded = needed;
    }
    return std::min(needed, 1.0 + s.croppingRatio);
}

// ---- ClipStabilization ----

bool operator==(const ClipStabilization& a, const ClipStabilization& b)
{
    if (!(a.settings == b.settings))
        return false;
    if (a.data == b.data)
        return true;
    return a.data && b.data && *a.data == *b.data;
}

bool isActive(const ClipStabilization& s)
{
    return s.settings.enabled && s.data && s.data->mode() == s.settings.mode
        && s.settings.strength != 0.0;
}

bool needsAnalysis(const ClipStabilization& s)
{
    return s.data && s.data->mode() != s.settings.mode;
}

int64_t sourceFrameAt(int64_t timeUs, Rational fps)
{
    if (!fps.isValid())
        return 0;
    return int64_t(std::floor(double(timeUs) * fps.num / (1e6 * fps.den) + kFrameTolerance));
}

QTransform transform(const ClipData& clip, int64_t localFrame, QSizeF frameSize, Rational fps)
{
    const ClipStabilization& st = clip.stabilization;
    if (!isActive(st) || !fps.isValid())
        return QTransform();
    const Data& data = *st.data;
    const Settings s = clamped(st.settings);
    const int64_t us = clip.sourceTimeUsAt(clip.start + localFrame, fps);
    const int64_t k = sourceFrameAt(us, data.fps());
    Mat3 m = data.correction(s, k);
    const double z = data.zoomFactor(s);
    if (z != 1.0) {
        Mat3 zoom;
        zoom.m = {z, 0, 0, 0, z, 0, 0, 0, 1};
        m = zoom * m;
    }
    // Normalised picture coordinates -> content pixels: the picture fills the frame at zoom 1.
    const double width = std::min(frameSize.width(), frameSize.height() / data.aspect());
    Mat3 toNorm, toPx;
    toNorm.m = {1.0 / width, 0, 0, 0, 1.0 / width, 0, 0, 0, 1};
    toPx.m = {width, 0, 0, 0, width, 0, 0, 0, 1};
    const Mat3 p = toPx * m * toNorm;
    // QTransform multiplies row vectors, so it holds the transpose.
    return QTransform(p.m[0], p.m[3], p.m[6], p.m[1], p.m[4], p.m[7], p.m[2], p.m[5], p.m[8]);
}

double zoomOf(const ClipData& clip)
{
    const ClipStabilization& st = clip.stabilization;
    return isActive(st) ? st.data->zoomFactor(clamped(st.settings)) : 1.0;
}

// ---- Project file ----

QJsonObject toJson(const ClipStabilization& st)
{
    const Settings& s = st.settings;
    QJsonObject o {{QStringLiteral("enabled"), s.enabled}, {QStringLiteral("mode"), modeId(s.mode)},
        {QStringLiteral("cameraLock"), s.cameraLock}, {QStringLiteral("zoom"), s.zoom},
        {QStringLiteral("croppingRatio"), s.croppingRatio}, {QStringLiteral("smooth"), s.smooth},
        {QStringLiteral("strength"), s.strength}};
    if (st.data) {
        const Data& d = *st.data;
        QByteArray bytes(qsizetype(d.path().size() * 4), Qt::Uninitialized);
        for (size_t i = 0; i < d.path().size(); ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, &d.path()[i], 4);
            for (int b = 0; b < 4; ++b) // little-endian on every host
                bytes[qsizetype(i * 4 + size_t(b))] = char((bits >> (8 * b)) & 0xFF);
        }
        o[QStringLiteral("data")] = QJsonObject {{QStringLiteral("mode"), modeId(d.mode())},
            {QStringLiteral("fps"), QJsonArray {d.fps().num, d.fps().den}},
            {QStringLiteral("first"), qint64(d.firstFrame())},
            {QStringLiteral("frames"), qint64(d.frames())}, {QStringLiteral("aspect"), d.aspect()},
            {QStringLiteral("path"), QString::fromLatin1(qCompress(bytes, 6).toBase64())}};
    }
    return o;
}

ClipStabilization fromJson(const QJsonObject& o)
{
    ClipStabilization st;
    Settings& s = st.settings;
    s.enabled = o.value(QLatin1String("enabled")).toBool(false);
    s.mode = modeFromId(o.value(QLatin1String("mode")).toString());
    s.cameraLock = o.value(QLatin1String("cameraLock")).toBool(false);
    s.zoom = o.value(QLatin1String("zoom")).toBool(false);
    s.croppingRatio = o.value(QLatin1String("croppingRatio")).toDouble(1.0);
    s.smooth = o.value(QLatin1String("smooth")).toDouble(0.25);
    s.strength = o.value(QLatin1String("strength")).toDouble(1.0);
    s = clamped(s);
    if (o.contains(QLatin1String("data"))) {
        const QJsonObject d = o.value(QLatin1String("data")).toObject();
        const Mode mode = modeFromId(d.value(QLatin1String("mode")).toString());
        const QJsonArray fpsArray = d.value(QLatin1String("fps")).toArray();
        const Rational fps {fpsArray.at(0).toInt(25), fpsArray.at(1).toInt(1)};
        const int64_t frames = d.value(QLatin1String("frames")).toInteger();
        const int64_t first = d.value(QLatin1String("first")).toInteger();
        const double aspect = d.value(QLatin1String("aspect")).toDouble(0.0);
        const QByteArray bytes = qUncompress(
            QByteArray::fromBase64(d.value(QLatin1String("path")).toString().toLatin1()));
        // The file is untrusted: every number is range-checked before it is multiplied or used.
        const int64_t perFrame = paramCount(mode);
        const bool sane = fpsArray.size() == 2 && fps.isValid() && fps.num <= 100000 && fps.den <= 100000 && frames > 0
            && frames <= kMaxFrames && int64_t(bytes.size()) / 4 / perFrame == frames
            && int64_t(bytes.size()) == frames * perFrame * 4 && first >= -kMaxFrames
            && first <= (int64_t(1) << 40) && std::isfinite(aspect) && aspect >= 0.05
            && aspect <= 20.0;
        if (sane) {
            const size_t count = size_t(frames * perFrame);
            std::vector<float> path(count);
            bool finite = true;
            for (size_t i = 0; i < count && finite; ++i) {
                uint32_t bits = 0;
                for (int b = 0; b < 4; ++b)
                    bits |= uint32_t(uchar(bytes[qsizetype(i * 4 + size_t(b))])) << (8 * b);
                std::memcpy(&path[i], &bits, 4);
                finite = std::isfinite(path[i]);
            }
            if (finite)
                st.data = std::make_shared<const Data>(mode, fps, first, aspect, std::move(path));
        }
    }
    return st;
}

} // namespace kk::stabilize
