// SPDX-License-Identifier: GPL-3.0-or-later
// GlRenderer — per-frame orchestration of the GPU compositor.
//
// Per frame:  evaluate tracks → decode visible media in parallel → for each video track
// (V1 first): draw its clip, or its two transition sides into temporaries + transition pass,
// premultiplied-over into an RGBA16F accumulator → copy to an 8-bit output (slot texture
// and/or PBO readback).
//
// Per clip (drawClip):
//   direct path (no active effects): the source (YUV planes / title texture / solid colour) is
//     drawn straight into the target through the full transform with crop + opacity — one pass;
//   layer path (effects, masks): the source is first drawn upright into a "layer" target (layer
//     resolution, optional transparent margin), the effects ping-pong over pooled targets, and
//     the layer is then drawn through the transform with opacity.
#include "render/GlRenderer.h"

#include "core/ParamKeys.h"
#include "render/CaptionRenderer.h"
#include "render/RenderLog.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <latch>

namespace kk::render {

namespace {

/// Largest texture/target edge we allocate (GL 3.3 guarantees 1024; real GPUs do ≥ 8192).
constexpr int kMaxTargetEdge = 8192;
/// Title textures kept in the cache (each is the text's bounding box, typically < 2 MB).
constexpr int kTitleCacheSize = 24;
/// Default colour of a Solid clip without a colour (solid_color descriptor default).
const QColor kDefaultSolid(0x30, 0x30, 0x34);
/// Worker threads for decoding the clips of one frame in parallel.
constexpr int kDecodeThreads = 4;
/// Deepest nesting rendered (a timeline cannot contain itself, see compound::canNest(); this
/// only guards against damaged data).
constexpr int kMaxNestingDepth = 16;
/// A nested timeline is composited at the size it occupies in the output, times its zoom up
/// to this factor (sharper when zoomed in, bounded memory).
constexpr double kMaxNestedZoom = 2.0;

/// Quantises a zoom factor for title rasterisation so that animated zooms re-rasterise only at
/// a few steps (the texture is then scaled by at most ~1.5× by the GPU).
double titleRasterZoom(double zoom)
{
    if (zoom <= 1.0)
        return std::max(0.25, std::ceil(zoom * 4.0) / 4.0);
    constexpr double kSteps[] = {1.5, 2.0, 3.0, 4.0};
    for (const double s : kSteps) {
        if (zoom <= s)
            return s;
    }
    return 4.0;
}

/// Crop rectangle (normalised content coordinates) of @p clip for a source of @p sourcePx.
QRectF cropRect(const ClipData& clip, int64_t localFrame, QSizeF sourcePx)
{
    const ParamSet& p = clip.transform;
    const double l = std::max(0.0, p.value(keys::CropL, localFrame)) / sourcePx.width();
    const double r = std::max(0.0, p.value(keys::CropR, localFrame)) / sourcePx.width();
    const double t = std::max(0.0, p.value(keys::CropT, localFrame)) / sourcePx.height();
    const double b = std::max(0.0, p.value(keys::CropB, localFrame)) / sourcePx.height();
    return QRectF(QPointF(l, t), QPointF(1.0 - r, 1.0 - b));
}

/// Unit quad → whole target (normalised device coordinates).
const QTransform kFullscreenNdc(2, 0, 0, 2, -1, -1);

/// True if @p tf shows an adjustment clip (alone or as a side of a transition).
bool showsAdjustment(const TrackFrame& tf)
{
    auto adjustment
        = [](const ClipData* c) { return c && c->enabled && c->kind == ClipKind::Adjustment; };
    if (tf.single)
        return adjustment(tf.single);
    return tf.transition
        && (adjustment(tf.transition->outgoing) || adjustment(tf.transition->incoming));
}

/// @p crop (content coordinates) expressed in the coordinates of sub-rectangle @p quad.
QRectF cropInQuad(const QRectF& crop, const QRectF& quad)
{
    return QRectF((crop.left() - quad.left()) / quad.width(),
        (crop.top() - quad.top()) / quad.height(), crop.width() / quad.width(),
        crop.height() / quad.height());
}

} // namespace

/// What a clip draws this frame. Content = the clip's picture at zoom 1 in timeline pixels.
struct GlRenderer::ClipSource
{
    enum class Kind {
        Video,
        Texture,
        Solid
    };
    Kind kind = Kind::Solid;
    QSizeF content; ///< timeline pixels (display orientation)
    QSizeF sourcePx; ///< source pixels covering the content (crop units, native resolution)
    QRectF quad = QRectF(0, 0, 1, 1); ///< part of the content the source covers (content uv)
    /// The texture already has output resolution (adjustment clip source): layers never need
    /// more detail than that.
    bool outputSized = false;
    const DecodedClip* video = nullptr;
    PlaneTextures* planes = nullptr;
    GLuint texture = 0;
    float color[4] = {0, 0, 0, 0}; ///< premultiplied
};

GlRenderer::GlRenderer(QOffscreenSurface* surface)
    : m_surface(surface)
{
    m_decodeThreads.setMaxThreadCount(kDecodeThreads);
    m_decodeThreads.setObjectName(QStringLiteral("kk.render.decode"));
}

GlRenderer::~GlRenderer()
{
    shutdown();
}

bool GlRenderer::initialize(QString* why)
{
    m_context = std::make_unique<QOpenGLContext>();
    QSurfaceFormat format = m_surface->format();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    m_context->setFormat(format);
    QOpenGLContext* share = QOpenGLContext::globalShareContext();
    if (share)
        m_context->setShareContext(share);
    if (!m_context->create()) {
        *why = QStringLiteral("cannot create an OpenGL context");
        return false;
    }
    if (share && m_context->shareContext() != share)
        qCWarning(lcRender) << "context sharing failed: viewers must use readback images";
    if (!m_context->makeCurrent(m_surface)) {
        *why = QStringLiteral("cannot make the OpenGL context current on the render thread");
        return false;
    }
    const QSurfaceFormat actual = m_context->format();
    if (m_context->isOpenGLES() || actual.version() < qMakePair(3, 3)) {
        *why = QStringLiteral("OpenGL %1.%2%3 < 3.3 core")
                   .arg(actual.majorVersion())
                   .arg(actual.minorVersion())
                   .arg(m_context->isOpenGLES() ? QStringLiteral(" ES") : QString());
        return false;
    }
    m_gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_context.get());
    if (!m_gl || !m_gl->initializeOpenGLFunctions()) {
        *why = QStringLiteral("OpenGL 3.3 core functions unavailable");
        return false;
    }
    const auto renderer = reinterpret_cast<const char*>(m_gl->glGetString(GL_RENDERER));
    m_description = QStringLiteral("OpenGL %1.%2 (%3)")
                        .arg(actual.majorVersion())
                        .arg(actual.minorVersion())
                        .arg(QString::fromLatin1(renderer ? renderer : "unknown"));

