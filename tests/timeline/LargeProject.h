// SPDX-License-Identifier: GPL-3.0-or-later
// A synthetic large project for the stress benchmark (polish round 4): about one hour at
// 25 fps on 8 video and 8 audio tracks, about 1,500 clips with transitions, effects,
// keyframes, clip markers and timeline markers. Built straight into the track vectors (no
// EditOps per clip), so building it costs a few milliseconds.
#pragma once

#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QColor>
#include <QStringList>

#include <cstdint>

namespace kk::test {

/// Shape of the synthetic project.
struct LargeProjectSpec
{
    int videoTracks = 8;
    int audioTracks = 8;
    int64_t durationFrames = 25 * 3600; ///< one hour at 25 fps
    int clips = 1500; ///< total, spread over every track
    int timelineMarkers = 200;
    int mediaFiles = 40; ///< distinct media pool items the clips refer to
};

/// Counts of what buildLargeProject() created.
struct LargeProjectStats
{
    int clips = 0;
    int transitions = 0;
    int keyframes = 0;
    int markers = 0; ///< timeline + clip markers
};

/// Replaces @p project's active timeline with the synthetic project described by @p spec
/// (one undo step "Build Large Project"). Media pool items are fake files (never decoded:
/// the timeline only needs their ids and names). V1 / A1 hold linked interview cuts with
/// dissolves; the upper tracks hold sparser B-roll, titles and music with gaps.
inline LargeProjectStats buildLargeProject(Project& project, const LargeProjectSpec& spec = {})
{
    LargeProjectStats stats;
    QStringList mediaIds;
    for (int i = 0; i < spec.mediaFiles; ++i) {
        MediaItem item;
        item.path = QStringLiteral("/stress/media/clip_%1.mov").arg(i, 3, 10, QLatin1Char('0'));
        item.name = QStringLiteral("clip_%1.mov").arg(i, 3, 10, QLatin1Char('0'));
        item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
        item.info.width = 1920;
        item.info.height = 1080;
        item.info.durationUs = int64_t(600) * 1000000; // ten minutes each
        mediaIds << project.mediaPool()->addItem(item);
    }

    project.beginEdit(QStringLiteral("Build Large Project"));
    TimelineData& d = project.timeline()->mutableData();
    d.videoTracks.clear();
    d.audioTracks.clear();
    for (int i = 0; i < spec.videoTracks; ++i) {
        TrackData t;
        t.id = d.allocateId();
        t.type = TrackType::Video;
        d.videoTracks.push_back(t);
    }
    for (int i = 0; i < spec.audioTracks; ++i) {
        TrackData t;
        t.id = d.allocateId();
        t.type = TrackType::Audio;
        d.audioTracks.push_back(t);
    }

    // V1 + A1 get half of the clips as linked pairs (a dense cut), the other tracks share the
    // rest evenly with gaps between their clips.
    const int pairs = spec.clips / 4;
    const int others = spec.clips - 2 * pairs;
    const int otherTracks = spec.videoTracks + spec.audioTracks - 2;
    const int perOther = otherTracks > 0 ? others / otherTracks : 0;
    const EffectRegistry& fx = EffectRegistry::instance();

    auto makeClip = [&](int n, int64_t start, int64_t duration) {
        ClipData c;
        c.id = d.allocateId();
        c.kind = ClipKind::Media;
        c.mediaId = mediaIds[n % mediaIds.size()];
        c.name = QStringLiteral("clip_%1.mov").arg(n % mediaIds.size(), 3, 10, QLatin1Char('0'));
        c.start = start;
        c.duration = duration;
        c.sourceInUs = int64_t((n * 7919) % 300) * 1000000;
        c.sourceDurationUs = int64_t(600) * 1000000;
        return c;
    };

    const int64_t pairLen = std::max<int64_t>(2, spec.durationFrames / std::max(1, pairs));
    for (int i = 0; i < pairs; ++i) {
        const int64_t start = i * pairLen;
        ClipData v = makeClip(i, start, pairLen);
        ClipData a = makeClip(i, start, pairLen);
        v.linkGroup = a.linkGroup = d.allocateId();
        if (i > 0 && i % 4 == 0) {
            v.transitionIn = fx.createTransition(QStringLiteral("cross_dissolve"), 25);
            a.transitionIn = fx.createTransition(QStringLiteral("cross_dissolve"), 25);
            stats.transitions += 2;
        }
        if (i % 10 == 0) {
            AnimatedValue& op = v.transform.animated(keys::Opacity, 100);
            op.setKey(0, 0);
            op.setKey(12, 100);
            AnimatedValue& vol = a.audio.animated(keys::VolumeDb, 0);
            vol.setKey(0, -30);
            vol.setKey(12, 0);
            stats.keyframes += 4;
        }
        if (i % 7 == 0) {
            v.markers.append(
                Marker {10, QStringLiteral("Note %1").arg(i), QColor(0xe8, 0x9b, 0x3c)});
            ++stats.markers;
        }
        d.videoTracks[0].clips.push_back(std::move(v));
        d.audioTracks[0].clips.push_back(std::move(a));
        stats.clips += 2;
    }

    int n = pairs;
    auto fillTrack = [&](TrackData& track, int count, int salt) {
        if (count <= 0)
            return;
        const int64_t slot = spec.durationFrames / count;
        for (int i = 0; i < count; ++i) {
            const int64_t len = std::max<int64_t>(2, slot * (50 + (i * 37 + salt) % 40) / 100);
            ClipData c = makeClip(n++, i * slot + (slot - len) / 2, len);
            if (track.type == TrackType::Video && i % 6 == 0) {
                c.effects.append(fx.createEffect(QStringLiteral("gaussian_blur"), d.allocateId()));
            }
            if (i % 5 == 0) {
                AnimatedValue& v = track.type == TrackType::Video
                    ? c.transform.animated(keys::ZoomX, 1.0)
                    : c.audio.animated(keys::VolumeDb, 0);
                v.setKey(0, track.type == TrackType::Video ? 1.0 : -12.0);
                v.setKey(len / 2, track.type == TrackType::Video ? 1.2 : 0.0);
                v.setKey(len - 1, track.type == TrackType::Video ? 1.0 : -12.0);
                stats.keyframes += 3;
            }
            if (i % 8 == 0) {
                c.transitionOut = fx.createTransition(QStringLiteral("cross_dissolve"), 12);
                ++stats.transitions;
            }
            track.clips.push_back(std::move(c));
            ++stats.clips;
        }
    };
    for (int t = 1; t < spec.videoTracks; ++t)
        fillTrack(d.videoTracks[size_t(t)], perOther, t);
    for (int t = 1; t < spec.audioTracks; ++t)
        fillTrack(d.audioTracks[size_t(t)], perOther, 100 + t);

    for (int i = 0; i < spec.timelineMarkers; ++i) {
        const QColor color = i % 3 == 0 ? QColor(0x4c, 0x9b, 0xe8)
            : i % 3 == 1                ? QColor(0xe8, 0x9b, 0x3c)
                                        : QColor(0x6c, 0xc0, 0x5c);
        d.markers.append(Marker {int64_t(i) * spec.durationFrames / spec.timelineMarkers + 7,
            QStringLiteral("Marker %1").arg(i + 1), color});
        ++stats.markers;
    }
    project.endEdit();
    return stats;
}

} // namespace kk::test
