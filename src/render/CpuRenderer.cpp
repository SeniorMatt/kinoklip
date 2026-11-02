// SPDX-License-Identifier: GPL-3.0-or-later
// CpuRenderer — QPainter fallback backend for machines without OpenGL 3.3 core.
//
// Same timeline semantics as the GPU path (TimelineEval): transform, opacity, crop, titles,
// solids, display rotation/SAR (VideoDecoder::frameAt applies them) and transitions — the
// latter all rendered as a cross dissolve. Effects are skipped (one warning per effect id);
// masks are applied as hard-edged clip paths (maskMatte()).
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "render/DecoderPool.h"
#include "render/RenderBackend.h"
#include "render/RenderLog.h"
#include "render/TimelineEval.h"
#include "render/TitleRenderer.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

namespace kk::render {

namespace {

/// Blend weights closer than this to a source frame show that frame alone.
constexpr double kMinBlend = 1.0 / 128.0;

class CpuRenderer final : public RenderBackend
{
public:
    bool initialize(QString*) override { return true; }
    QString description() const override { return QStringLiteral("CPU (QPainter fallback)"); }
    bool isGpu() const override { return false; }

    void render(RenderJob& job) override
    {
        QElapsedTimer timer;
        timer.start();
        auto frame = std::make_shared<RenderedFrame>();
        const RenderRequest& rq = job.request;
        frame->frame = rq.frame;
        if (!rq.timeline || rq.timeline->width <= 0 || rq.timeline->height <= 0) {
            job.promise.set_value(std::move(frame));
            return;
        }
        const TimelineData& tl = *rq.timeline;
        const QSize requested = rq.outSize.isEmpty() ? QSize(tl.width, tl.height) : rq.outSize;
        const QSize out = scaledRenderSize(requested, rq.quality);
        m_media = job.media.get();
        m_nested = &tl.nested;
        m_depth = 0;
        m_decoders.beginFrame();
        QImage image = renderTimeline(tl, rq.frame, out);
        m_decoders.collectIdle();
        frame->size = out;
        frame->ok = true;
        frame->image = std::move(image);
        frame->renderMs = double(timer.nsecsElapsed()) / 1e6;
        job.promise.set_value(std::move(frame));
    }

private:
    /// Deepest nesting rendered (guards against damaged data only).
    static constexpr int kMaxNestingDepth = 16;

    /// Paints every video track of @p tl at @p frame into a new image of @p out pixels.
    /// Re-entrant for nested timelines (the per-timeline members are saved and restored).
    QImage renderTimeline(const TimelineData& tl, int64_t frame, QSize out)
    {
        const TimelineData* savedTimeline = m_timeline;
        const int64_t savedFrame = m_frame;
        const QSize savedOut = m_out;
        const QTransform savedScale = m_scale;
        m_timeline = &tl;
        m_frame = frame;
        m_out = out;
        m_scale = QTransform::fromScale(
            double(out.width()) / tl.width, double(out.height()) / tl.height);
        QImage image(out, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            for (const TrackData& track : tl.videoTracks) {
                if (!track.enabled)
                    continue;
                const TrackFrame tf = evaluateTrack(track, frame);
                if (tf.single)
                    drawClip(painter, *tf.single, 1.0);
                else if (tf.transition)
                    drawTransition(painter, *tf.transition);
            }
        }
        m_timeline = savedTimeline;
        m_frame = savedFrame;
        m_out = savedOut;
        m_scale = savedScale;
        return image;
    }

    /// Dissolve approximation of every transition: temp = A·(1 − t) + B·t (CompositionMode_Plus
    /// adds premultiplied values), then temp over the output.
    void drawTransition(QPainter& painter, const TransitionSpan& span)
    {
        const double t = span.progress(m_frame);
        QImage temp(m_out, QImage::Format_ARGB32_Premultiplied);
        temp.fill(Qt::transparent);
        {
            QPainter p(&temp);
            if (span.outgoing)
                drawClip(p, *span.outgoing, 1.0 - t);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            if (span.incoming)
                drawClipPlus(p, *span.incoming, t);
        }
        painter.drawImage(0, 0, temp);
    }