    m_kit = std::make_unique<GlKit>(m_gl);
    if (!m_kit->initialize(why) || !m_video.initialize(*m_kit, why)
        || !m_masks.initialize(*m_kit, why) || !m_generators.initialize(*m_kit, why))
        return false;
    m_output = std::make_unique<GlOutput>(*m_kit);

    for (const GlEffectFactory& f : builtinGlEffects()) {
        std::shared_ptr<GlEffect> effect = f.create();
        QString log;
        if (effect->initialize(*m_kit, &log))
            m_effects.insert(QLatin1String(f.id), std::move(effect));
        else
            qCWarning(lcRender).noquote() << "effect" << f.id << "disabled:" << log;
    }
    for (const GlTransitionFactory& f : builtinGlTransitions()) {
        std::shared_ptr<GlTransition> transition = f.create();
        QString log;
        if (transition->initialize(*m_kit, &log))
            m_transitions.insert(QLatin1String(f.id), std::move(transition));
        else
            qCWarning(lcRender).noquote() << "transition" << f.id << "disabled:" << log;
    }
    const auto dissolve = m_transitions.constFind(QStringLiteral("cross_dissolve"));
    if (dissolve == m_transitions.constEnd()) {
        *why = QStringLiteral("cross_dissolve shader failed");
        return false;
    }
    m_fallbackTransition = dissolve->get();

    // Plane textures live with their decoder; delete them when the pool closes it.
    m_decoders.setCloseHook([this](DecoderPool::Entry& e) {
        if (auto planes = std::static_pointer_cast<PlaneTextures>(e.userData))
            GlVideo::release(*m_kit, *planes);
    });
    m_decoded.reserve(16);
    // One list per depth, allocated up front: a deeper level must never reallocate the
    // outer vector while a shallower level iterates its list.
    m_trackFrames.resize(size_t(kMaxNestingDepth) + 1);
    m_trackFrames[0].reserve(16);
    m_nested.reserve(4);
    m_generated.reserve(4);
    m_activeEffects.reserve(16);
    m_activeInstances.reserve(16);
    const GLenum error = m_gl->glGetError();
    if (error != GL_NO_ERROR) {
        *why = QStringLiteral("GL error 0x%1 during initialisation").arg(error, 0, 16);
        return false;
    }
    return true;
}

void GlRenderer::shutdown()
{
    if (!m_context)
        return;
    m_decodeThreads.waitForDone();
    if (m_kit && m_context->makeCurrent(m_surface)) {
        if (m_output)
            m_output->destroy();
        m_decoders.clear();
        for (const TitleTexture& t : std::as_const(m_titles))
            m_gl->glDeleteTextures(1, &t.texture);
        m_titles.clear();
        for (auto& e : m_effects)
            e->destroy(*m_kit);
        for (auto& t : m_transitions)
            t->destroy(*m_kit);
        m_video.destroy(*m_kit);
        m_masks.destroy(*m_kit);
        m_generators.destroy(*m_kit);
        m_generated.clear();
        m_kit->destroy();
        m_context->doneCurrent();
    }
    m_effects.clear();
    m_transitions.clear();
    m_output.reset();
    m_kit.reset();
    m_context.reset();
}

