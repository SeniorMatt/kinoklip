// SPDX-License-Identifier: GPL-3.0-or-later
// CpuComposition implementation: QPainter geometry plus per-pixel loops for the blend,
// mix and colour nodes (formulas of render/CompMath.h, as on the GPU).
#include "render/CpuComposition.h"

#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/EffectMath.h"
#include "render/Generators.h"
#include "render/RenderLog.h"
#include "render/TitleRenderer.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

namespace k = nodekeys;

QImage blank(QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

/// Image of the same size in the premultiplied format (null stays null).
QImage normalized(const QImage& image, QSize size)
{
    if (image.isNull())
        return {};
    QImage out = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (out.size() != size)
        out = out.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return out;
}

inline void unpack(QRgb p, float c[4])
{
    c[0] = qRed(p) / 255.0f;
    c[1] = qGreen(p) / 255.0f;
    c[2] = qBlue(p) / 255.0f;
    c[3] = qAlpha(p) / 255.0f;
}

inline QRgb pack(const float c[4])
{
    const auto q = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    const int a = q(c[3]);
    return qRgba(std::min(q(c[0]), a), std::min(q(c[1]), a), std::min(q(c[2]), a), a);
}

/// mix(a, b, amount · mask alpha) per pixel (null a / b = transparent, null mask = 1).
QImage mixImages(const QImage& a, const QImage& b, const QImage& mask, float amount, QSize size)
{
    QImage out = blank(size);
    for (int y = 0; y < size.height(); ++y) {
        const QRgb* pa = a.isNull() ? nullptr : reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* pb = b.isNull() ? nullptr : reinterpret_cast<const QRgb*>(b.constScanLine(y));
        const QRgb* pm
            = mask.isNull() ? nullptr : reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        QRgb* po = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            float ca[4] = {0, 0, 0, 0}, cb[4] = {0, 0, 0, 0}, o[4];
            if (pa)
                unpack(pa[x], ca);
            if (pb)
                unpack(pb[x], cb);
            const float t = amount * (pm ? qAlpha(pm[x]) / 255.0f : 1.0f);
            for (int i = 0; i < 4; ++i)
                o[i] = ca[i] + (cb[i] - ca[i]) * t;
            po[x] = pack(o);
        }
    }
    return out;
}

/// Applies @p fn(rgba straight, in place) to every pixel of @p in.
template<typename Fn>
QImage pointwise(const QImage& in, Fn&& fn)
{
    QImage out = in.copy();
    for (int y = 0; y < out.height(); ++y) {
        QRgb* p = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            float c[4];
            unpack(p[x], c);
            if (c[3] > 0) {
                for (int i = 0; i < 3; ++i)
                    c[i] /= c[3];
            }
            fn(c);
            for (int i = 0; i < 3; ++i)
                c[i] = std::clamp(c[i], 0.0f, 1.0f) * c[3];
            p[x] = pack(c);
        }
    }
    return out;
}

/// Timeline px -> image px transform conjugation: an image-pixel QPainter transform for a
/// timeline-pixel matrix @p m.
QTransform inImagePixels(const QTransform& m, double scale)
{
    return QTransform::fromScale(1.0 / scale, 1.0 / scale) * m
        * QTransform::fromScale(scale, scale);
}

/// @p in moved by (dx, dy) image pixels with bilinear sampling (premultiplied ARGB32, transparent
/// outside). QPainter rounds a pure translation to whole pixels even with smooth
/// transformation on, which would make a sub-pixel Match Move jump.
QImage shifted(const QImage& in, double dx, double dy)
{
    QImage out(in.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const int w = in.width(), h = in.height();
    for (int y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        const double sy = y - dy;
        const int y0 = int(std::floor(sy));
        const double fy = sy - y0;
        for (int x = 0; x < w; ++x) {
            const double sx = x - dx;
            const int x0 = int(std::floor(sx));
            const double fx = sx - x0;
            double acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int xx = x0 + i, yy = y0 + j;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                        continue;
                    const double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    const QRgb c = reinterpret_cast<const QRgb*>(in.constScanLine(yy))[xx];
                    acc[0] += wgt * qRed(c);
                    acc[1] += wgt * qGreen(c);
                    acc[2] += wgt * qBlue(c);
                    acc[3] += wgt * qAlpha(c);
                }
            }
            dst[x] = qRgba(int(std::lround(acc[0])), int(std::lround(acc[1])),
                int(std::lround(acc[2])), int(std::lround(acc[3])));
        }
    }
    return out;
}

