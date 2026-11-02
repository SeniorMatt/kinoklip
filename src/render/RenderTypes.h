// SPDX-License-Identifier: GPL-3.0-or-later
// RenderTypes — value types shared by the Compositor API and its consumers (viewer, playback,
// export): render quality, media lookup table, render requests and rendered frames.
#pragma once

#include "core/TimelineData.h"

#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace kk {

struct MediaPoolState;

/// Preview quality: the frame is rendered at a fraction of the requested size.
/// The viewer scales the (smaller) result up; export always uses Full.
enum class Quality {
    Full, ///< requested size
    Half, ///< 1/2 width and height (1/4 of the pixels)
    Quarter ///< 1/4 width and height
};

/// Linear scale factor of @p quality (1, 0.5, 0.25).
[[nodiscard]] constexpr double qualityFactor(Quality quality)
{
    return quality == Quality::Full ? 1.0 : quality == Quality::Half ? 0.5 : 0.25;
}

/// Size actually rendered for @p requested at @p quality (never smaller than 1x1).
[[nodiscard]] QSize scaledRenderSize(QSize requested, Quality quality);

/// MediaItem::id → absolute file path. ClipData only stores a media id; the renderer and the
/// audio mixer resolve it through this table. Build it on the GUI thread with
/// makeMediaPathMap() and hand the immutable shared pointer to render/audio threads.
using MediaPathMap = QHash<QString, QString>;

/// Builds the id → path table of a media pool (GUI thread; O(items)).
[[nodiscard]] std::shared_ptr<const MediaPathMap> makeMediaPathMap(const MediaPoolState& pool);

/// A proxy (or optimized media) file that stands in for a media item's video.
///
/// Proxies are *display-equivalent*: upright, square pixels, the same display aspect as the
/// original, any size (src/export/ProxyJob). The renderer decodes `path` but keeps the
/// geometry of the original: `displaySize` is the original's upright display size in pixels
/// (the unit of the crop parameters and of the fit into the timeline frame), so a clip looks
/// the same with or without its proxy, only less detailed.
struct ProxyRef
{
    QString path;
    QSizeF displaySize; ///< original display size (after SAR and rotation), pixels
};

/// MediaItem::id → substitute video file. Only the items listed use a substitute; audio
/// always comes from the MediaPathMap (AudioMixer). Built on the GUI thread
/// (src/export/ProxyResolver), immutable once shared.
using ProxyMap = QHash<QString, ProxyRef>;

/// One chunk of the render cache that is on disk, as seen by the Compositor (see
/// render/RenderCacheKeys.h): timeline frames [start, end) are shown from a single media clip
/// `timeline` (frame `start` = the file's first frame) with `media` as its path table.
struct RenderCacheChunk
{
    int64_t start = 0;
    int64_t end = 0;
    std::shared_ptr<const TimelineData> timeline;
    std::shared_ptr<const MediaPathMap> media;
};

/// The cached chunks of ONE timeline snapshot (immutable). The Compositor uses them only for
/// requests whose RenderRequest::timeline is exactly `snapshot` (pointer identity): any edit
/// makes a new snapshot, so a stale view can never show outdated pictures.
struct RenderCacheView
{
    std::shared_ptr<const TimelineData> snapshot;
    std::vector<RenderCacheChunk> chunks; ///< sorted by start, non-overlapping

    /// Chunk containing @p frame, or nullptr (binary search).
    [[nodiscard]] const RenderCacheChunk* find(int64_t frame) const;
};

/// What a render request should produce (bit flags, combine with |).
enum RenderOutput : unsigned {
    /// An RGBA8 GL texture in the shared context group, for zero-copy display
    /// (RenderedFrame::texture + RenderedFrame::fence). Ignored by the CPU fallback.
    OutputTexture = 1u << 0,
    /// A QImage (Format_ARGB32_Premultiplied) read back from the GPU, for export/tests.
    /// The CPU fallback always produces the image.
    OutputImage = 1u << 1,
};

