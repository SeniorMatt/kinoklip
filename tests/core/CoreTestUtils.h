// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the core unit tests: clip factories and an invariant-checking macro.
#pragma once

#include "core/EditOps.h"
#include "core/TimelineData.h"

#include <QTest>

namespace cw::test {

/// 25 fps → 40 000 µs per frame.
inline constexpr int64_t kUsPerFrame25 = 40000;

/// A media clip [start, start + duration) showing source from @p sourceInFrames (25 fps frames)
/// of a source that is @p sourceFrames long (0 = unbounded).
inline ClipData mediaClip(int64_t start, int64_t duration, int64_t sourceInFrames = 0,
    int64_t sourceFrames = 0, const QString& name = QStringLiteral("clip"))
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = QStringLiteral("media-1");
    c.name = name;
    c.start = start;
    c.duration = duration;
    c.sourceInUs = sourceInFrames * kUsPerFrame25;
    c.sourceDurationUs = sourceFrames * kUsPerFrame25;
    return c;
}

/// Default 25 fps timeline with V1, V2, A1, A2.
inline TimelineData timeline()
{
    return makeDefaultTimeline(QStringLiteral("Test"), Rational {25, 1});
}

inline constexpr TrackRef kV1 {TrackType::Video, 0};
inline constexpr TrackRef kV2 {TrackType::Video, 1};
inline constexpr TrackRef kA1 {TrackType::Audio, 0};
inline constexpr TrackRef kA2 {TrackType::Audio, 1};

/// "start:duration" pairs of the clips on a track, e.g. "0:10 20:5", for compact comparisons.
inline QString layout(const TimelineData& data, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : data.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

} // namespace cw::test

/// Fails the current test if the timeline violates its invariants.
#define CW_VERIFY_INVARIANTS(data)                                                                 \
    do {                                                                                           \
        QString why_;                                                                              \
        QVERIFY2(cw::editops::checkInvariants((data), &why_), qPrintable(why_));                   \
    } while (false)