bool GlRenderer::hasPending() const
{
    return m_output && m_output->hasPending();
}

void GlRenderer::finishPending(bool keepNewest)
{
    if (m_output)
        m_output->finishPending(keepNewest);
}

// ---- Decoding ----

void GlRenderer::queueDecode(const ClipData* clip, int64_t frame, int64_t nextFrame,
    const TimelineData& tl, const NestedTimelines& nested, const MediaPathMap* media, int depth)
{
    if (!clip || !clip->enabled)
        return;
    if (clip->kind == ClipKind::Nested) {
        const TimelineData* n = nested.find(clip->nestedUid);
        if (!n || depth >= kMaxNestingDepth)
            return;
        queueTimeline(*n, nestedFrameAt(*clip, frame, tl.fps, n->fps),
            nestedFrameAt(*clip, nextFrame, tl.fps, n->fps), nested, media, depth + 1);
        return;
    }
    if (clip->kind != ClipKind::Media)
        return;
    const int64_t timeUs = clampedSourceTimeUs(*clip, frame, tl.fps);
    if (decoded(clip, timeUs))
        return;
    const QString path = media ? media->value(clip->mediaId) : QString();
    if (path.isEmpty())
        return; // unknown media id: the clip is offline and draws nothing
    DecoderPool::Entry* entry = m_decoders.acquire(path, timeUs);
    if (!entry)
        return;
    m_decoded.push_back(
        {clip, entry, timeUs, clampedSourceTimeUs(*clip, nextFrame, tl.fps), nullptr});
}

void GlRenderer::queueTimeline(const TimelineData& tl, int64_t frame, int64_t nextFrame,
    const NestedTimelines& nested, const MediaPathMap* media, int depth)
{
    for (const TrackData& track : tl.videoTracks) {
        if (!track.enabled)
            continue;
        const TrackFrame tf = evaluateTrack(track, frame);
        if (tf.single) {
            queueDecode(tf.single, frame, nextFrame, tl, nested, media, depth);
        } else if (tf.transition) {
            queueDecode(tf.transition->outgoing, frame, nextFrame, tl, nested, media, depth);
            queueDecode(tf.transition->incoming, frame, nextFrame, tl, nested, media, depth);
        }
    }
}

void GlRenderer::decodeQueued()
{
    const int n = int(m_decoded.size());
    if (n == 0)
        return;
    for (DecodedClip& d : m_decoded)
        d.entry->waitAhead(); // usually already done: it ran while the last frame was drawn
    // Every queued clip has its own decoder (DecoderPool hands out a decoder once per frame),
    // so the decodes are independent: run all but the first on worker threads.
    std::latch done(n - 1);
    for (int i = 1; i < n; ++i) {
        DecodedClip* d = &m_decoded[size_t(i)];
        m_decodeThreads.start([d, &done] {
            d->frame = d->entry->decoder->decodeFrame(d->timeUs);
            done.count_down();
        });
    }
    m_decoded[0].frame = m_decoded[0].entry->decoder->decodeFrame(m_decoded[0].timeUs);
    done.wait();
}

void GlRenderer::startDecodeAhead()
{
    // Playback and export ask for frame + 1 next. Decoding it now, on a worker thread, overlaps
    // the decode with this frame's upload/drawing/output and with the time until the next
    // request arrives; the decoder then returns the cached frame. Only for decoders that are
    // moving forward one frame at a time (not for scrubbing, reverse play or stills).
    for (const DecodedClip& d : m_decoded) {
        DecoderPool::Entry* entry = d.entry;
        if (!d.frame || entry->ahead.valid() || entry->previousTimeUs < 0)
            continue;
        const int64_t nextUs = d.nextTimeUs;
        const int64_t step = nextUs - d.timeUs;
        const int64_t lastStep = d.timeUs - entry->previousTimeUs;
        if (step <= 0 || lastStep <= 0 || lastStep > 2 * step + 1)
            continue;
        auto done = std::make_shared<std::promise<void>>();
        entry->ahead = done->get_future();
        media::VideoDecoder* decoder = entry->decoder.get();
        m_decodeThreads.start([decoder, nextUs, done] {
            (void)decoder->decodeFrame(nextUs);
            done->set_value();
        });
    }
}

const GlRenderer::DecodedClip* GlRenderer::decoded(const ClipData* clip, int64_t timeUs) const
{
    for (const DecodedClip& d : m_decoded) {
        if (d.clip == clip && d.timeUs == timeUs)
            return &d;
    }
    return nullptr;
}

// ---- Frame ----

