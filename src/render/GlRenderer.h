// SPDX-License-Identifier: GPL-3.0-or-later
// GlRenderer — the GPU RenderBackend: walks the tracks of a timeline frame, decodes the visible
// media (in parallel), and composites everything with OpenGL 3.3 core passes.
#pragma once

#include "media/VideoFrame.h"
#include "render/DecoderPool.h"
#include "render/GlComposition.h"
#include "render/GlEffects.h"
#include "render/GlGenerators.h"
#include "render/GlGrade.h"
#include "render/GlKit.h"
#include "render/GlMasks.h"
#include "render/GlOutput.h"
#include "render/GlRetime.h"
#include "render/GlTransitions.h"
#include "render/GlVideo.h"
#include "render/RenderBackend.h"
#include "render/TimelineEval.h"
#include "render/TitleRenderer.h"

#include <QHash>
#include <QThreadPool>

#include <deque>
#include <memory>
#include <vector>

class QOffscreenSurface;
class QOpenGLContext;

namespace kk::render {

/// GPU backend. Lives entirely on the render thread; its context stays current there for the
/// renderer's whole lifetime (no other code uses that thread), so no makeCurrent() per frame.
class GlRenderer final : public RenderBackend
{
public:
    explicit GlRenderer(QOffscreenSurface* surface);
    ~GlRenderer() override;

    bool initialize(QString* why) override;
    [[nodiscard]] QString description() const override { return m_description; }
    [[nodiscard]] bool isGpu() const override { return true; }
    [[nodiscard]] DecoderPool& decoders() override { return m_decoders; }
    void render(RenderJob& job) override;
    void finishPending(bool keepNewest) override;
    [[nodiscard]] bool hasPending() const override;
    void shutdown() override;

    /// Number of transition programs built so far (a fresh renderer has built only the
    /// fallback, cross_dissolve). Render thread only; for tests.
    [[nodiscard]] int builtTransitionCount() const { return int(m_transitions.size()); }
    /// Number of effect programs built so far. Render thread only; for tests.
    [[nodiscard]] int builtEffectCount() const { return int(m_effects.size()); }

private:
    /// The implementation of transition @p id, built on first use; nullptr when the id is not
    /// a GPU transition or its program failed to build (then the caller uses the fallback).
    GlTransition* transitionFor(const QString& id);
    /// The implementation of effect @p id, built on first use; nullptr when the id is not a
    /// GPU effect or its program failed to build (reported once).
    GlEffect* effectFor(const QString& id);

    /// Frame-wide values every clip draw needs.
    struct FrameContext
    {
        const TimelineData* timeline = nullptr; ///< the root or a nested timeline
        int64_t frame = 0; ///< frame of `timeline`
        QSize outSize;
        double sx = 1.0; ///< output pixels per timeline pixel, horizontally
        double sy = 1.0; ///< ... vertically
        const NestedTimelines* nested = nullptr; ///< the root snapshot's nested timelines
        int depth = 0; ///< nesting depth (0 = root)
        /// While an adjustment clip's track is drawn: the composite of the tracks below it
        /// (output-sized texture), the source of every adjustment clip. 0 otherwise.
        GLuint below = 0;
        bool subtitles = false; ///< draw the subtitle tracks (root timeline only)
        bool grades = true; ///< apply clip and timeline grades (false: RenderRequest::bypassGrades)
        bool dynamicZoom = true; ///< RenderRequest::dynamicZoom
    };
    /// A media clip decoded for the current frame. A clip inside a nested timeline can be
    /// shown at several times in one frame (two instances), so (clip, timeUs) is the key.
    struct DecodedClip
    {
        const ClipData* clip = nullptr;
        DecoderPool::Entry* entry = nullptr;
        int64_t timeUs = 0;
        int64_t nextTimeUs = 0; ///< source time of the next frame (decode-ahead)
        std::shared_ptr<const media::VideoFrame> frame;
        /// Proxy decoded instead of the original: the original's display size (crop units).
        QSizeF displayOverride;
        /// Retimed clips with Frame Blend / Optical Flow: the following source frame (own
        /// decoder, so both keep decoding forward), the process and the weight of frameB
        /// (0 = only `frame`; set by the decode when timeUs falls between two frames).
        RetimeProcess process = RetimeProcess::Nearest;
        DecoderPool::Entry* entryB = nullptr;
        std::shared_ptr<const media::VideoFrame> frameB;
        float weightB = 0.0f;
    };
    /// A nested timeline composited for the current frame (reused by every instance that
    /// shows the same frame at the same size).
    struct NestedComposite
    {
        const TimelineData* timeline = nullptr;
        int64_t frame = 0;
        QSize size;
        PooledTarget target;
    };
    /// What a clip draws, resolved for this frame (see drawClip()).
    struct ClipSource;
    /// A cached title texture.
    struct TitleTexture
    {
        GLuint texture = 0;
        QSize size;
        QRectF rect; ///< position inside the title canvas (output pixels of the raster)
        QSize canvas;
        quint64 lastUse = 0;
    };