/// One request for the Compositor.
struct RenderRequest
{
    std::shared_ptr<const TimelineData> timeline; ///< immutable snapshot (Timeline::snapshot())
    /// Media id → path table; nullptr = the table given to Compositor::setMediaPaths().
    std::shared_ptr<const MediaPathMap> media;
    int64_t frame = 0; ///< timeline frame (PLAN §5.1)
    QSize outSize; ///< output size in pixels; empty = timeline resolution
    Quality quality = Quality::Full;
    unsigned outputs = OutputTexture; ///< RenderOutput flags
    /// Optional cancellation flag: when it is true by the time the render thread picks the
    /// request up, the request is skipped and completes with `ok == false, cancelled == true`
    /// (a request already being rendered finishes normally). Lets playback drop its stale
    /// look-ahead on a seek instead of waiting for it.
    std::shared_ptr<const std::atomic<bool>> cancelled = nullptr;
    /// Draw the captions of the enabled subtitle tracks over the picture (viewer; export
    /// with "burn into video"). Only the root timeline's subtitle tracks are drawn.
    bool subtitles = true;
    /// Video substitutes (proxy / optimized media); nullptr = camera originals (export).
    std::shared_ptr<const ProxyMap> proxies = nullptr;
    /// Render cache of `timeline`; nullptr or a view of another snapshot = render normally.
    /// A frame inside a cached chunk is rendered from the chunk's file instead (the request is
    /// rewritten by Compositor::renderAsync(); RenderedFrame::fromCache tells).
    std::shared_ptr<const RenderCacheView> renderCache = nullptr;
};

/// Result of a render request. Immutable once handed out; share it as RenderedFramePtr.
///
/// Pixel convention (texture and image alike): premultiplied alpha, gamma-encoded (sRGB-ish)
/// RGB, row 0 = TOP of the picture. Areas no clip covers are transparent black, so
/// "premultiplied over black" (what export wants) is simply the RGB channels.
///
/// Texture lifetime: `texture` belongs to the compositor's pool and is NOT reused while this
/// object is alive. Keep the RenderedFramePtr until the draw that samples it has been
/// submitted (e.g. until the next frame replaces it), and never past the Compositor's
/// lifetime.
struct RenderedFrame
{
    int64_t frame = 0; ///< timeline frame that was rendered
    QSize size; ///< pixel size of texture/image (requested size scaled by Quality)
    bool ok = false; ///< false: invalid or cancelled request — everything else empty
    bool cancelled = false; ///< skipped because RenderRequest::cancelled was set
    /// GL_TEXTURE_2D name (GL_RGBA8) in the shared context group, or 0 (CPU fallback, or
    /// OutputTexture not requested). Bind it in any context that shares with
    /// QOpenGLContext::globalShareContext().
    unsigned texture = 0;
    /// GLsync fence (cast to GLsync) signalled when the texture is complete. The consumer calls
    /// `glWaitSync(fence, 0, GL_TIMEOUT_IGNORED)` in its own context before sampling. Owned by
    /// the compositor — never delete it. nullptr when there is no texture.
    void* fence = nullptr;
    QImage image; ///< ARGB32_Premultiplied when OutputImage was requested (or CPU fallback)
    double renderMs = 0.0; ///< wall-clock time the render thread spent on this frame
    // Stages of renderMs (render-thread wall clock; GPU work runs asynchronously):
    double decodeMs = 0.0; ///< decoding every visible clip (incl. hardware frame download)
    double uploadMs = 0.0; ///< plane texture uploads
    double compositeMs = 0.0; ///< drawing layers, effects, transitions (excluding uploads)
    double outputMs = 0.0; ///< output slot + fence (+ readback for OutputImage)
    bool fromCache = false; ///< shown from the render cache (RenderRequest::renderCache)
};

/// Shared handle to a rendered frame (releasing the last reference recycles the texture).
using RenderedFramePtr = std::shared_ptr<const RenderedFrame>;

} // namespace kk
