// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCacheKeys — chunk grid, edit hash per chunk, Smart / User selection, cache view.
#include "render/RenderCacheKeys.h"

#include "core/CompOps.h"
#include "core/MediaItem.h"
#include "core/Retime.h"
#include "core/Serialization.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace kk {

namespace {

/// Nested timelines deeper than this are not followed (the renderer stops at 16 as well).
constexpr int kMaxNestingDepth = 16;

/// Clip JSON keys that cannot change the clip's pictures (left out of the hash so renaming
/// or recolouring a clip keeps its cache).
const char* const kIgnoredClipKeys[]
    = {"id", "name", "label", "markers", "audio", "linkGroup", "cacheOutput"};

QByteArray canonicalJson(const QJsonObject& o)
{
    // QJsonObject keeps keys sorted, so the compact form is canonical.
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

/// Longest transition on @p track (frames): how far a clip's data reaches beyond its edges.
int64_t transitionMargin(const TrackData& track)
{
    int64_t m = 0;
    for (const ClipData& c : track.clips) {
        if (c.transitionIn)
            m = std::max(m, c.transitionIn->duration);
        if (c.transitionOut)
            m = std::max(m, c.transitionOut->duration);
    }
    return m;
}

/// Reasons of @p clip that do not depend on the chunk; @p json = clipToJson(clip).
unsigned staticClipReasons(const ClipData& clip, const QJsonObject& json)
{
    unsigned r = 0;
    if (clip.kind == ClipKind::Nested)
        r |= CacheNested;
    if (retime::isRetimed(clip)) // speed change, reverse, freeze or a retime curve
        r |= CacheRetime;
    // Masks live in the clip JSON (masks branch); a non-empty "masks" value counts as an
    // effect. Looking at the JSON keeps this file independent of that field.
    const QJsonValue masks = json.value(QLatin1String("masks"));
    const bool hasMasks = masks.isArray() ? !masks.toArray().isEmpty() : !masks.isUndefined();
    if (hasMasks
        || std::any_of(clip.effects.begin(), clip.effects.end(),
            [](const EffectInstance& e) { return e.enabled; }))
        r |= CacheEffects;
    // A composition (Compositing page) costs like effects: Smart mode caches it.
    if (clip.composition && !compops::isPassThrough(*clip.composition))
        r |= CacheEffects;
    if (clip.cacheOutput)
        r |= CacheUserClip;
    return r;
}

/// CacheTransition if a transition of @p clip may be visible in [start, end).
unsigned transitionReason(const ClipData& clip, int64_t start, int64_t end)
{
    // A transition window lies within one duration of the cut on either side (whatever the
    // alignment), so this test is a superset of the exact window.
    auto near = [start, end](int64_t cut, int64_t d) { return cut - d < end && cut + d > start; };
    if ((clip.transitionIn && near(clip.start, clip.transitionIn->duration))
        || (clip.transitionOut && near(clip.end(), clip.transitionOut->duration)))
        return CacheTransition;
    return 0;
}

/// Hash of the picture-relevant part of one clip (cached per clip within one computation).
class ClipHasher
{
public:
    ClipHasher(const TimelineData& root, const MediaKeyMap& mediaKeys)
        : m_root(root)
        , m_mediaKeys(mediaKeys)
    { }

    /// Picture-relevant hash of @p clip (20 bytes).
    const QByteArray& hash(const ClipData& clip) { return entry(clip).hash; }
    /// Reasons that do not depend on the chunk range (effects, masks, nested, retime, user).
    unsigned staticReasons(const ClipData& clip) { return entry(clip).reasons; }

private:
    struct Entry
    {
        QByteArray hash;
        unsigned reasons = 0;
    };

    const Entry& entry(const ClipData& clip)
    {
        auto it = m_clips.find(&clip);
        if (it != m_clips.end())
            return it->second;
        QCryptographicHash h(QCryptographicHash::Sha1);
        QJsonObject json = serialization::clipToJson(clip);
        Entry e;
        e.reasons = staticClipReasons(clip, json);
        for (const char* key : kIgnoredClipKeys)
            json.remove(QLatin1String(key));
        if (clip.composition) {
            // Only what changes the picture: node names, positions, notes and the node
            // editor's view do not invalidate the cache. MediaIns reading other media pool
            // items add those items' keys.
            json[QLatin1String("composition")]
                = serialization::compositionToJson(*clip.composition, true);
            for (const CompNode& n : clip.composition->nodes) {
                if (!n.mediaId.isEmpty())
                    h.addData(m_mediaKeys.value(n.mediaId, QByteArrayLiteral("unknown-media")));
            }
        }
        h.addData(canonicalJson(json));
        if (clip.kind == ClipKind::Media) {
            h.addData(m_mediaKeys.value(clip.mediaId, QByteArrayLiteral("unknown-media")));
        } else if (clip.kind == ClipKind::Nested) {
            QSet<quint64> seen;
            addNested(h, clip.nestedUid, 0, seen);
        }
        e.hash = h.result();
        return m_clips.emplace(&clip, std::move(e)).first->second;
    }

    /// Adds the JSON of nested timeline @p uid, its media keys and everything it reaches.
    void addNested(QCryptographicHash& h, quint64 uid, int depth, QSet<quint64>& seen)
    {
        h.addData(QByteArrayLiteral("nested:"));
        h.addData(QByteArray::number(qulonglong(uid)));
        const TimelineData* n = m_root.nested.find(uid);
        if (!n || depth >= kMaxNestingDepth || seen.contains(uid)) {
            h.addData(QByteArrayLiteral(":missing"));
            return;
        }
        seen.insert(uid);
        h.addData(canonicalJson(serialization::timelineToJson(*n)));
        for (const TrackData& t : n->videoTracks) {
            for (const ClipData& c : t.clips) {
                if (c.kind == ClipKind::Media)
                    h.addData(m_mediaKeys.value(c.mediaId, QByteArrayLiteral("unknown-media")));
                else if (c.kind == ClipKind::Nested)
                    addNested(h, c.nestedUid, depth + 1, seen);
            }
        }
    }

    const TimelineData& m_root;
    const MediaKeyMap& m_mediaKeys;
    std::unordered_map<const ClipData*, Entry> m_clips;
};

bool wantedIn(const CacheKeyOptions& o, unsigned reasons)
{
    switch (o.mode) {
    case RenderCacheMode::None:
        return false;
    case RenderCacheMode::Smart:
        return reasons != 0; // every heavy reason, and clips the user marked as well
    case RenderCacheMode::User:
        return (reasons & CacheUserClip) != 0
            || (o.userTransitions && (reasons & CacheTransition) != 0)
            || (o.userComposites && (reasons & CacheComposite) != 0);
    }
    return false;
}

} // namespace

MediaKeyMap makeMediaKeys(const MediaPoolState& pool)
{
    MediaKeyMap keys;
    keys.reserve(pool.items.size());
    for (const MediaItem& item : pool.items) {
        if (item.isTimeline())
            continue;
        const QString path = QFileInfo(item.path).absoluteFilePath();
        const SourceFingerprint f = SourceFingerprint::of(path);
        QByteArray k = f.isValid() ? QByteArrayLiteral("file:") : QByteArrayLiteral("offline:");
        k += path.toUtf8();
        if (f.isValid()) {
            k += ':' + QByteArray::number(f.size) + ':' + QByteArray::number(f.mtimeMs);
        }
        keys.insert(item.id, k);
    }
    return keys;
}

int cacheChunkFrames(Rational fps, const CacheKeyOptions& options)
{
    if (options.chunkFrames > 0)
        return options.chunkFrames;
    const double rate = fps.isValid() ? double(fps.num) / fps.den : 25.0;
    return std::max(1, int(std::lround(rate)));
}

std::vector<CacheChunk> renderCacheChunks(
    const TimelineData& timeline, const MediaKeyMap& mediaKeys, const CacheKeyOptions& options)
{
    std::vector<CacheChunk> chunks;
    int64_t endFrame = 0;
    for (const TrackData& t : timeline.videoTracks) {
        if (!t.clips.empty())
            endFrame = std::max(endFrame, t.clips.back().end());
    }
    if (endFrame <= 0)
        return chunks;
    const int64_t step = cacheChunkFrames(timeline.fps, options);

    // Everything every chunk shares.
    QCryptographicHash header(QCryptographicHash::Sha1);
    header.addData(QByteArrayLiteral("kinoklip-render-cache:"));
    header.addData(QByteArray::number(kRenderCacheVersion));
    header.addData(QByteArrayLiteral(":" KK_APP_VERSION ":"));
    header.addData(options.format.toUtf8());
    header.addData(':' + QByteArray::number(timeline.width) + 'x'
        + QByteArray::number(timeline.height) + '@' + QByteArray::number(timeline.fps.num) + '/'
        + QByteArray::number(timeline.fps.den));
    // The timeline's default retime process (Frame Blend / Optical Flow) changes the pictures
    // of every retimed clip that follows the project default. The clips' own retime state
    // (speed, curve, process, pitch) is part of their JSON.
    header.addData(QByteArrayLiteral(":retime:") + QByteArray::number(int(timeline.retimeProcess)));
    const QByteArray headerHash = header.result();

    ClipHasher hasher(timeline, mediaKeys);
    std::vector<int64_t> margins;
    margins.reserve(timeline.videoTracks.size());
    for (const TrackData& t : timeline.videoTracks)
        margins.push_back(transitionMargin(t));

    chunks.reserve(size_t((endFrame + step - 1) / step));
    for (int64_t start = 0; start < endFrame; start += step) {
        const int64_t end = std::min(endFrame, start + step);
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(headerHash);
        h.addData(QByteArray::number(qlonglong(start)) + '-' + QByteArray::number(qlonglong(end)));
        unsigned reasons = 0;
        int tracksWithPictures = 0;
        for (size_t ti = 0; ti < timeline.videoTracks.size(); ++ti) {
            const TrackData& track = timeline.videoTracks[ti];
            h.addData(QByteArrayLiteral("|track:") + QByteArray::number(qulonglong(ti))
                + (track.enabled ? QByteArrayLiteral(":on") : QByteArrayLiteral(":off")));
            const int64_t m = margins[ti];
            // First clip whose influence range [start - m, end + m) can reach the chunk.
            auto it = std::lower_bound(track.clips.begin(), track.clips.end(), start - m,
                [](const ClipData& c, int64_t f) { return c.end() <= f; });
            bool pictures = false;
            for (; it != track.clips.end() && it->start - m < end; ++it) {
                h.addData(hasher.hash(*it));
                if (!track.enabled || !it->enabled)
                    continue;
                // A transition reaches into the chunk even when its clip starts after it
                // (centred dissolve: the incoming clip shows before its start).
                reasons |= transitionReason(*it, start, end);
                if (it->start >= end || it->end() <= start)
                    continue; // hashed (it may influence a neighbour), but not visible here
                pictures = true;
                reasons |= hasher.staticReasons(*it);
            }
            if (pictures)
                ++tracksWithPictures;
        }
        if (tracksWithPictures >= 2)
            reasons |= CacheComposite;
        CacheChunk c;
        c.start = start;
        c.end = end;
        c.key = h.result().toHex();
        c.reasons = reasons;
        c.wanted = wantedIn(options, reasons);
        chunks.push_back(std::move(c));
    }
    return chunks;
}

QString renderCacheFile(const QString& directory, const QByteArray& key)
{
    return directory + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".mov");
}

