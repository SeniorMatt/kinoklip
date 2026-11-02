// SPDX-License-Identifier: GPL-3.0-or-later
// SourcePlayer — plays one media item (source viewer, Media page preview) with its own in/out
// marks, by wrapping a PlaybackEngine around a private one-clip timeline.
#pragma once

#include "core/MediaItem.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"

#include <QHash>
#include <QObject>

#include <cstdint>
#include <memory>
#include <optional>

namespace kk {

class Compositor;
class Timeline;

/// A marked range of a media item, in source microseconds (what F9/F10/F12 and the
/// mime::SourceRange drag carry).
struct SourceRange
{
    QString mediaId;
    int64_t inUs = 0;
    int64_t outUs = 0; ///< exclusive; <= inUs means "to the end of the media"
};

/// Light player for a single media item.
///
/// Design choice: instead of a second decoding path (VideoDecoder/AudioDecoder driven by
/// hand), the source player builds a private one-clip TimelineData — V1 and A1 holding the
/// whole item at the media's own frame rate and display size — and plays it with a normal
/// PlaybackEngine. That reuses everything: the audio-master clock, J/K/L, loop, look-ahead,
/// quality, zero-copy textures, and frames are exact media frames (frame n = n / mediaFps).
/// The Compositor is normally shared with the program engine (one render thread and GL
/// context for the whole application; requests are cheap to interleave because only one
/// engine plays at a time).
///
/// In/out marks are stored in the private timeline (TimelineData::markIn/markOut, frames of
/// the media rate) and remembered per media item while the player lives, like the playhead.
///
/// Thread-safety: GUI thread only.
class SourcePlayer : public QObject
{
    Q_OBJECT
public:
    /// Default duration given to stills (seconds).
    static constexpr int kStillSeconds = 5;
    /// Frame rate used for media without one (stills, audio files).
    static constexpr Rational kFallbackFps {25, 1};

    /// Creates a player sharing @p compositor (nullptr = its own) with the default audio output.
    explicit SourcePlayer(std::shared_ptr<Compositor> compositor = {}, QObject* parent = nullptr);
    /// Creates a player with explicit engine options (tests: fake audio, no audio).
    explicit SourcePlayer(PlaybackEngine::Options options, QObject* parent = nullptr);
    ~SourcePlayer() override;

    /// Loads @p item (copied; nullptr = unload). Restores the marks and position it had the
    /// last time it was loaded. Stops playback.
    void setMedia(const MediaItem* item);
    /// Loads @p item shown through the given private timeline @p content instead of the
    /// one-clip timeline (the multicam viewer's angle grid, a multicam clip's angle), with
    /// @p media for the clips inside and @p resolver for its nested timelines. Marks and
    /// position are remembered per item as with setMedia(); loading the same item again
    /// (another page, angle or an edited multicam clip) keeps the position and playback.
    void setContent(const MediaItem& item, TimelineData content,
        std::shared_ptr<const MediaPathMap> media, Timeline::NestedResolver resolver);
    /// Id of the loaded item (empty = none).
    [[nodiscard]] QString mediaId() const { return m_item.id; }
    /// The loaded item (default-constructed if none).
    [[nodiscard]] const MediaItem& media() const { return m_item; }
    [[nodiscard]] bool hasMedia() const { return !m_item.id.isEmpty(); }

    /// The engine (transport, frames, position).
    [[nodiscard]] PlaybackEngine* engine() const { return m_engine; }
    /// The private one-clip timeline (fps = media rate; marks = the source marks).
    [[nodiscard]] Timeline* timeline() const { return m_timeline; }
    /// Frame rate of the loaded item's frames.
    [[nodiscard]] Rational fps() const;

    // ---- Marks (frames of the media rate; -1 = unset; out is exclusive) ----
    [[nodiscard]] int64_t markIn() const;
    [[nodiscard]] int64_t markOut() const;
    /// Sets both marks (invalid combinations clear the out mark).
    void setMarks(int64_t in, int64_t out);
    /// In at the current frame (keeps the out mark if it is after it).
    void markInAtPosition();
    /// Out after the current frame (the frame itself is included).
    void markOutAtPosition();
    void clearIn();
    void clearOut();
    void clearBoth();

    /// In point of item @p mediaId in source µs as last marked in this player (the loaded item
    /// or a remembered one); 0 when it has none (Sync Based on In Points).
    [[nodiscard]] int64_t markInUs(const QString& mediaId) const;

    /// The marked range of the loaded item (whole item where unmarked), or nullopt if nothing
    /// is loaded.
    [[nodiscard]] std::optional<SourceRange> selection() const;

Q_SIGNALS:
    /// A different item was loaded (empty id = unloaded).
    void mediaChanged(const QString& mediaId);
    /// In or out mark changed.
    void marksChanged();

private:
    struct Memory
    {
        int64_t markIn = -1;
        int64_t markOut = -1;
        int64_t position = 0;
        Rational fps; ///< rate of the frame numbers above
    };

    void init(PlaybackEngine::Options options);
    void remember();

    PlaybackEngine* m_engine = nullptr;
    Timeline* m_timeline = nullptr;
    MediaItem m_item;
    QHash<QString, Memory> m_memory;
};

/// The one-clip timeline SourcePlayer plays for @p item (exposed for tests).
[[nodiscard]] TimelineData makeSourceTimeline(const MediaItem& item);

} // namespace kk