void GlRenderer::render(RenderJob& job)
{
    QElapsedTimer timer;
    timer.start();
    const RenderRequest& rq = job.request;
    auto invalid = [&job] {
        auto frame = std::make_shared<RenderedFrame>();
        frame->frame = job.request.frame;
        job.promise.set_value(std::move(frame));
    };
    if (!rq.timeline || rq.timeline->width <= 0 || rq.timeline->height <= 0) {
        invalid();
        return;
    }
    const TimelineData& tl = *rq.timeline;
    const QSize requested = rq.outSize.isEmpty() ? QSize(tl.width, tl.height) : rq.outSize;
    const QSize out = scaledRenderSize(requested, rq.quality);
    if (out.width() > kMaxTargetEdge || out.height() > kMaxTargetEdge) {
        qCWarning(lcRender) << "output size too large:" << out;
        invalid();
        return;
    }

    FrameContext fc;
    fc.timeline = &tl;
    fc.frame = rq.frame;
    fc.outSize = out;
    fc.sx = double(out.width()) / tl.width;
    fc.sy = double(out.height()) / tl.height;

    fc.nested = &tl.nested;
    fc.subtitles = rq.subtitles;

    // 1 + 2. Decode every visible media clip, including the clips inside visible nested
    //        timelines (in parallel when there are several).
    m_decoders.beginFrame();
    m_decoded.clear();
    m_nested.clear();
    queueTimeline(tl, rq.frame, rq.frame + 1, tl.nested, job.media.get(), 0);
    decodeQueued();
    const qint64 decodedNs = timer.nsecsElapsed();
    startDecodeAhead();
    m_uploadNs = 0;

    // 3. Composite (V1 = index 0 is the bottom layer, drawn first).
    PooledTarget accumulator = compositeTimeline(fc);
    if (fc.subtitles && !tl.subtitleTracks.empty())
        drawSubtitles(fc, *accumulator);
    m_nested.clear(); // the nested composites were consumed by the draws above
    m_generated.clear();

    // 4. Output.
    const qint64 compositedNs = timer.nsecsElapsed();
    PooledTarget scratch;
    GLuint readFbo = 0;
    std::shared_ptr<RenderedFrame> result
        = m_output->produce(*accumulator, (rq.outputs & OutputTexture) != 0, scratch, &readFbo);
    result->frame = rq.frame;
    result->decodeMs = double(decodedNs) / 1e6;
    result->uploadMs = double(m_uploadNs) / 1e6;
    result->compositeMs = double(compositedNs - decodedNs - m_uploadNs) / 1e6;
    result->outputMs = double(timer.nsecsElapsed() - compositedNs) / 1e6; // + readback below
    if (rq.outputs & OutputImage) {
        m_output->startReadback(job, std::move(result), readFbo, timer);
    } else {
        result->renderMs = double(timer.nsecsElapsed()) / 1e6;
        job.promise.set_value(std::move(result));
    }

    // Housekeeping (cheap loops; actual deletions are rare).
    m_kit->targets().trim();
    m_decoders.collectIdle();
    trimTitleCache();
}

PooledTarget GlRenderer::compositeTimeline(const FrameContext& fc)
{
    std::vector<TrackFrame>& frames = m_trackFrames[size_t(fc.depth)];
    frames.clear();
    for (const TrackData& track : fc.timeline->videoTracks) {
        if (!track.enabled)
            continue;
        TrackFrame tf = evaluateTrack(track, fc.frame);
        if (!tf.isEmpty())
            frames.push_back(tf);
    }
    PooledTarget accumulator = m_kit->acquire(fc.outSize);
    m_kit->bindTarget(*accumulator);
    m_kit->clearTransparent();
    // Index loop: a nested clip composites at depth + 1 and never touches this list.
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!showsAdjustment(frames[i])) {
            drawTrack(frames[i], fc, *accumulator);
            continue;
        }
        drawAdjustment(frames[i], fc, accumulator);
    }
    return accumulator;
}

bool GlRenderer::hasActiveEffect(const ClipData& clip, int64_t local) const
{
    for (const EffectInstance& e : clip.effects) {
        if (!e.enabled)
            continue;
        const auto it = m_effects.constFind(e.type);
        if (it != m_effects.constEnd() && !(*it)->isIdentity(e.params, local))
            return true;
    }
    return false;
}

void GlRenderer::drawAdjustment(
    const TrackFrame& tf, const FrameContext& fc, PooledTarget& accumulator)
{
    // What has been composited so far (every track below) is the adjustment clip's source.
    FrameContext adjust = fc;
    if (tf.single) {
        const ClipData& clip = *tf.single;
        const int64_t local = clip.localFrame(fc.frame);
        if (hasIdentityFootprint(clip, local)) {
            // The common case (a grade or blur over everything): nothing to do without an
            // active effect; otherwise the layer path reads the accumulator into its layer and
            // the result overwrites it - no copy, no footprint pass.
            if (!hasActiveEffect(clip, local))
                return;
            adjust.below = accumulator->texture;
            drawClip(clip, adjust, *accumulator, ClipBlend::Replace);
            return;
        }
    }
    // General case: the accumulator is copied into a fresh target (a texture cannot be sampled
    // while it is drawn to), then drawTrack() replaces the clip's footprint with the processed
    // picture: masks, crop, transform and opacity < 100 % leave the tracks below visible
    // where the adjustment does not reach. Cost: one full-frame copy and one footprint pass.
    PooledTarget below = std::move(accumulator);
    accumulator = m_kit->acquire(fc.outSize);
    m_kit->bindTarget(*accumulator);
    m_kit->setBlendOver(false);
    m_kit->blit(below->texture, kFullscreenNdc);
    adjust.below = below->texture;
    drawTrack(tf, adjust, *accumulator);
}

