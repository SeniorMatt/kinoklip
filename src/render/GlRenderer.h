// SPDX-License-Identifier: GPL-3.0-or-later
// GlRenderer — the GPU RenderBackend: walks the tracks of a timeline frame, decodes the visible
// media (in parallel), and composites everything with OpenGL 3.3 core passes.
#pragma once

#include "media/VideoFrame.h"
#include "render/DecoderPool.h"
#include "render/GlEffects.h"
#include "render/GlKit.h"
#include "render/GlOutput.h"
#include "render/GlTransitions.h"
#include "render/GlVideo.h"
#include "render/RenderBackend.h"
#include "render/TimelineEval.h"
#include "render/TitleRenderer.h"

#include <QHash>
#include <QThreadPool>

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
    void render(RenderJob& job) override;
    void finishPending(bool keepNewest) override;
    [[nodiscard]] bool hasPending() const override;
    void shutdown() override;

private:
    /// Frame-wide values every clip draw needs.
    struct FrameContext
    {
        const TimelineData* timeline = nullptr;
        int64_t frame = 0;
        QSize outSize;
        double sx = 1.0; ///< output pixels per timeline pixel, horizontally
        double sy = 1.0; ///< ... vertically
    };
    /// A media clip decoded for the current frame.
    struct DecodedClip
    {
        const ClipData* clip = nullptr;
        DecoderPool::Entry* entry = nullptr;
        int64_t timeUs = 0;
        std::shared_ptr<const media::VideoFrame> frame;
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

    void queueDecode(
        const ClipData* clip, int64_t frame, const TimelineData& tl, const MediaPathMap* media);
    void decodeQueued();
    const DecodedClip* decoded(const ClipData* clip) const;

    void drawTrack(
        const TrackFrame& track, const FrameContext& fc, const RenderTarget& accumulator);
    void drawClip(const ClipData& clip, const FrameContext& fc, const RenderTarget& target);
    bool resolveSource(const ClipData& clip, int64_t localFrame, const FrameContext& fc,
        double maxZoomValue, ClipSource& out);
    void drawSource(
        const ClipSource& src, const QTransform& quadToNdc, const QRectF& crop, float opacity);
    const TitleTexture* titleTexture(const TitleStyle& style);
    void trimTitleCache();

    QOffscreenSurface* m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    GlFunctions* m_gl = nullptr;
    std::unique_ptr<GlKit> m_kit;
    std::unique_ptr<GlOutput> m_output;
    GlVideo m_video;
    QString m_description;

    QHash<QString, std::shared_ptr<GlEffect>> m_effects; ///< descriptor id → implementation
    QHash<QString, std::shared_ptr<GlTransition>> m_transitions;
    GlTransition* m_fallbackTransition = nullptr; ///< cross_dissolve for unknown ids

    DecoderPool m_decoders;
    QThreadPool m_decodeThreads; ///< parallel decoding of the clips visible in one frame
    std::vector<DecodedClip> m_decoded; ///< per-frame scratch (capacity reused)
    std::vector<TrackFrame> m_trackFrames; ///< per-frame scratch
    std::vector<GlEffect*> m_activeEffects; ///< per-clip scratch
    std::vector<const EffectInstance*> m_activeInstances; ///< parallel to m_activeEffects

    QHash<TitleStyle, TitleTexture> m_titles;
    quint64 m_useCounter = 0;
    QSet<QString> m_warnedEffects; ///< unknown effect ids already reported
};

} // namespace kk::render
