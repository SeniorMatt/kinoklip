// SPDX-License-Identifier: GPL-3.0-or-later
// CpuRenderer — QPainter fallback backend for machines without OpenGL 3.3 core.
//
// Same timeline semantics as the GPU path (TimelineEval): transform, opacity, crop, titles,
// solids, display rotation/SAR (VideoDecoder::frameAt applies them) and transitions — the
// latter all rendered as a cross dissolve. The pointwise effects (keyers without shrink / grow
// and edge blur, colour effects; render/EffectMath.h) are applied to the clip's picture, the
// others are skipped (one warning per effect id); masks are applied as hard-edged clip paths
// (maskMatte()).
#include "core/CompOps.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "render/CaptionRenderer.h"
#include "render/CpuComposition.h"
#include "render/DecoderPool.h"
#include "render/EffectMath.h"
#include "render/Generators.h"
#include "render/GradeEval.h"
#include "render/RenderBackend.h"
#include "render/RenderLog.h"
#include "render/TimelineEval.h"
#include "render/TitleRenderer.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QThreadPool>

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
    DecoderPool& decoders() override { return m_decoders; }

    CpuRenderer()
    {
        m_prepareThreads.setMaxThreadCount(DecoderPool::kMaxPrepareRunning);
        m_prepareThreads.setObjectName(QStringLiteral("kk.render.prepare"));
        m_decoders.setPrepareThreads(&m_prepareThreads);
    }

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
        m_proxies = rq.proxies.get();
        m_nested = &tl.nested;
        m_depth = 0;
        m_grades = !rq.bypassGrades;
        m_dynamicZoom = rq.dynamicZoom;
        m_decoders.beginFrame();
        m_decoders.setCatchUp(rq.catchUp);
        QImage image;
        if (rq.compositionNode) {
            image = compositionNodeImage(tl, *rq.compositionNode, out);
        } else {
            image = renderTimeline(tl, rq.frame, out);
            if (rq.subtitles && !tl.subtitleTracks.empty())
                drawSubtitles(image, tl, rq.frame);
        }
        m_decoders.collectIdle();
        frame->size = out;
        frame->ok = true;
        frame->fromCache = job.fromCache;
        frame->image = std::move(image);
        frame->renderMs = double(timer.nsecsElapsed()) / 1e6;
        job.promise.set_value(std::move(frame));
    }