    /// Queues the decode of @p clip at @p frame of @p tl (@p nextFrame: the frame playback
    /// asks for next); a Nested clip queues the clips visible in its timeline (recursive).
    void queueDecode(const ClipData* clip, int64_t frame, int64_t nextFrame, const TimelineData& tl,
        const NestedTimelines& nested, const MediaPathMap* media, int depth);
    /// queueDecode() of one frame (without the time effects' extra frames).
    void queueDecodeAt(const ClipData* clip, int64_t frame, int64_t nextFrame,
        const TimelineData& tl, const NestedTimelines& nested, const MediaPathMap* media,
        int depth);
    /// queueDecode() for every visible clip of @p tl at @p frame.
    void queueTimeline(const TimelineData& tl, int64_t frame, int64_t nextFrame,
        const NestedTimelines& nested, const MediaPathMap* media, int depth);
    void decodeQueued();
    /// Decodes @p d (worker thread): its frame, and for Frame Blend / Optical Flow the next
    /// source frame when the source time lies between two frames.
    static void decodeOne(DecodedClip& d);
    /// Starts decoding the next frame of every clip that plays forward (worker threads).
    void startDecodeAhead();
    const DecodedClip* decoded(const ClipData* clip, int64_t timeUs) const;
    /// Composites every video track of @p fc.timeline at @p fc.frame into a new target of
    /// @p fc.outSize (premultiplied, transparent where nothing is).
    PooledTarget compositeTimeline(const FrameContext& fc);
    /// The composite of nested timeline @p tl at @p frame and @p size (cached per frame).
    const RenderTarget* nestedComposite(
        const TimelineData& tl, int64_t frame, QSize size, const FrameContext& parent);

    void drawTrack(
        const TrackFrame& track, const FrameContext& fc, const RenderTarget& accumulator);
    /// How drawClip() combines the clip with the target.
    enum class ClipBlend {
        Over, ///< premultiplied over (every normal clip)
        /// Adjustment clips, pass 1: target *= 1 - coverage, where coverage is the clip's
        /// footprint (crop, masks, transform) times its opacity; effects are skipped.
        Erase,
        /// Adjustment clips, pass 2: target += processed picture × opacity.
        Add,
        /// Adjustment clips covering the whole frame unchanged (hasIdentityFootprint()): the
        /// processed layer overwrites the target (blending off).
        Replace
    };
    void drawClip(const ClipData& clip, const FrameContext& fc, const RenderTarget& target,
        ClipBlend blend = ClipBlend::Over);
    /// The layer path of a clip with time effects (Echo, Strobe): geometry and active effects
    /// of the frame drawn, reused to draw the clip at the other frames the effects read.
    struct LayerJob
    {
        const ClipData* clip = nullptr;
        const FrameContext* fc = nullptr;
        int64_t local = 0; ///< clip-local frame drawn
        double zoom = 1.0;
        QSize size; ///< layer size
        double scale = 1.0; ///< layer px per timeline px
        double padPx = 0.0;
        QSizeF content; ///< content size (timeline px)
        std::vector<GlEffect*> effects; ///< active effects (a copy of m_activeEffects)
        std::vector<const EffectInstance*> instances;
    };
    class JobHistory;
    /// Applies the first @p count effects of @p job at clip-local frame @p at to @p layer
    /// (each with a LayerHistory of the effects before it).
    void applyEffects(const LayerJob& job, int64_t at, size_t count, PooledTarget& layer);
    /// The clip's layer at clip-local frame @p at with the first @p count effects applied;
    /// empty if the frame's source is not available.
    PooledTarget renderLayerAt(const LayerJob& job, int64_t at, size_t count);
    /// Sets the GL blend function for @p blend (blending enabled).
    void applyBlend(ClipBlend blend);
    /// True if adjustment clip @p clip has an effect that changes the picture at @p local.
    [[nodiscard]] bool hasActiveEffect(const ClipData& clip, int64_t local);
    /// Draws the track frame @p tf showing an adjustment clip over @p accumulator (which holds
    /// the tracks below); may replace @p accumulator.
    void drawAdjustment(const TrackFrame& tf, const FrameContext& fc, PooledTarget& accumulator);
    /// What @p clip draws at @p localFrame. With @p useComposition (the default) a clip with
    /// a composition draws the composition's output (compositionTarget()); false gives the
    /// clip's own picture (its MediaIn).
    bool resolveSource(const ClipData& clip, int64_t localFrame, const FrameContext& fc,
        double maxZoomValue, ClipSource& out, bool useComposition = true);
    /// Evaluates node @p node (0 = MediaOut) of @p clip's composition at comp frame
    /// @p compFrame into a per-frame target of @p size (@p scale target px per timeline px of
    /// fc.timeline), kept until the end of the frame. nullptr if the clip has no composition.
    const RenderTarget* compositionTarget(const ClipData& clip, int64_t compFrame,
        const FrameContext& fc, double zoom, quint64 node, QSize size, double scale);
    /// Draws @p clip's own picture (the MediaIn of its composition, or a MediaIn reading
    /// another media pool item through its stand-in clip) at @p compFrame into @p target.
    bool drawCompositionInput(const ClipData& owner, const CompNode& node, int64_t compFrame,
        const FrameContext& fc, double zoom, const RenderTarget& target, QSize size, double scale);
    /// The stand-in clip of a MediaIn node that reads media pool item CompNode::mediaId
    /// (frame 0 of the comp = the item's start, speed 1), per frame; created on demand when
    /// @p create. nullptr if missing.
    const ClipData* compClip(const ClipData* owner, const CompNode& node, bool create);
    /// Queues the decodes of the MediaIn nodes of @p clip's composition at timeline frame
    /// @p frame that read other media pool items.
    void queueCompositionDecodes(const ClipData* clip, int64_t frame, int64_t nextFrame,
        const TimelineData& tl, const NestedTimelines& nested, const MediaPathMap* media,
        int depth);
    class CompHost;
    void drawSource(
        const ClipSource& src, const QTransform& quadToNdc, const QRectF& crop, float opacity);
    /// Renders generator clip @p clip into a per-frame target (cached for the frame).
    const RenderTarget* generatorTarget(
        const ClipData& clip, int64_t localFrame, const FrameContext& fc, double zoom);
    /// Draws the caption of every enabled subtitle track at fc.frame over @p target.
    void drawSubtitles(const FrameContext& fc, const RenderTarget& target);
    /// Frame blend / optical flow of a decoded clip with a frameB: converts both frames to RGB
    /// at display orientation (at most @p maxSize) and returns the in-between picture's
    /// texture, kept alive until the end of the frame. 0 on failure.
    GLuint retimedPicture(const DecodedClip& d, QSize maxSize);
    const TitleTexture* titleTexture(const TitleStyle& style);
    void trimTitleCache();