std::shared_ptr<const RenderCacheView> makeRenderCacheView(
    std::shared_ptr<const TimelineData> snapshot, const std::vector<CacheChunk>& chunks,
    const QSet<QByteArray>& present, const QString& directory)
{
    auto view = std::make_shared<RenderCacheView>();
    if (!snapshot)
        return view;
    for (const CacheChunk& c : chunks) {
        if (!present.contains(c.key))
            continue;
        // A one-clip timeline showing the chunk file: same size and rate as the real one, the
        // clip covers [start, end) and starts at the file's first frame, default transform
        // (a timeline-sized picture fitted into the frame = identity).
        auto tl = std::make_shared<TimelineData>();
        tl->uid = snapshot->uid;
        tl->fps = snapshot->fps;
        tl->width = snapshot->width;
        tl->height = snapshot->height;
        TrackData track;
        track.id = tl->allocateId();
        track.type = TrackType::Video;
        ClipData clip;
        clip.id = tl->allocateId();
        clip.kind = ClipKind::Media;
        clip.mediaId = QStringLiteral("render-cache:") + QString::fromLatin1(c.key);
        clip.start = c.start;
        clip.duration = c.end - c.start;
        clip.sourceInUs = 0;
        clip.sourceDurationUs = framesToUs(clip.duration, snapshot->fps);
        track.clips.push_back(std::move(clip));
        tl->videoTracks.push_back(std::move(track));
        // Chunks are rendered without captions (ExportJob, subtitle export off); the viewer
        // still draws the timeline's subtitle tracks over the cached picture.
        tl->subtitleTracks = snapshot->subtitleTracks;
        auto media = std::make_shared<MediaPathMap>();
        media->insert(tl->videoTracks[0].clips[0].mediaId, renderCacheFile(directory, c.key));
        view->chunks.push_back({c.start, c.end, std::move(tl), std::move(media)});
    }
    view->snapshot = std::move(snapshot);
    return view;
}

} // namespace kk