const RenderTarget* GlRenderer::nestedComposite(
    const TimelineData& tl, int64_t frame, QSize size, const FrameContext& parent)
{
    for (const NestedComposite& n : m_nested) {
        if (n.timeline == &tl && n.frame == frame && n.size == size)
            return n.target.get();
    }
    FrameContext fc;
    fc.timeline = &tl;
    fc.frame = frame;
    fc.outSize = size;
    fc.sx = double(size.width()) / tl.width;
    fc.sy = double(size.height()) / tl.height;
    fc.nested = parent.nested;
    fc.depth = parent.depth + 1;
    PooledTarget target = compositeTimeline(fc);
    m_nested.push_back({&tl, frame, size, std::move(target)});
    return m_nested.back().target.get();
}

void GlRenderer::drawTrack(
    const TrackFrame& tf, const FrameContext& fc, const RenderTarget& accumulator)
{
    if (tf.single) {
        m_kit->bindTarget(accumulator);
        if (tf.single->kind == ClipKind::Adjustment && fc.below) {
            // Inside its footprint the processed picture REPLACES the tracks below (mixed by
            // opacity), outside it they stay untouched: erase the footprint, then add.
            drawClip(*tf.single, fc, accumulator, ClipBlend::Erase);
            drawClip(*tf.single, fc, accumulator, ClipBlend::Add);
            return;
        }
        drawClip(*tf.single, fc, accumulator);
        return;
    }
    // Transition: render both sides (already transformed) into temporaries, then combine them
    // with the transition shader over the accumulator.
    const TransitionSpan& span = *tf.transition;
    PooledTarget a, b;
    if (span.outgoing && span.outgoing->enabled) {
        a = m_kit->acquire(fc.outSize);
        m_kit->bindTarget(*a);
        m_kit->clearTransparent();
        drawClip(*span.outgoing, fc, *a);
    }
    if (span.incoming && span.incoming->enabled) {
        b = m_kit->acquire(fc.outSize);
        m_kit->bindTarget(*b);
        m_kit->clearTransparent();
        drawClip(*span.incoming, fc, *b);
    }
    GlTransition* transition = m_fallbackTransition;
    if (auto it = m_transitions.constFind(span.transition->type); it != m_transitions.constEnd())
        transition = it->get();
    TransitionInput in;
    in.a = a ? a->texture : m_kit->transparentTexture();
    in.b = b ? b->texture : m_kit->transparentTexture();
    in.progress = span.progress(fc.frame);
    in.params = &span.transition->params;
    in.localFrame = fc.frame - span.start;
    in.size = fc.outSize;
    m_kit->bindTarget(accumulator);
    m_kit->setBlendOver(true);
    transition->draw(*m_kit, in);
}