private:
    /// Deepest nesting rendered (guards against damaged data only).
    static constexpr int kMaxNestingDepth = 16;

    /// Compositing page viewer request: one node of one clip's composition (transparent if
    /// the clip, its composition or the node is missing).
    QImage compositionNodeImage(const TimelineData& tl, const CompositionNodeView& view, QSize out)
    {
        const ClipData* clip = nullptr;
        for (const TrackData& track : tl.videoTracks) {
            const int i = track.indexOfClip(view.clipId);
            if (i >= 0)
                clip = &track.clips[size_t(i)];
        }
        QImage image(out, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        if (!clip)
            return image;
        // No stored graph: the default MediaIn -> MediaOut (the clip's own picture).
        std::optional<ClipData> defaultOwner;
        if (!clip->composition) {
            defaultOwner = *clip;
            defaultOwner->composition = compops::defaultComposition();
            clip = &*defaultOwner;
        }
        m_timeline = &tl;
        m_frame = clip->start + view.compFrame;
        m_out = out;
        m_scale = QTransform::fromScale(
            double(out.width()) / tl.width, double(out.height()) / tl.height);
        const double outScale = std::max(m_scale.m11(), m_scale.m22());
        QSizeF content, sourcePx;
        QImage picture;
        QRectF pictureRect;
        QColor solid;
        if (!clipPicture(
                *clip, view.compFrame, outScale, content, sourcePx, picture, pictureRect, solid))
            pictureRect = QRectF();
        image = compositionImage(
            *clip, view.compFrame, view.nodeId, out, picture, pictureRect, solid);
        m_timeline = nullptr;
        return image;
    }

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
        const QImage savedBelow = m_below;
        {
            QPainter painter(&image);
            for (const TrackData& track : tl.videoTracks) {
                if (!track.enabled)
                    continue;
                const TrackFrame tf = evaluateTrack(track, frame);
                if (tf.isEmpty())
                    continue;
                if (tf.single && tf.single->kind == ClipKind::Adjustment
                    && hasIdentityFootprint(*tf.single, tf.single->localFrame(frame), m_dynamicZoom)
                    && !isGraded(*tf.single, tf.single->localFrame(frame))
                    && std::none_of(tf.single->effects.cbegin(), tf.single->effects.cend(),
                        [](const EffectInstance& e) {
                            return e.enabled && fxmath::isPointEffect(e.type);
                        }))
                    continue; // only non-pointwise effects (skipped here): it changes nothing
                if (showsAdjustment(tf)) {
                    // The tracks below become the adjustment clip's picture (see GlRenderer).
                    painter.end();
                    m_below = image.copy();
                    painter.begin(&image);
                }
                if (const int mode = compositeModeIndex(tf, frame);
                    mode != 0 && !showsAdjustment(tf)) {
                    // Composite mode (spec 24.19): the track into a layer of its own, then
                    // the Merge node's blend with everything below (as GlRenderer does).
                    QImage layer(out, QImage::Format_ARGB32_Premultiplied);
                    layer.fill(Qt::transparent);
                    {
                        QPainter lp(&layer);
                        if (tf.single)
                            drawClip(lp, *tf.single, 1.0);
                        else if (tf.transition)
                            drawTransition(lp, *tf.transition);
                    }
                    painter.end();
                    blendLayerInto(image, layer, mode);
                    painter.begin(&image);
                    continue;
                }
                if (tf.single && tf.single->kind == ClipKind::Adjustment) {
                    // Replace the footprint with the processed picture (see GlRenderer).
                    drawClip(painter, *tf.single, 1.0, Blend::Erase);
                    drawClip(painter, *tf.single, 1.0, Blend::Add);
                } else if (tf.single)
                    drawClip(painter, *tf.single, 1.0);
                else if (tf.transition)
                    drawTransition(painter, *tf.transition);
            }
        }
        // The timeline grade (Color page): over the composite, before the captions.
        if (m_grades && !tl.grade.isEmpty() && !gradeops::isIdentity(tl.grade, frame))
            applyGrade(tl.grade, frame, image);
        m_below = savedBelow;
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

    /// Over: normal clips. Erase / Add: the two passes of an adjustment clip (footprint ×
    /// opacity removed from the target, then the processed picture added).
    enum class Blend {
        Over,
        Erase,
        Add
    };

    void drawClip(QPainter& painter, const ClipData& clip, double weight, Blend blend = Blend::Over)
    {
        if (!clip.enabled)
            return;
        const int64_t local = clip.localFrame(m_frame);
        const double opacity = clipOpacity(clip, local) * weight;
        if (opacity <= 0.0)
            return;
        for (const EffectInstance& e : clip.effects) {
            if (e.enabled && !fxmath::isPointEffect(e.type) && !m_warned.contains(e.type)) {
                m_warned.insert(e.type);
                qCWarning(lcRender) << "CPU fallback: effect" << e.type << "skipped";
            }
        }
        const QSizeF frameSize(m_timeline->width, m_timeline->height);
        const double outScale = std::max(m_scale.m11(), m_scale.m22());
        const QTransform toOutput = clipTransform(clip, local, frameSize, m_dynamicZoom, m_timeline->fps) * m_scale;

        QSizeF content = frameSize;
        QSizeF sourcePx = frameSize;
        QImage picture;
        QRectF pictureRect; // in content coordinates (centred)
        QColor solid;
        if (!clipPicture(clip, local, outScale, content, sourcePx, picture, pictureRect, solid))
            return;
        if (clip.composition && !compops::isPassThrough(*clip.composition)) {
            // The composition's output replaces the picture (frame-sized, timeline px crop).
            const double zoom = std::clamp(maxZoom(clip, local, m_dynamicZoom), 1.0, 2.0);
            const QSize canvas = (frameSize * outScale * zoom).toSize().expandedTo(QSize(1, 1));
            picture = compositionImage(clip, local, 0, canvas, picture, pictureRect, solid);
            content = sourcePx = frameSize;
            pictureRect = QRectF(-frameSize.width() / 2, -frameSize.height() / 2, frameSize.width(),
                frameSize.height());
            solid = QColor();
        }
        if (picture.isNull() && !solid.isValid())
            return;
        if (blend != Blend::Erase)
            applyPointEffects(clip, local,
                m_timeline->fps.isValid() ? m_timeline->fps.toDouble() : 25.0, picture, solid);
        if (blend != Blend::Erase && isGraded(clip, local))
            gradePicture(clip, local, content, outScale, picture, pictureRect, solid);
        if (blend == Blend::Erase) {
            solid = QColor(Qt::white);
            picture = QImage();
        }

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
        if (blend == Blend::Erase)
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        else if (blend == Blend::Add)
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
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

    /// The picture of @p clip at @p local before its effects (its source): @p picture covering
    /// @p pictureRect (content coordinates, centred) or a @p solid colour. False if nothing.
    bool clipPicture(const ClipData& clip, int64_t local, double outScale, QSizeF& content,
        QSizeF& sourcePx, QImage& picture, QRectF& pictureRect, QColor& solid)
    {
        const QSizeF frameSize(m_timeline->width, m_timeline->height);
        switch (clip.kind) {
        case ClipKind::Media: {
            // Proxy: decode the substitute, keep the original's geometry (see ProxyRef).
            const auto proxy
                = m_proxies ? m_proxies->constFind(clip.mediaId) : ProxyMap::const_iterator();
            const bool useProxy = m_proxies && proxy != m_proxies->constEnd();
            const QString path = useProxy ? proxy->path
                : m_media                 ? m_media->value(clip.mediaId)
                                          : QString();
            if (path.isEmpty())
                return false;
            const int64_t timeUs = clampedSourceTimeUs(clip, m_frame, m_timeline->fps);
            DecoderPool::Entry* entry = m_decoders.acquire(path, timeUs);
            if (!entry)
                return false;
            const media::MediaInfo& info = entry->decoder->info();
            // Clip Attributes: the decoder applies the file's pixel aspect and rotation; an
            // override changes the display size (the picture is stretched into it) and an
            // extra rotation turns the decoded image. Data levels and deinterlacing are GPU
            // only (like effects).
            const ClipAttributes& attributes = clip.attributes;
            const int extraRotation = normalizedRotation(attributes.rotation);
            const QSizeF fileDisplay
                = useProxy ? proxy->displaySize : QSizeF(info.displayWidth, info.displayHeight);
            if (useProxy || !attributes.pixelAspect.isValid()) {
                sourcePx = extraRotation % 180 ? fileDisplay.transposed() : fileDisplay;
            } else {
                sourcePx = displaySize(info.width, info.height, attributes.pixelAspect,
                    normalizedRotation(info.rotation + extraRotation));
            }
            content = fitSize(sourcePx, frameSize);
            // Decode at the size the picture occupies in the output (never above native).
            const double zoom = std::max(1.0, maxZoom(clip, local, m_dynamicZoom));
            QSize want = (content * outScale * zoom).toSize().boundedTo(sourcePx.toSize());
            if (extraRotation % 180)
                want.transpose(); // decoded in the file's orientation, turned below
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
            if (extraRotation != 0 && !picture.isNull())
                picture = picture.transformed(QTransform().rotate(extraRotation));
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
                return false;
            sourcePx = QSizeF(n->width, n->height);
            content = fitSize(sourcePx, frameSize);
            const double zoom = std::clamp(maxZoom(clip, local, m_dynamicZoom), 1.0, 2.0);
            const QSize want = (content * outScale * zoom).toSize().expandedTo(QSize(1, 1));
            ++m_depth;
            picture
                = renderTimeline(*n, nestedFrameAt(clip, m_frame, m_timeline->fps, n->fps), want);
            --m_depth;
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        case ClipKind::Adjustment:
            if (m_below.isNull())
                return false;
            picture = m_below;
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        case ClipKind::Generator: {
            const double zoom = std::clamp(maxZoom(clip, local, m_dynamicZoom), 1.0, 2.0);
            const QSize canvas = (frameSize * outScale * zoom).toSize().expandedTo(QSize(1, 1));
            picture = renderGeneratorImage(
                resolveGenerator(clip, local, m_frame + m_timeline->startTimecode, m_timeline->fps),
                canvas, frameSize);
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        case ClipKind::Subtitle:
            return false; // drawn over the picture by drawSubtitles()
        case ClipKind::Solid:
            solid = clip.solidColor.isValid() ? clip.solidColor : QColor(0x30, 0x30, 0x34);
            pictureRect = QRectF(
                -content.width() / 2, -content.height() / 2, content.width(), content.height());
            break;
        }
        return true;
    }

    /// Node @p node (0 = MediaOut) of @p clip's composition at comp frame @p compFrame as an
    /// image of @p canvas px. The clip's own picture (MediaIn) is @p picture / @p solid
    /// covering @p pictureRect; MediaIns of other media pool items decode here.
    QImage compositionImage(const ClipData& clip, int64_t compFrame, quint64 node, QSize canvas,
        const QImage& picture, const QRectF& pictureRect, const QColor& solid)
    {
        const QSizeF frameSize(m_timeline->width, m_timeline->height);
        const double scale = canvas.width() / frameSize.width();
        CpuCompEval ev;
        ev.comp = &*clip.composition;
        ev.compFrame = compFrame;
        ev.frame = frameSize;
        ev.size = canvas;
        ev.scale = scale;
        ev.fps = m_timeline->fps.isValid() ? m_timeline->fps.toDouble() : 25.0;
        ev.warned = &m_warned;
        const QRectF own = pictureRect.translated(frameSize.width() / 2, frameSize.height() / 2);
        ev.mediaIn = [&](const CompNode& n, int64_t at) -> QImage {
            QImage out(canvas, QImage::Format_ARGB32_Premultiplied);
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.scale(scale, scale);
            if (n.mediaId.isEmpty()) {
                if (solid.isValid())
                    p.fillRect(own, solid);
                else if (!picture.isNull())
                    p.drawImage(own, picture);
                return out;
            }
            // Another media pool item, from its start (comp frame 0), fitted like a clip.
            const QString path = m_media ? m_media->value(n.mediaId) : QString();
            DecoderPool::Entry* entry = path.isEmpty()
                ? nullptr
                : m_decoders.acquire(path, framesToUs(at, m_timeline->fps));
            if (!entry)
                return out;
            const media::MediaInfo& info = entry->decoder->info();
            const QSizeF content
                = fitSize(QSizeF(info.displayWidth, info.displayHeight), frameSize);
            const QImage img = entry->decoder->frameAt(framesToUs(at, m_timeline->fps),
                (content * scale).toSize().expandedTo(QSize(1, 1)));
            p.drawImage(
                QRectF((frameSize.width() - content.width()) / 2,
                    (frameSize.height() - content.height()) / 2, content.width(), content.height()),
                img);
            return out;
        };
        const quint64 id = node ? node : clip.composition->mediaOutId;
        return evaluateCompositionCpu(ev, id);
    }

    /// The pointwise effects (keyers, colour) on the clip's picture before its crop, masks
    /// and transform, in the clip's order (fxmath::applyPointEffect(); the GPU's other
    /// effects are skipped). A solid becomes a small image first so it can be keyed.
    static void applyPointEffects(
        const ClipData& clip, int64_t local, double fps, QImage& picture, QColor& solid)
    {
        constexpr int kSolidImageEdge = 8; // a solid is uniform: any small size is exact
        for (const EffectInstance& e : clip.effects) {
            if (!e.enabled || !fxmath::isPointEffect(e.type))
                continue;
            if (solid.isValid()) {
                picture = QImage(
                    QSize(kSolidImageEdge, kSolidImageEdge), QImage::Format_ARGB32_Premultiplied);
                picture.fill(solid);
                solid = QColor();
            }
            fxmath::applyPointEffect(e.type, e.params, local, picture, fps);
        }
    }

    /// True if the Color page grade of @p clip changes its picture at @p local.
    bool isGraded(const ClipData& clip, int64_t local) const
    {
        return m_grades && !clip.grade.isEmpty() && !gradeops::isIdentity(clip.grade, local);
    }

    /// The clip's Color page grade on its picture (after the point effects, before the crop,
    /// masks and transform), the twin of the GPU layer pass: picture fractions map onto the
    /// content box (@p content, centred). A solid becomes an image first: a small one when the
    /// grade has no windows (uniform result), else one at the output resolution.
    static void gradePicture(const ClipData& clip, int64_t local, QSizeF content, double outScale,
        QImage& picture, QRectF& pictureRect, QColor& solid)
    {
        constexpr int kSolidImageEdge = 8;
        constexpr int kMaxSolidEdge = 4096;
        if (solid.isValid()) {
            const bool spatial = std::any_of(clip.grade.nodes.cbegin(), clip.grade.nodes.cend(),
                [](const GradeNode& n) { return !n.windows.isEmpty(); });
            QSize size(kSolidImageEdge, kSolidImageEdge);
            if (spatial) {
                size = (pictureRect.size() * outScale).toSize().expandedTo(QSize(1, 1));
                size = size.boundedTo(QSize(kMaxSolidEdge, kMaxSolidEdge));
            }
            picture = QImage(size, QImage::Format_ARGB32_Premultiplied);
            picture.fill(solid);
            solid = QColor();
        }
        if (picture.isNull() || pictureRect.width() <= 0 || pictureRect.height() <= 0)
            return;
        // Picture pixel x <-> content x = rect.x + (x + 0.5) * rect.w / W (same for y).
        const double px = picture.width() / pictureRect.width();
        const double py = picture.height() / pictureRect.height();
        grademath::PictureGeometry g;
        g.originX = (-content.width() / 2 - pictureRect.x()) * px;
        g.originY = (-content.height() / 2 - pictureRect.y()) * py;
        g.width = content.width() * px;
        g.height = content.height() * py;
        applyGrade(prepareGrade(clip.grade, local, g), picture);
    }

    static bool showsAdjustment(const TrackFrame& tf)
    {
        auto adjustment
            = [](const ClipData* c) { return c && c->enabled && c->kind == ClipKind::Adjustment; };
        if (tf.single)
            return adjustment(tf.single);
        return tf.transition
            && (adjustment(tf.transition->outgoing) || adjustment(tf.transition->incoming));
    }

    /// Captions of the enabled subtitle tracks over @p image (same layout as the GPU path).
    static void drawSubtitles(QImage& image, const TimelineData& tl, int64_t frame)
    {
        QPainter painter(&image);
        const double scale = double(image.height()) / tl.height;
        for (const TrackData& track : tl.subtitleTracks) {
            if (!track.enabled)
                continue;
            const ClipData* caption = captionAt(track, frame);
            if (!caption)
                continue;
            CaptionPlacement placement;
            const TitleStyle style = resolveCaptionStyle(
                *caption, &track, caption->localFrame(frame), image.size(), scale, &placement);
            const TitleImage text = renderTitle(style);
            if (text.image.isNull())
                continue;
            painter.drawImage(text.rect.translated(
                                  captionOffset(text.rect, image.size(), style.align, placement)),
                text.image);
        }
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

    QThreadPool m_prepareThreads; ///< decoder look-ahead; outlives m_decoders
    DecoderPool m_decoders;
    QSet<QString> m_warned;
    QImage m_below; ///< composite below the adjustment track being drawn (output size)
    const TimelineData* m_timeline = nullptr;
    const MediaPathMap* m_media = nullptr;
    const ProxyMap* m_proxies = nullptr;
    const NestedTimelines* m_nested = nullptr;
    int m_depth = 0;
    bool m_grades = true; ///< apply grades (false: RenderRequest::bypassGrades)
    bool m_dynamicZoom = true; ///< RenderRequest::dynamicZoom
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