    QOffscreenSurface* m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    GlFunctions* m_gl = nullptr;
    std::unique_ptr<GlKit> m_kit;
    std::unique_ptr<GlOutput> m_output;
    GlVideo m_video;
    GlMasks m_masks; ///< clip mask pass (applied on the layer path)
    GlGrade m_grade; ///< Color page grade pass (clip layers and the timeline composite)
    GlGenerators m_generators; ///< generator patterns
    GlComposition m_comp; ///< Compositing page node graphs
    /// Stand-in clips of MediaIn nodes reading other media pool items (per frame; a deque
    /// keeps the addresses the decode list refers to stable).
    struct CompClip
    {
        const ClipData* owner = nullptr;
        quint64 node = 0;
        ClipData clip;
    };
    std::deque<CompClip> m_compClips;
    GlRetime m_retime;
    bool m_retimeOk = false; ///< retime shaders built (else every clip uses Nearest)
    std::vector<PooledTarget> m_retimeTargets; ///< per-frame in-between pictures
    QString m_description;

    /// Effects built so far (an effect builds when first used, see effectFor()).
    QHash<QString, std::shared_ptr<GlEffect>> m_effects;
    QSet<QString> m_failedEffects; ///< ids whose program did not build (logged once)
    /// Transitions built so far (a transition builds when first drawn, see transitionFor()).
    QHash<QString, std::shared_ptr<GlTransition>> m_transitions;
    QSet<QString> m_failedTransitions; ///< ids whose program did not build (logged once)
    GlTransition* m_fallbackTransition = nullptr; ///< cross_dissolve for unknown ids

    DecoderPool m_decoders;
    const ProxyMap* m_proxies = nullptr; ///< RenderRequest::proxies of the current frame
    QThreadPool m_decodeThreads; ///< parallel decoding of the clips visible in one frame
    std::vector<DecodedClip> m_decoded; ///< per-frame scratch (capacity reused)
    qint64 m_uploadNs = 0; ///< plane upload time of the current frame (statistics)
    /// Per-frame scratch of the track walk, one list per nesting depth (a nested timeline is
    /// composited while its parent's list is still in use).
    std::vector<std::vector<TrackFrame>> m_trackFrames;
    std::vector<NestedComposite> m_nested; ///< per-frame nested composites (capacity reused)
    std::vector<PooledTarget> m_generated; ///< per-frame generator pictures (capacity reused)
    std::vector<GlEffect*> m_activeEffects; ///< per-clip scratch
    std::vector<const EffectInstance*> m_activeInstances; ///< parallel to m_activeEffects

    QHash<TitleStyle, TitleTexture> m_titles;
    quint64 m_useCounter = 0;
    QSet<QString> m_warnedEffects; ///< unknown effect ids already reported
};

} // namespace kk::render