bool GlRenderer::resolveSource(const ClipData& clip, int64_t localFrame, const FrameContext& fc,
    double maxZoomValue, ClipSource& src)
{
    const QSizeF frameSize(fc.timeline->width, fc.timeline->height);
    switch (clip.kind) {
    case ClipKind::Media: {
        const DecodedClip* d
            = decoded(&clip, clampedSourceTimeUs(clip, fc.frame, fc.timeline->fps));
        if (!d || !d->frame)
            return false;
        auto planes = std::static_pointer_cast<PlaneTextures>(d->entry->userData);
        if (!planes) {
            planes = std::make_shared<PlaneTextures>();
            d->entry->userData = planes;
        }
        QElapsedTimer uploadTimer;
        uploadTimer.start();
        m_video.upload(*m_kit, *planes, d->frame);
        m_uploadNs += uploadTimer.nsecsElapsed();
        const media::VideoFrame& f = *d->frame;
        src.kind = ClipSource::Kind::Video;
        src.sourcePx = displaySize(f.width, f.height, f.sampleAspectRatio, f.rotation);
        src.content = fitSize(src.sourcePx, frameSize);
        src.video = d;
        src.planes = planes.get();
        return true;
    }
    case ClipKind::Title: {
        const double rasterScale = std::max(fc.sx, fc.sy) * titleRasterZoom(maxZoomValue);
        const QSize canvas(std::max(1, int(std::lround(frameSize.width() * rasterScale))),
            std::max(1, int(std::lround(frameSize.height() * rasterScale))));
        const TitleTexture* t
            = titleTexture(resolveTitleStyle(clip, localFrame, canvas, rasterScale));
        if (!t)
            return false;
        src.kind = ClipSource::Kind::Texture;
        src.content = frameSize;
        src.sourcePx = frameSize; // crop is given in timeline pixels for generated clips
        src.texture = t->texture;
        src.quad = QRectF(t->rect.x() / canvas.width(), t->rect.y() / canvas.height(),
            t->rect.width() / canvas.width(), t->rect.height() / canvas.height());
        return true;
    }
    case ClipKind::Nested: {
        // The nested timeline is composited into its own target at the size it covers in
        // the output (fitted like media: aspect kept), then drawn like a texture source, so
        // the instance's transform, crop, opacity, effects and transitions apply to the
        // whole nested picture.
        const TimelineData* n = fc.nested ? fc.nested->find(clip.nestedUid) : nullptr;
        if (!n || n->width <= 0 || n->height <= 0 || fc.depth + 1 > kMaxNestingDepth)
            return false;
        const QSizeF native(n->width, n->height);
        const QSizeF content = fitSize(native, frameSize);
        const double scale = std::max(fc.sx, fc.sy) * content.width() / native.width()
            * std::clamp(maxZoomValue, 1.0, kMaxNestedZoom);
        QSize size(std::max(1, int(std::ceil(native.width() * scale))),
            std::max(1, int(std::ceil(native.height() * scale))));
        if (size.width() > kMaxTargetEdge || size.height() > kMaxTargetEdge)
            size.scale(kMaxTargetEdge, kMaxTargetEdge, Qt::KeepAspectRatio);
        const RenderTarget* target = nestedComposite(
            *n, nestedFrameAt(clip, fc.frame, fc.timeline->fps, n->fps), size, fc);
        if (!target)
            return false;
        src.kind = ClipSource::Kind::Texture;
        src.content = content;
        src.sourcePx = native; // crop in nested timeline pixels
        src.texture = target->texture;
        return true;
    }
    case ClipKind::Adjustment: {
        // The composite of the tracks below (see compositeTimeline()), covering the frame.
        if (!fc.below)
            return false;
        src.kind = ClipSource::Kind::Texture;
        src.content = frameSize;
        src.sourcePx = frameSize; // crop in timeline pixels
        src.texture = fc.below;
        src.outputSized = true;
        return true;
    }
    case ClipKind::Generator: {
        const RenderTarget* target = generatorTarget(clip, localFrame, fc, maxZoomValue);
        if (!target)
            return false;
        src.kind = ClipSource::Kind::Texture;
        src.content = frameSize;
        src.sourcePx = frameSize;
        src.texture = target->texture;
        return true;
    }
    case ClipKind::Subtitle:
        return false; // drawn over the picture by drawSubtitles(), never as a layer
    case ClipKind::Solid: {
        const QColor c = clip.solidColor.isValid() ? clip.solidColor : kDefaultSolid;
        const float a = float(c.alphaF());
        src.kind = ClipSource::Kind::Solid;
        src.content = frameSize;
        src.sourcePx = frameSize;
        src.color[0] = float(c.redF()) * a;
        src.color[1] = float(c.greenF()) * a;
        src.color[2] = float(c.blueF()) * a;
        src.color[3] = a;
        return true;
    }
    }
    return false;
}

void GlRenderer::drawSource(
    const ClipSource& src, const QTransform& quadToNdc, const QRectF& crop, float opacity)
{
    switch (src.kind) {
    case ClipSource::Kind::Video:
        m_video.draw(*m_kit, *src.planes, *src.video->frame, quadToNdc, crop, opacity);
        break;
    case ClipSource::Kind::Texture:
        m_kit->blit(src.texture, quadToNdc, opacity, crop);
        break;
    case ClipSource::Kind::Solid: {
        const float c[4] = {src.color[0] * opacity, src.color[1] * opacity, src.color[2] * opacity,
            src.color[3] * opacity};
        m_kit->fill(quadToNdc, c, crop);
        break;
    }
    }
}

void GlRenderer::applyBlend(ClipBlend blend)
{
    m_kit->setBlendOver(true);
    switch (blend) {
    case ClipBlend::Over:
        m_gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case ClipBlend::Erase:
        m_gl->glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case ClipBlend::Add:
        m_gl->glBlendFunc(GL_ONE, GL_ONE);
        break;
    case ClipBlend::Replace:
        m_kit->setBlendOver(false);
        break;
    }
}

