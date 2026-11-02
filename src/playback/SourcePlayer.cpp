// SPDX-License-Identifier: GPL-3.0-or-later
// SourcePlayer — one media item on a private timeline.
#include "playback/SourcePlayer.h"

#include "core/Timeline.h"
#include "render/Compositor.h"

#include <cmath>

namespace kk {

namespace {
constexpr int kAudioOnlyWidth
    = 1920; ///< picture size of the (black) source timeline for audio files
constexpr int kAudioOnlyHeight = 1080;
} // namespace

TimelineData makeSourceTimeline(const MediaItem& item)
{
    const media::MediaInfo& info = item.info;
    const Rational fps = info.frameRate.isValid() ? info.frameRate : SourcePlayer::kFallbackFps;
    int width = info.displayWidth > 0 ? info.displayWidth : info.width;
    int height = info.displayHeight > 0 ? info.displayHeight : info.height;
    if (width <= 0 || height <= 0) {
        width = kAudioOnlyWidth;
        height = kAudioOnlyHeight;
    }
    TimelineData tl;
    tl.name = item.name;
    tl.fps = fps;
    tl.width = width;
    tl.height = height;
    tl.videoTracks.resize(1);
    tl.audioTracks.resize(1);
    tl.videoTracks[0].id = tl.allocateId();
    tl.audioTracks[0].id = tl.allocateId();

    // Whole media, rounded up to whole frames (the last partial frame stays reachable).
    int64_t frames = int64_t(SourcePlayer::kStillSeconds) * fps.num / fps.den;
    if (!info.isStillImage && info.durationUs > 0) {
        frames = int64_t(std::ceil(double(info.durationUs) * fps.num / (1e6 * fps.den) - 1e-6));
    }
    frames = std::max<int64_t>(1, frames);

    ClipData clip;
    clip.kind = ClipKind::Media;
    clip.mediaId = item.id;
    clip.name = item.name;
    clip.start = 0;
    clip.duration = frames;
    clip.sourceInUs = 0;
    clip.sourceDurationUs = info.isStillImage ? 0 : info.durationUs;
    clip.linkGroup = 1;
    if (info.hasVideo) {
        clip.id = tl.allocateId();
        tl.videoTracks[0].clips.push_back(clip);
    }
    if (info.hasAudio) {
        clip.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(clip);
    }
    return tl;
}

SourcePlayer::SourcePlayer(std::shared_ptr<Compositor> compositor, QObject* parent)
    : QObject(parent)
{
    PlaybackEngine::Options options;
    options.compositor = std::move(compositor);
    init(std::move(options));
}

SourcePlayer::SourcePlayer(PlaybackEngine::Options options, QObject* parent)
    : QObject(parent)
{
    init(std::move(options));
}

SourcePlayer::~SourcePlayer()
{
    // The engine must let go of the timeline before it is destroyed (both are children; make
    // the order explicit).
    m_engine->setTimeline(nullptr);
}

void SourcePlayer::init(PlaybackEngine::Options options)
{
    m_timeline = new Timeline(this);
    m_engine = new PlaybackEngine(std::move(options), this);
    connect(m_timeline, &Timeline::changed, this, &SourcePlayer::marksChanged);
}

Rational SourcePlayer::fps() const
{
    return m_timeline->data().fps;
}

void SourcePlayer::remember()
{
    if (hasMedia()) {
        m_memory.insert(m_item.id, Memory {markIn(), markOut(), m_engine->position(), fps()});
    }
}

void SourcePlayer::setMedia(const MediaItem* item)
{
    const QString newId = item ? item->id : QString();
    if (newId == m_item.id && (!item || item->path == m_item.path)) {
        return;
    }
    m_engine->stop();
    remember();
    if (!item) {
        m_item = MediaItem {};
        m_engine->setTimeline(nullptr);
        m_timeline->setData(TimelineData {});
        Q_EMIT mediaChanged(QString());
        return;
    }
    m_item = *item;
    TimelineData tl = makeSourceTimeline(m_item);
    const Memory memory = m_memory.value(m_item.id);
    tl.markIn = memory.markIn;
    tl.markOut = memory.markOut;
    auto media = std::make_shared<MediaPathMap>();
    media->insert(m_item.id, m_item.path);
    m_timeline->setNestedResolver({}); // plain media: no nested timelines
    // Detach while replacing the content so the engine renders once, for the final state.
    m_engine->setTimeline(nullptr);
    m_engine->setMediaPaths(media);
    m_timeline->setData(std::move(tl));
    m_timeline->setPlayhead(std::clamp<int64_t>(
        memory.position, 0, std::max<int64_t>(0, m_timeline->data().duration() - 1)));
    m_engine->setTimeline(m_timeline);
    Q_EMIT mediaChanged(m_item.id);
}

void SourcePlayer::setContent(const MediaItem& item, TimelineData content,
    std::shared_ptr<const MediaPathMap> media, Timeline::NestedResolver resolver)
{
    const bool same = item.id == m_item.id;
    if (same) {
        // Same item, new content (grid page, angle, edited multicam clip): keep the marks,
        // the position and the transport; the engine re-renders on Timeline::changed().
        content.markIn = m_timeline->data().markIn;
        content.markOut = m_timeline->data().markOut;
        m_engine->setMediaPaths(std::move(media));
        m_timeline->setNestedResolver(std::move(resolver));
        m_timeline->setData(std::move(content));
        return;
    }
    m_engine->stop();
    remember();
    m_item = item;
    const Memory memory = m_memory.value(m_item.id);
    content.markIn = memory.markIn;
    content.markOut = memory.markOut;
    m_engine->setTimeline(nullptr);
    m_engine->setMediaPaths(std::move(media));
    m_timeline->setNestedResolver(std::move(resolver));
    m_timeline->setData(std::move(content));
    m_timeline->setPlayhead(std::clamp<int64_t>(
        memory.position, 0, std::max<int64_t>(0, m_timeline->data().duration() - 1)));
    m_engine->setTimeline(m_timeline);
    Q_EMIT mediaChanged(m_item.id);
}

int64_t SourcePlayer::markInUs(const QString& mediaId) const
{
    if (hasMedia() && mediaId == m_item.id)
        return markIn() > 0 ? framesToUs(markIn(), fps()) : 0;
    const auto it = m_memory.constFind(mediaId);
    return it != m_memory.constEnd() && it->markIn > 0 ? framesToUs(it->markIn, it->fps) : 0;
}

int64_t SourcePlayer::markIn() const
{
    return m_timeline->data().markIn;
}

int64_t SourcePlayer::markOut() const
{
    return m_timeline->data().markOut;
}

void SourcePlayer::setMarks(int64_t in, int64_t out)
{
    if (!hasMedia()) {
        return;
    }
    if (in >= 0 && out >= 0 && out <= in) {
        out = -1;
    }
    TimelineData& d = m_timeline->mutableData();
    if (d.markIn == in && d.markOut == out) {
        return;
    }
    // Source marks are viewer state, not project edits: no undo step.
    d.markIn = in;
    d.markOut = out;
    m_timeline->notifyChanged();
}

void SourcePlayer::markInAtPosition()
{
    const int64_t in = m_engine->position();
    setMarks(in, markOut() > in ? markOut() : -1);
}

void SourcePlayer::markOutAtPosition()
{
    // Like the timeline: the out mark is exclusive, marking out on a frame includes it.
    const int64_t out = m_engine->position() + 1;
    setMarks(markIn() >= 0 && markIn() < out ? markIn() : -1, out);
}

void SourcePlayer::clearIn()
{
    setMarks(-1, markOut());
}

void SourcePlayer::clearOut()
{
    setMarks(markIn(), -1);
}

void SourcePlayer::clearBoth()
{
    setMarks(-1, -1);
}

std::optional<SourceRange> SourcePlayer::selection() const
{
    if (!hasMedia()) {
        return std::nullopt;
    }
    SourceRange range;
    range.mediaId = m_item.id;
    const Rational rate = fps();
    range.inUs = markIn() >= 0 ? framesToUs(markIn(), rate) : 0;
    if (markOut() > 0) {
        range.outUs = framesToUs(markOut(), rate);
        if (!m_item.info.isStillImage && m_item.info.durationUs > 0) {
            range.outUs = std::min(range.outUs, m_item.info.durationUs);
        }
    } else {
        range.outUs = m_item.info.isStillImage ? 0 : m_item.info.durationUs; // 0 = to the end
    }
    return range;
}

} // namespace kk