    /// drawClip() for the B side of a dissolve: rendered into its own layer first so the
    /// Plus mode adds the clip's composite, not each primitive separately.
    void drawClipPlus(QPainter& painter, const ClipData& clip, double weight)
    {
        QImage layer(m_out, QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        {
            QPainter p(&layer);
            drawClip(p, clip, weight);
        }
        painter.drawImage(0, 0, layer);
    }

    void drawClip(QPainter& painter, const ClipData& clip, double weight)
    {
        if (!clip.enabled)
            return;
        const int64_t local = clip.localFrame(m_frame);
        const double opacity
            = std::clamp(clip.transform.value(keys::Opacity, local) / 100.0, 0.0, 1.0) * weight;
        if (opacity <= 0.0)
            return;
        for (const EffectInstance& e : clip.effects) {
            if (e.enabled && !m_warned.contains(e.type)) {
                m_warned.insert(e.type);
                qCWarning(lcRender) << "CPU fallback: effect" << e.type << "skipped";
            }
        }
        const QSizeF frameSize(m_timeline->width, m_timeline->height);
        const double outScale = std::max(m_scale.m11(), m_scale.m22());
        const QTransform toOutput = clipTransform(clip, local, frameSize) * m_scale;

        QSizeF content = frameSize;
        QSizeF sourcePx = frameSize;
        QImage picture;
        QRectF pictureRect; // in content coordinates (centred)
        QColor solid;
        switch (clip.kind) {
        case ClipKind::Media: {
            const QString path = m_media ? m_media->value(clip.mediaId) : QString();
            if (path.isEmpty())
                return;
            const int64_t timeUs = clampedSourceTimeUs(clip, m_frame, m_timeline->fps);
            DecoderPool::Entry* entry = m_decoders.acquire(path, timeUs);
            if (!entry)
                return;
            const media::MediaInfo& info = entry->decoder->info();
            sourcePx = QSizeF(info.displayWidth, info.displayHeight);
            content = fitSize(sourcePx, frameSize);
            // Decode at the size the picture occupies in the output (never above native).
            const double zoom = std::max(1.0, maxZoom(clip, local));
            const QSize want = (content * outScale * zoom).toSize().boundedTo(sourcePx.toSize());
            picture = entry->decoder->frameAt(timeUs, want.expandedTo(QSize(1, 1)));
            // Frame Blend / Optical Flow (no flow on the CPU: blended): mix in the next source
            // frame by where the source time falls between the two.
            if (retime::effectiveProcess(clip, *m_timeline) != RetimeProcess::Nearest
                && !picture.isNull() && info.frameRate.isValid()) {
                const int64_t frameUs = int64_t(1e6 / info.frameRate.toDouble());
                const int64_t ptsUs = entry->decoder->lastFrameTimeUs();
                const double w = frameUs > 0 ? double(timeUs - ptsUs) / double(frameUs) : 0.0;
                DecoderPool::Entry* next = w > kMinBlend && w < 1.0 - kMinBlend
                    ? m_decoders.acquire(path, ptsUs + frameUs)
                    : nullptr;
                const QImage b
                    = next ? next->decoder->frameAt(ptsUs + frameUs, picture.size()) : QImage();
                if (!b.isNull() && next->decoder->lastFrameTimeUs() > ptsUs) {
                    QPainter blend(&picture);
                    blend.setOpacity(w);
                    blend.drawImage(QRect(QPoint(0, 0), picture.size()), b);
                }
            }
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        case ClipKind::Title: {
            const TitleStyle style = resolveTitleStyle(
                clip, local, (frameSize * outScale).toSize().expandedTo(QSize(1, 1)), outScale);
            const TitleImage title = renderTitle(style);
            picture = title.image;
            pictureRect = QRectF(title.rect.x() / outScale - frameSize.width() / 2,
                title.rect.y() / outScale - frameSize.height() / 2, title.rect.width() / outScale,
                title.rect.height() / outScale);
            break;
        }
        case ClipKind::Nested: {
            const TimelineData* n = m_nested ? m_nested->findClip(clip) : nullptr;
            if (!n || n->width <= 0 || n->height <= 0 || m_depth >= kMaxNestingDepth)
                return;
            sourcePx = QSizeF(n->width, n->height);
            content = fitSize(sourcePx, frameSize);
            const double zoom = std::clamp(maxZoom(clip, local), 1.0, 2.0);
            const QSize want = (content * outScale * zoom).toSize().expandedTo(QSize(1, 1));
            ++m_depth;
            picture
                = renderTimeline(*n, nestedFrameAt(clip, m_frame, m_timeline->fps, n->fps), want);
            --m_depth;
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        case ClipKind::Solid:
            solid = clip.solidColor.isValid() ? clip.solidColor : QColor(0x30, 0x30, 0x34);
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        if (picture.isNull() && !solid.isValid())
            return;

        // Crop in source pixels → content coordinates.
        const ParamSet& p = clip.transform;
        const double sx = content.width() / sourcePx.width();
        const double sy = content.height() / sourcePx.height();
        const QRectF cropRect(QPointF(-content.width() / 2 + p.value(keys::CropL, local) * sx,
                                  -content.height() / 2 + p.value(keys::CropT, local) * sy),
            QPointF(content.width() / 2 - p.value(keys::CropR, local) * sx,
                content.height() / 2 - p.value(keys::CropB, local) * sy));
        if (cropRect.width() <= 0 || cropRect.height() <= 0)
            return;

        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(toOutput);
        painter.setOpacity(opacity);
        painter.setClipRect(cropRect);
        if (const auto matte = maskMatte(clip, local, content)) {
            if (matte->isEmpty()) {
                painter.restore();
                return;
            }
            painter.setClipPath(*matte, Qt::IntersectClip);
        }
        if (solid.isValid())
            painter.fillRect(pictureRect, solid);
        else
            painter.drawImage(pictureRect, picture);
        painter.restore();
    }

    /// Hard-edged approximation of the GPU mask matte as a clip path in content coordinates
    /// (modes and invert exact; feather, expansion and partial opacity are ignored: a mask with
    /// opacity 0 is skipped). std::nullopt when the clip has no renderable mask.
    static std::optional<QPainterPath> maskMatte(
        const ClipData& clip, int64_t local, QSizeF content)
    {
        std::optional<QPainterPath> matte;
        QPainterPath everything;
        // Generous bounds: masks may reach beyond the content (the crop clips anyway).
        everything.addRect(QRectF(-content.width() * 4, -content.height() * 4, content.width() * 8,
            content.height() * 8));
        for (const MaskData& m : clip.masks) {
            if (!m.isRenderable())
                continue;
            if (!matte) {
                const bool full = m.mode == MaskMode::Subtract || m.mode == MaskMode::Intersect;
                matte = full ? everything : QPainterPath();
            }
            if (m.value(maskkeys::Opacity, local) <= 0.0)
                continue;
            QPainterPath shape = maskOutline(m, local);
            shape.setFillRule(Qt::OddEvenFill);
            if (m.invert)
                shape = everything.subtracted(shape);
            switch (m.mode) {
            case MaskMode::Add:
                *matte = matte->united(shape);
                break;
            case MaskMode::Subtract:
                *matte = matte->subtracted(shape);
                break;
            case MaskMode::Intersect:
                *matte = matte->intersected(shape);
                break;
            case MaskMode::Difference:
                *matte = matte->united(shape).subtracted(matte->intersected(shape));
                break;
            }
        }
        return matte;
    }

    DecoderPool m_decoders;
    QSet<QString> m_warned;
    const TimelineData* m_timeline = nullptr;
    const MediaPathMap* m_media = nullptr;
    const NestedTimelines* m_nested = nullptr;
    int m_depth = 0;
    int64_t m_frame = 0;
    QSize m_out;
    QTransform m_scale;
};

} // namespace

std::unique_ptr<RenderBackend> createCpuBackend()
{
    return std::make_unique<CpuRenderer>();
}

} // namespace kk::render