void GlRenderer::drawClip(
    const ClipData& clip, const FrameContext& fc, const RenderTarget& target, ClipBlend blend)
{
    if (!clip.enabled)
        return;
    const int64_t local = clip.localFrame(fc.frame);
    const float opacity
        = float(std::clamp(clip.transform.value(keys::Opacity, local) / 100.0, 0.0, 1.0));
    if (opacity <= 0.0f)
        return;
    const double zoom = maxZoom(clip, local);
    ClipSource src;
    if (zoom <= 0.0 || !resolveSource(clip, local, fc, zoom, src))
        return;
    const bool erase = blend == ClipBlend::Erase;
    if (erase) {
        // The footprint: an opaque white picture of the same size through the same crop,
        // masks and transform.
        src.kind = ClipSource::Kind::Solid;
        src.quad = QRectF(0, 0, 1, 1);
        std::fill(std::begin(src.color), std::end(src.color), 1.0f);
    }
    const QRectF crop = cropRect(clip, local, src.sourcePx);
    if (crop.width() <= 0 || crop.height() <= 0)
        return;

    // Content (centred, timeline px) → timeline frame px → output px.
    const QTransform toOutput
        = clipTransform(clip, local, QSizeF(fc.timeline->width, fc.timeline->height))
        * QTransform::fromScale(fc.sx, fc.sy);
    const QTransform outputToNdc = GlKit::pixelToNdc(fc.outSize);
    const double cw = src.content.width();
    const double ch = src.content.height();

    // Active effects of this clip (known, enabled, not a no-op with the current values).
    m_activeEffects.clear();
    m_activeInstances.clear();
    double padding = 0.0;
    for (const EffectInstance& e : clip.effects) {
        if (!e.enabled || erase)
            continue;
        const auto it = m_effects.constFind(e.type);
        if (it == m_effects.constEnd()) {
            if (!m_warnedEffects.contains(e.type)) {
                m_warnedEffects.insert(e.type);
                qCWarning(lcRender) << "no GPU implementation for effect" << e.type;
            }
            continue;
        }
        if ((*it)->isIdentity(e.params, local))
            continue;
        m_activeEffects.push_back(it->get());
        m_activeInstances.push_back(&e);
        padding += (*it)->paddingPx(e.params, local);
    }

    // Masks cut the clip's own picture (after its effects, before its transform), so they
    // need the layer path too.
    const bool masked = GlMasks::hasActiveMasks(clip);
    if (m_activeEffects.empty() && !masked) {
        // Direct path: one pass from the source into the target.
        const QTransform quad(src.quad.width() * cw, 0, 0, src.quad.height() * ch,
            (src.quad.x() - 0.5) * cw, (src.quad.y() - 0.5) * ch);
        m_kit->bindTarget(target);
        applyBlend(blend);
        drawSource(src, quad * toOutput * outputToNdc, cropInQuad(crop, src.quad), opacity);
        if (blend != ClipBlend::Over)
            applyBlend(ClipBlend::Over);
        return;
    }

    // Layer path. Layer resolution: output resolution at the clip's zoom, but never more
    // detailed than the source itself (zooming into a 720p clip does not need a 4K layer).
    const double base = std::max(fc.sx, fc.sy);
    const double native = src.sourcePx.width() / cw; // source pixels per timeline pixel
    const double nativeLimit = src.kind == ClipSource::Kind::Video ? native / base
        : src.outputSized                                          ? 1.0
                                                                   : 4.0;
    double layerScale = base * std::min(zoom, std::max(1.0, nativeLimit));
    const double padPx = std::ceil(padding * layerScale);
    QSize layerSize(
        int(std::ceil(cw * layerScale + 2 * padPx)), int(std::ceil(ch * layerScale + 2 * padPx)));
    if (layerSize.width() > kMaxTargetEdge || layerSize.height() > kMaxTargetEdge) {
        const double shrink
            = double(kMaxTargetEdge) / std::max(layerSize.width(), layerSize.height());
        layerScale *= shrink;
        layerSize = QSize(std::min(kMaxTargetEdge, int(layerSize.width() * shrink)),
            std::min(kMaxTargetEdge, int(layerSize.height() * shrink)));
    }
    layerSize = layerSize.expandedTo(QSize(1, 1));

    PooledTarget layer = m_kit->acquire(layerSize);
    m_kit->bindTarget(*layer);
    m_kit->clearTransparent();
    m_kit->setBlendOver(false);
    const QRectF sourceRect(padPx + src.quad.x() * cw * layerScale,
        padPx + src.quad.y() * ch * layerScale, src.quad.width() * cw * layerScale,
        src.quad.height() * ch * layerScale);
    drawSource(src, GlKit::quadToNdc(sourceRect, layerSize), cropInQuad(crop, src.quad), 1.0f);

    for (size_t i = 0; i < m_activeEffects.size(); ++i) {
        EffectInput in;
        in.layer = layer.get();
        in.scale = layerScale;
        in.localFrame = local;
        in.params = &m_activeInstances[i]->params;
        PooledTarget next = m_activeEffects[i]->apply(*m_kit, in);
        if (next)
            layer = std::move(next);
    }
    if (masked) {
        const MaskLayerGeometry geometry {QSizeF(cw, ch), layerScale, padPx};
        if (PooledTarget next = m_masks.apply(*m_kit, *layer, geometry, clip, local))
            layer = std::move(next);
    }

    // Layer pixel x ↔ content coordinate (x − pad) / layerScale − cw/2 (same for y).
    const QTransform quad(layerSize.width() / layerScale, 0, 0, layerSize.height() / layerScale,
        -padPx / layerScale - cw * 0.5, -padPx / layerScale - ch * 0.5);
    m_kit->bindTarget(target);
    applyBlend(blend);
    m_kit->blit(layer->texture, quad * toOutput * outputToNdc, opacity);
    if (blend != ClipBlend::Over)
        applyBlend(ClipBlend::Over);
}

