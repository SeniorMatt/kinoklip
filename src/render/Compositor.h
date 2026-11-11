// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor — renders timeline frames on a dedicated render thread (OpenGL 3.3 core, with a
// QPainter fallback). The API used by the viewer/playback engine and the exporter.
#pragma once

#include "render/DecoderPool.h"
#include "render/RenderTypes.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>
#include <future>
#include <memory>

namespace kk {

/// Renders frames of a timeline snapshot: decode → crop → effects → transform → opacity →
/// transitions → composite (V1 at the bottom), see src/render/readme.md.
///
/// Threading model: the Compositor owns one render thread. That thread owns an offscreen
/// OpenGL 3.3 core context which shares objects with QOpenGLContext::globalShareContext()
/// (Qt::AA_ShareOpenGLContexts, set in main.cpp), the video decoder pool and all GL
/// resources. Requests from any thread are queued and executed in order; render() blocks,
/// renderAsync() returns a future. Several requests in flight let the GPU readback of frame N
/// overlap the rendering of frame N+1 (export should keep 2-3 renderAsync() calls queued).
///
/// Output for display (zero copy): request OutputTexture and draw RenderedFrame::texture in a
/// context of the share group after `glWaitSync(fence)`. Output for encoding: request
/// OutputImage (QImage ARGB32_Premultiplied, read back through pixel buffer objects).
///
/// Fallback: when no OpenGL 3.3 core context can be created (offscreen QPA, broken driver,
/// environment KINOKLIP_RENDER=cpu), a QPainter backend renders images only: transform,
/// opacity, crop, titles, solids and dissolves; effects are skipped with a warning.
///
/// Thread-safety: all public methods may be called from any thread, except the constructor
/// and destructor, which must run on the GUI thread (QOffscreenSurface requirement).
class Compositor
{
public:
    /// Construction options.
    struct Options
    {
        bool forceCpu = false; ///< use the QPainter backend even if OpenGL works
        /// Background work (proxies, render cache): the render thread and the decoder threads
        /// it starts yield to the playback threads when the cores are busy.
        bool background = false;
    };

    /// Starts the render thread and initialises the backend (blocks until ready; typically
    /// 20-100 ms for context creation + shader compilation). GUI thread only.
    explicit Compositor(Options options);
    Compositor();
    /// Finishes queued requests (their futures still complete) and stops the thread.
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    /// True if the GPU backend is active (textures available, effects rendered).
    [[nodiscard]] bool isGpu() const;
    /// Backend description for logs/about boxes, e.g. "OpenGL 4.6 (NVIDIA GeForce GTX 1650)".
    [[nodiscard]] QString backendDescription() const;

    /// Default media id → path table for requests that do not carry their own.
    void setMediaPaths(std::shared_ptr<const MediaPathMap> media);

    /// Queues a request. The future yields the frame (RenderedFrame::ok false on a bad request;
    /// never an exception).
    [[nodiscard]] std::future<RenderedFramePtr> renderAsync(RenderRequest request);

    /// Compositing page viewers: queues a render of node @p nodeId (0 = MediaOut) of the
    /// composition of clip @p clipId (a video clip of @p timeline's own tracks, e.g.
    /// Project::renderSnapshot(uid)) at comp frame @p compFrame, at @p outSize (empty =
    /// timeline resolution) scaled by @p quality. Same zero-copy texture / image output as
    /// renderAsync(); a missing clip, composition or node gives a transparent frame. Cheap
    /// enough to call on every scrub step: one decode per MediaIn plus one pass per node, no
    /// readback unless OutputImage is asked for. MediaIn nodes decode through @p media (nullptr
    /// = the table of setMediaPaths()) and @p proxies (nullptr = the camera originals), as a
    /// timeline render does; PlaybackEngine::renderCompositionNode() passes the engine's own.
    [[nodiscard]] std::future<RenderedFramePtr> renderCompositionNode(
        std::shared_ptr<const TimelineData> timeline, quint64 clipId, quint64 nodeId,
        int64_t compFrame, QSize outSize = {}, Quality quality = Quality::Full,
        unsigned outputs = OutputTexture, std::shared_ptr<const MediaPathMap> media = nullptr,
        std::shared_ptr<const ProxyMap> proxies = nullptr);

    /// Renders @p frame of @p timeline at @p outSize (empty = timeline resolution) scaled by
    /// @p quality, producing @p outputs (RenderOutput flags). Blocks until done.
    [[nodiscard]] RenderedFramePtr render(std::shared_ptr<const TimelineData> timeline,
        int64_t frame, QSize outSize, Quality quality = Quality::Full,
        unsigned outputs = OutputTexture);

    /// Counters of the decoder look-ahead (RenderRequest::prepare): measurements and tests.
    [[nodiscard]] render::DecoderPool::PrepareStats prepareStats() const;

    /// Convenience: render() with OutputImage and return the image (null on failure).
    [[nodiscard]] QImage renderImage(std::shared_ptr<const TimelineData> timeline, int64_t frame,
        QSize outSize = {}, Quality quality = Quality::Full);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace kk
