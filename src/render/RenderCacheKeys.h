// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCacheKeys — pure functions that cut a timeline into render cache chunks, give each
// chunk a content key (an "edit hash" of everything that can change its pictures) and decide
// which chunks are worth caching (Smart / User mode). The key is the cache file's name, so
// invalidation is by construction: an edit changes the key, the old file is simply never
// looked up again (and eventually evicted).
#pragma once

#include "core/MediaLinks.h"
#include "render/RenderTypes.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <cstdint>
#include <vector>

namespace kk {

struct MediaPoolState;

/// Bump when the renderer changes the pictures it produces for the same input (shader fix,
/// new default...): every cached chunk then gets a new key. The application version is part
/// of the key as well, so a release never reuses an older release's chunks.
inline constexpr int kRenderCacheVersion = 1;

/// Why a chunk counts as heavy (bit flags).
enum CacheReason : unsigned {
    CacheTransition = 1u << 0, ///< a transition is (or may be) visible
    CacheComposite = 1u << 1, ///< two or more video tracks have clips in the chunk
    CacheEffects = 1u << 2, ///< a clip with enabled effects (or masks)
    CacheNested = 1u << 3, ///< a compound clip / nested timeline
    CacheRetime = 1u << 4, ///< retimed: speed change, reverse, freeze or retime curve
    CacheUserClip = 1u << 5, ///< a clip marked "Render Cache Clip Output"
};

/// One render cache chunk: timeline frames [start, end).
struct CacheChunk
{
    int64_t start = 0;
    int64_t end = 0;
    QByteArray key; ///< 40 hex digits (sha1); also the file name "<key>.mov"
    unsigned reasons = 0; ///< CacheReason flags
    bool wanted = false; ///< should be cached in the current mode (Smart / User rules)
};

/// Options of a key computation.
struct CacheKeyOptions
{
    RenderCacheMode mode = RenderCacheMode::Smart;
    bool userTransitions = false; ///< User mode: also cache transitions
    bool userComposites = false; ///< User mode: also cache composites
    QString format = QStringLiteral("dnxhr_lb"); ///< cache codec id (part of the key)
    int chunkFrames = 0; ///< 0 = one second of frames (rounded fps), at least 1
};

/// media id → bytes identifying the original file's content: absolute path, size and
/// modification time (SourceFingerprint). Stats every file (GUI thread; build it when the
/// pool changes, not per paint). Missing files give "offline:<path>".
using MediaKeyMap = QHash<QString, QByteArray>;
[[nodiscard]] MediaKeyMap makeMediaKeys(const MediaPoolState& pool);

/// Number of frames per chunk for @p fps and @p options.
[[nodiscard]] int cacheChunkFrames(Rational fps, const CacheKeyOptions& options);

/// Chunks of @p timeline from frame 0 to the end of its last video clip, on a grid of
/// cacheChunkFrames() frames.
///
/// The key of a chunk covers, conservatively (a false "changed" only costs a re-render):
/// the cache version, application version, cache format, timeline size and frame rate, the
/// chunk range, and for every video track (in order) its enabled flag and the complete JSON
/// form of every clip whose influence range overlaps the chunk, where the influence range
/// is the clip extended on both sides by the longest transition on its track (transitions
/// show the neighbours' handles). Fields that cannot change pictures are left out (name,
/// colour label, markers, audio parameters, link group, clip id, the cacheOutput flag). Media
/// clips add their MediaKeyMap entry (relink, replaced file); Nested clips add the JSON of
/// every timeline they reach and those timelines' media keys. Because whole clip JSON is
/// hashed, fields added later (masks, retime curves...) are covered automatically as soon
/// as they are saved with the project.
[[nodiscard]] std::vector<CacheChunk> renderCacheChunks(
    const TimelineData& timeline, const MediaKeyMap& mediaKeys, const CacheKeyOptions& options);

/// Builds the view the Compositor uses: the chunks of @p chunks whose key is in @p present,
/// each as a one-clip timeline showing "<directory>/<key>.mov" (frame `start` = the file's
/// first frame). @p snapshot is the timeline snapshot the keys were computed from.
[[nodiscard]] std::shared_ptr<const RenderCacheView> makeRenderCacheView(
    std::shared_ptr<const TimelineData> snapshot, const std::vector<CacheChunk>& chunks,
    const QSet<QByteArray>& present, const QString& directory);

/// File of the chunk with key @p key in @p directory.
[[nodiscard]] QString renderCacheFile(const QString& directory, const QByteArray& key);

} // namespace kk