// ---- Generators and captions ----

const RenderTarget* GlRenderer::generatorTarget(
    const ClipData& clip, int64_t localFrame, const FrameContext& fc, double zoom)
{
    GeneratorFrame g = resolveGenerator(clip, localFrame, fc.frame, fc.timeline->fps);
    if (g.kind == GeneratorFrame::Kind::Unknown)
        return nullptr;
    // Rendered at the size the frame covers in the output (sharper when zoomed in, up to 2x).
    const QSizeF frameSize(fc.timeline->width, fc.timeline->height);
    const double scale = std::max(fc.sx, fc.sy) * std::clamp(zoom, 1.0, kMaxNestedZoom);
    QSize size(std::max(1, int(std::ceil(frameSize.width() * scale))),
        std::max(1, int(std::ceil(frameSize.height() * scale))));
    if (size.width() > kMaxTargetEdge || size.height() > kMaxTargetEdge)
        size.scale(kMaxTargetEdge, kMaxTargetEdge, Qt::KeepAspectRatio);
    PooledTarget target = m_kit->acquire(size);
    m_kit->bindTarget(*target);
    m_generators.drawPattern(*m_kit, g, frameSize);
    if (!g.textStyle.text.isEmpty()) {
        if (g.kind == GeneratorFrame::Kind::Countdown)
            g.textStyle.size = countdownNumberSize(frameSize.height());
        g.textStyle.canvas = size;
        g.textStyle.scale = double(size.height()) / frameSize.height();
        if (const TitleTexture* t = titleTexture(g.textStyle)) {
            const QRectF rect = t->rect.translated(0, g.textOffsetY * size.height());
            m_kit->bindTarget(*target);
            m_kit->setBlendOver(true);
            m_kit->blit(t->texture, GlKit::quadToNdc(rect, size));
        }
    }
    m_generated.push_back(std::move(target));
    return m_generated.back().get();
}

void GlRenderer::drawSubtitles(const FrameContext& fc, const RenderTarget& target)
{
    // Captions are drawn at output resolution straight from the (cached) title textures: a
    // static caption costs one textured quad per frame.
    const double scale = fc.sy;
    for (const TrackData& track : fc.timeline->subtitleTracks) {
        if (!track.enabled)
            continue;
        const ClipData* caption = captionAt(track, fc.frame);
        if (!caption)
            continue;
        CaptionPlacement placement;
        const TitleStyle style = resolveCaptionStyle(
            *caption, &track, caption->localFrame(fc.frame), fc.outSize, scale, &placement);
        const TitleTexture* t = titleTexture(style);
        if (!t)
            continue;
        const QRectF rect
            = t->rect.translated(captionOffset(t->rect, fc.outSize, style.align, placement));
        m_kit->bindTarget(target);
        m_kit->setBlendOver(true);
        m_kit->blit(t->texture, GlKit::quadToNdc(rect, fc.outSize));
    }
}

// ---- Title cache ----

const GlRenderer::TitleTexture* GlRenderer::titleTexture(const TitleStyle& style)
{
    ++m_useCounter;
    auto it = m_titles.find(style);
    if (it != m_titles.end()) {
        it->lastUse = m_useCounter;
        return &*it;
    }
    const TitleImage image = renderTitle(style);
    if (image.image.isNull())
        return nullptr;
    TitleTexture t;
    t.size = image.image.size();
    t.rect = image.rect;
    t.canvas = style.canvas;
    t.lastUse = m_useCounter;
    m_gl->glGenTextures(1, &t.texture);
    m_gl->glBindTexture(GL_TEXTURE_2D, t.texture);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(image.image.bytesPerLine() / 4));
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.size.width(), t.size.height(), 0, GL_BGRA,
        GL_UNSIGNED_BYTE, image.image.constBits());
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return &*m_titles.insert(style, t);
}

void GlRenderer::trimTitleCache()
{
    while (m_titles.size() > kTitleCacheSize) {
        auto oldest = m_titles.begin();
        for (auto it = m_titles.begin(); it != m_titles.end(); ++it) {
            if (it->lastUse < oldest->lastUse)
                oldest = it;
        }
        m_gl->glDeleteTextures(1, &oldest->texture);
        m_titles.erase(oldest);
    }
}

std::unique_ptr<RenderBackend> createGlBackend(QOffscreenSurface* surface)
{
    return std::make_unique<GlRenderer>(surface);
}

} // namespace kk::render