class Evaluator
{
public:
    explicit Evaluator(const CpuCompEval& ev)
        : m_ev(ev)
    { }

    QImage eval(quint64 id, int depth)
    {
        const auto memo = m_done.constFind(id);
        if (memo != m_done.constEnd())
            return *memo;
        const CompNode* n = m_ev.comp->node(id);
        if (!n || depth > m_ev.comp->nodes.size())
            return {};
        QImage image = evalNode(*n, depth);
        m_done.insert(id, image);
        return image;
    }

private:
    QImage input(const CompNode& n, const QString& port, int depth)
    {
        const auto it = n.inputs.constFind(port);
        if (it == n.inputs.constEnd() || !it->isConnected() || it->outputIndex != 0)
            return {};
        return normalized(eval(it->nodeId, depth + 1), m_ev.size);
    }

    void warn(const QString& type)
    {
        if (m_ev.warned && !m_ev.warned->contains(type)) {
            m_ev.warned->insert(type);
            qCWarning(lcRender) << "CPU fallback: composition node" << type << "passes through";
        }
    }

    QImage evalNode(const CompNode& n, int depth)
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(n.type);
        const QString main = d ? d->mainInput : QString();
        if (!d || !n.enabled || n.type == nodetypes::MediaOut)
            return main.isEmpty() ? QImage() : input(n, main, depth);
        const int64_t f = m_ev.compFrame;
        const QSize size = m_ev.size;
        const double s = m_ev.scale;
        const QImage mask = d->input(nodeports::Mask) ? input(n, nodeports::Mask, depth) : QImage();
        const QImage in = main.isEmpty() ? QImage() : input(n, main, depth);
        QImage out;
        bool maskDone = false;
        if (n.type == nodetypes::MediaIn) {
            out = m_ev.mediaIn ? normalized(m_ev.mediaIn(n, f), size) : QImage();
            if (out.isNull())
                out = blank(size);
        } else if (n.type == nodetypes::Merge) {
            out = merge(n, in, input(n, nodeports::Foreground, depth), mask);
            maskDone = true;
        } else if (n.type == nodetypes::Dissolve) {
            out = mixImages(in, input(n, nodeports::Foreground, depth), mask,
                float(std::clamp(n.params.value(k::Mix, f, 0.0), 0.0, 1.0)), size);
            maskDone = true;
        } else if ((n.type == nodetypes::Transform || n.type == nodetypes::Tracker
                       || n.type == nodetypes::PlanarTracker)
            && !in.isNull()) {
            out = blank(size);
            const QTransform m = inImagePixels(n.type == nodetypes::PlanarTracker
                    ? compmath::planarMatrix(n.params, f, m_ev.frame)
                    : n.type == nodetypes::Tracker
                    ? compmath::trackerMatrix(n.params, f, m_ev.frame)
                    : compmath::transformMatrix(n.params, f, m_ev.frame),
                s);
            if (m.type() == QTransform::TxTranslate
                && (std::abs(m.dx() - std::round(m.dx())) > 1e-3
                    || std::abs(m.dy() - std::round(m.dy())) > 1e-3)) {
                out = shifted(in, m.dx(), m.dy());
            } else {
                QPainter p(&out);
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.setRenderHint(QPainter::Antialiasing);
                p.setTransform(m);
                p.drawImage(0, 0, in);
            }
        } else if (n.type == nodetypes::Crop && !in.isNull()) {
            out = blank(size);
            const QRectF r = compmath::cropRect(n.params, f, m_ev.frame);
            const QRectF px(r.x() * s, r.y() * s, r.width() * s, r.height() * s);
            QPainter p(&out);
            p.setClipRect(px);
            p.drawImage(0, 0, in);
        } else if (n.type == nodetypes::Resize && !in.isNull()) {
            out = blank(size);
            const QRectF r = compmath::resizeRect(n.params, f, m_ev.frame);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRectF(r.x() * s, r.y() * s, r.width() * s, r.height() * s), in);
        } else if (n.type == nodetypes::Letterbox && !in.isNull()) {
            out = in.copy();
            const QRectF keep = compmath::letterboxRect(n.params, f, m_ev.frame);
            const QRectF px(keep.x() * s, keep.y() * s, keep.width() * s, keep.height() * s);
            QPainterPath bars;
            bars.addRect(QRectF(QPointF(0, 0), size));
            QPainterPath kept;
            kept.addRect(px);
            QPainter p(&out);
            const QVariant c = n.params.staticValue(k::Color, QColor(Qt::black));
            p.fillPath(
                bars.subtracted(kept), c.value<QColor>().isValid() ? c.value<QColor>() : Qt::black);
        } else if (n.type == nodetypes::Background) {
            out = renderGeneratorImage(
                compmath::backgroundFrame(n.params, f, m_ev.frame), size, m_ev.frame);
        } else if (n.type == nodetypes::Text) {
            out = blank(size);
            QPointF offset;
            const TitleStyle style = compmath::textStyle(n.params, f, size, s, m_ev.frame, &offset);
            if (!style.text.isEmpty()) {
                const TitleImage t = renderTitle(style);
                if (!t.image.isNull()) {
                    QPainter p(&out);
                    p.setRenderHint(QPainter::SmoothPixmapTransform);
                    p.drawImage(t.rect.translated(offset), t.image);
                }
            }
        } else if (n.type == nodetypes::FastNoise) {
            out = noise(n);
        } else if (d->category == NodeCategory::Mask) {
            out = maskImage(n, in);
            maskDone = true;
        } else if (n.type == nodetypes::ColorCorrector && !in.isNull()) {
            const compmath::CorrectorParams c = compmath::correctorParams(n.params, f);
            if (!c.isIdentity())
                out = pointwise(in, [&c](float* px) { compmath::applyCorrector(c, px); });
        } else if (n.type == nodetypes::ChannelBooleans && !in.isNull()) {
            const std::array<int, 4> src = compmath::channelSources(n.params);
            out = pointwise(in, [&src](float* px) {
                // Channel sources read the alpha of the input; the result is re-premultiplied.
                compmath::applyChannels(src, px);
            });
        } else if (!d->effectId.isEmpty() && !in.isNull()) {
            if (fxmath::isPointEffect(d->effectId)) {
                out = in.copy();
                fxmath::applyPointEffect(d->effectId, n.params, f, out, m_ev.fps);
            } else {
                warn(n.type);
            }
        }
        if (out.isNull())
            return in; // identity or not available: the main input passes through
        if (!mask.isNull() && !maskDone)
            out = mixImages(in, out, mask, 1.0f, size);
        return out;
    }

    QImage merge(const CompNode& n, const QImage& bg, const QImage& fg, const QImage& mask)
    {
        if (fg.isNull())
            return bg;
        const QSize size = m_ev.size;
        const int64_t f = m_ev.compFrame;
        QImage moved = blank(size);
        {
            QPainter p(&moved);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setRenderHint(QPainter::Antialiasing);
            p.setTransform(inImagePixels(
                compmath::mergeForegroundMatrix(n.params, f, m_ev.frame), m_ev.scale));
            p.drawImage(0, 0, fg);
        }
        const int mode = blendmodes::index(n.params.staticValue(k::BlendMode).toString());
        const float opacity = float(std::clamp(n.params.value(k::Opacity, f, 1.0), 0.0, 1.0));
        QImage out = blank(size);
        for (int y = 0; y < size.height(); ++y) {
            const QRgb* pb
                = bg.isNull() ? nullptr : reinterpret_cast<const QRgb*>(bg.constScanLine(y));
            const QRgb* pf = reinterpret_cast<const QRgb*>(moved.constScanLine(y));
            const QRgb* pm
                = mask.isNull() ? nullptr : reinterpret_cast<const QRgb*>(mask.constScanLine(y));
            QRgb* po = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                float b[4] = {0, 0, 0, 0}, s[4], o[4];
                if (pb)
                    unpack(pb[x], b);
                unpack(pf[x], s);
                for (float& v : s)
                    v *= opacity;
                compmath::blendPremultiplied(mode, b, s, o);
                if (pm) {
                    const float t = qAlpha(pm[x]) / 255.0f;
                    for (int i = 0; i < 4; ++i)
                        o[i] = b[i] + (o[i] - b[i]) * t;
                }
                po[x] = pack(o);
            }
        }
        return out;
    }

    QImage noise(const CompNode& n)
    {
        const compmath::NoiseParams np
            = compmath::noiseParams(n.params, m_ev.compFrame, m_ev.frame, m_ev.fps);
        const QSize size = m_ev.size;
        QImage out(size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < size.height(); ++y) {
            QRgb* po = reinterpret_cast<QRgb*>(out.scanLine(y));
            const double ty = (y + 0.5) / m_ev.scale;
            for (int x = 0; x < size.width(); ++x) {
                const float v = compmath::noiseAt(np, (x + 0.5) / m_ev.scale, ty, m_ev.frame);
                float c[4];
                for (int i = 0; i < 4; ++i)
                    c[i] = np.color1[size_t(i)] + (np.color2[size_t(i)] - np.color1[size_t(i)]) * v;
                po[x] = pack(c);
            }
        }
        return out;
    }

    /// Mask nodes: the shape filled white (hard edge: the soft edge is GPU only), inverted /
    /// faded by Level, united with the mask input.
    QImage maskImage(const CompNode& n, const QImage& in)
    {
        const QSize size = m_ev.size;
        const MaskData m = compmath::maskOf(n, m_ev.compFrame, m_ev.frame);
        QImage out = blank(size);
        if (m.isRenderable()) {
            QPainter p(&out);
            p.setRenderHint(QPainter::Antialiasing);
            p.setTransform(
                QTransform::fromTranslate(m_ev.frame.width() / 2, m_ev.frame.height() / 2)
                * QTransform::fromScale(m_ev.scale, m_ev.scale));
            QPainterPath shape = maskOutline(m, m_ev.compFrame);
            if (m.invert) {
                QPainterPath all;
                all.addRect(QRectF(-m_ev.frame.width() / 2, -m_ev.frame.height() / 2,
                    m_ev.frame.width(), m_ev.frame.height()));
                shape = all.subtracted(shape);
            }
            p.setOpacity(std::clamp(m.value(maskkeys::Opacity, 0) / 100.0, 0.0, 1.0));
            p.fillPath(shape, Qt::white);
        }
        if (!in.isNull()) {
            QPainter p(&out);
            p.drawImage(0, 0, in); // union: "over" of white mattes
        }
        return out;
    }

    const CpuCompEval& m_ev;
    QHash<quint64, QImage> m_done;
};

} // namespace

QImage evaluateCompositionCpu(const CpuCompEval& ev, quint64 nodeId)
{
    if (!ev.comp || ev.size.isEmpty())
        return blank(ev.size.expandedTo(QSize(1, 1)));
    Evaluator e(ev);
    QImage out = normalized(e.eval(nodeId, 0), ev.size);
    return out.isNull() ? blank(ev.size) : out;
}

void blendLayerInto(QImage& background, const QImage& layer, int mode)
{
    if (layer.isNull() || layer.size() != background.size())
        return;
    for (int y = 0; y < background.height(); ++y) {
        QRgb* pb = reinterpret_cast<QRgb*>(background.scanLine(y));
        const QRgb* pl = reinterpret_cast<const QRgb*>(layer.constScanLine(y));
        for (int x = 0; x < background.width(); ++x) {
            if (qAlpha(pl[x]) == 0)
                continue; // nothing over this pixel
            float b[4], s[4], o[4];
            unpack(pb[x], b);
            unpack(pl[x], s);
            compmath::blendPremultiplied(mode, b, s, o);
            pb[x] = pack(o);
        }
    }
}

} // namespace kk::render
