// SPDX-License-Identifier: GPL-3.0-or-later
// EdlExport implementation: CMX3600 events, dissolves and M2 speed lines; marker lists.
#include "export/EdlExport.h"

#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/Retime.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::edl {

namespace {

constexpr int kReelWidth = 8; ///< CMX3600 reel field
constexpr int kChannelWidth = 5; ///< "V", "A", "A2", padded
const QString kAuxReel = QStringLiteral("AX"); ///< reel of sources without a file

/// One CMX event line before formatting.
struct EventLine
{
    QString reel;
    QString channel;
    QString edit; ///< "C" or "D"
    int64_t transitionFrames = 0; ///< D: dissolve length
    int64_t srcIn = 0, srcOut = 0, recIn = 0, recOut = 0; ///< frames (timeline rate)
};

/// One numbered event: one line, or two for a dissolve (outgoing cut + incoming D).
struct Event
{
    std::vector<EventLine> lines;
    QStringList comments; ///< "* FROM CLIP NAME: ..." etc.
    QString speedLine; ///< "M2 ..." (empty = 100 %)
    int64_t recIn = 0; ///< sort key
    int order = 0; ///< tie break: video, A, A2
};

QString pad(const QString& s, int width)
{
    return s.leftJustified(width, QLatin1Char(' '), true);
}

QString reelOf(const ClipData& clip, const MediaPool* pool)
{
    if (clip.kind != ClipKind::Media)
        return kAuxReel;
    const MediaItem* item = pool ? pool->item(clip.mediaId) : nullptr;
    QString reel = item ? QFileInfo(item->path).completeBaseName() : clip.name;
    if (reel.isEmpty())
        reel = kAuxReel;
    reel.replace(QLatin1Char(' '), QLatin1Char('_'));
    return reel.left(kReelWidth);
}

/// One event line; @p recordStart (the timeline's starting timecode) is added to the record
/// timecodes.
QString formatLine(int number, const EventLine& l, Rational fps, int64_t recordStart)
{
    // "001  REEL     V     C        00:00:00:00 00:00:05:00 00:00:00:00 00:00:05:00"
    const QString edit = l.edit == QLatin1String("D")
        ? QStringLiteral("D    %1").arg(l.transitionFrames, 3, 10, QLatin1Char('0'))
        : QStringLiteral("C       ");
    return QStringLiteral("%1  %2 %3 %4 %5 %6 %7 %8")
        .arg(number, 3, 10, QLatin1Char('0'))
        .arg(pad(l.reel, kReelWidth), pad(l.channel, kChannelWidth), edit, timecode(l.srcIn, fps),
            timecode(l.srcOut, fps), timecode(l.recIn + recordStart, fps),
            timecode(l.recOut + recordStart, fps));
}

/// Source frame (timeline rate, with the source start offset) shown at timeline frame
/// @p frame of @p clip.
int64_t sourceFrame(const ClipData& clip, int64_t frame, Rational fps, int64_t startUs)
{
    return usToFrames(startUs + clip.sourceTimeUsAt(frame, fps), fps);
}

/// Frames of @p clip's transitionIn that lie before its start (the cut), if the clip before
/// it on the track touches it (else the transition is a fade from nothing: 0).
int64_t dissolveBefore(const std::vector<ClipData>& clips, size_t i)
{
    const ClipData& c = clips[i];
    if (!c.transitionIn || i == 0 || clips[i - 1].end() != c.start)
        return -1;
    return c.transitionIn->framesBeforeCut();
}

void addTrackEvents(std::vector<Event>& events, const TrackData& track, const QString& channel,
    int order, const TimelineData& data, const MediaPool* pool, const EdlOptions& options)
{
    const Rational fps = data.fps;
    const auto& clips = track.clips;
    auto startUs = [&](const ClipData& c) {
        return options.sourceStartUs ? options.sourceStartUs(c) : int64_t(0);
    };
    for (size_t i = 0; i < clips.size(); ++i) {
        const ClipData& c = clips[i];
        const int64_t start = c.start;
        const int64_t end = c.end();
        // A dissolve into this clip moves its record in back by the frames before the cut,
        // and shortens the previous event (handled when that one was written, below).
        const int64_t before = dissolveBefore(clips, i);
        // The next clip's dissolve takes this clip's last frames.
        const int64_t nextBefore = i + 1 < clips.size() ? dissolveBefore(clips, i + 1) : -1;
        const int64_t recOut = nextBefore > 0 ? end - nextBefore : end;

        Event e;
        e.order = order;
        const QString reel = reelOf(c, pool);
        EventLine line;
        line.reel = reel;
        line.channel = channel;
        if (before >= 0 && c.transitionIn) {
            const ClipData& prev = clips[i - 1];
            const int64_t dStart = start - before;
            const int64_t dLength = c.transitionIn->duration;
            // Outgoing clip: zero-length cut at the dissolve start.
            EventLine from;
            from.reel = reelOf(prev, pool);
            from.channel = channel;
            from.edit = QStringLiteral("C");
            from.srcIn = from.srcOut = sourceFrame(prev, dStart, fps, startUs(prev));
            from.recIn = from.recOut = dStart;
            e.lines.push_back(from);
            line.edit = QStringLiteral("D");
            line.transitionFrames = dLength;
            line.recIn = dStart;
            line.srcIn = sourceFrame(c, dStart, fps, startUs(c));
        } else {
            line.edit = QStringLiteral("C");
            line.recIn = start;
            line.srcIn = sourceFrame(c, start, fps, startUs(c));
        }
        line.recOut = std::max(line.recIn, recOut);
        // Source out follows the clip's speed over the event's length.
        const double speed = c.retime.isAnimated()
            ? double(c.sourceTimeUsAt(end, fps) - c.sourceTimeUsAt(start, fps))
                / std::max<double>(1.0, double(framesToUs(end - start, fps)))
            : c.speed;
        line.srcOut = line.srcIn + int64_t(std::llround(double(line.recOut - line.recIn) * speed));
        e.recIn = line.recIn;
        e.lines.push_back(line);

        if (std::abs(speed - 1.0) > 1e-9) {
            // M2: speed in frames per second of the record rate, with the sign of the
            // direction (reverse < 0), and the source in it applies from.
            const double fpsSpeed = speed * fps.toDouble();
            e.speedLine = QStringLiteral("M2   %1       %2                %3")
                              .arg(pad(reel, kReelWidth))
                              .arg(fpsSpeed, 5, 'f', 1, QLatin1Char('0'))
                              .arg(timecode(line.srcIn, fps));
        }
        e.comments << QStringLiteral("* FROM CLIP NAME: %1").arg(c.name);
        if (c.retime.isAnimated())
            e.comments << QStringLiteral("* SPEED RAMP (WRITTEN AT THE AVERAGE SPEED)");
        if (!c.enabled)
            e.comments << QStringLiteral("* CLIP DISABLED");
        events.push_back(std::move(e));
    }
}

QString csvField(const QString& s)
{
    if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"'))
        && !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r')))
        return s;
    QString q = s;
    q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

QString header(const TimelineData& data)
{
    const QString title = data.name.isEmpty() ? QStringLiteral("Timeline") : data.name;
    return QStringLiteral("TITLE: %1\nFCM: NON-DROP FRAME\n\n").arg(title);
}

} // namespace

QString timecode(int64_t frames, Rational fps)
{
    const int64_t rate
        = fps.isValid() ? std::max<int64_t>(1, std::llround(fps.toDouble())) : int64_t(25);
    const int64_t f = std::max<int64_t>(0, frames);
    const int64_t s = f / rate;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'))
        .arg(f % rate, 2, 10, QLatin1Char('0'));
}

QString markerColorName(const QColor& color, bool upper)
{
    const QString name = QLatin1String(nearestMarkerColor(color).name);
    return upper ? name.toUpper() : name;
}

QString timelineToEdl(const TimelineData& data, const MediaPool* pool, const EdlOptions& options)
{
    std::vector<Event> events;
    if (options.videoTrack >= 0 && options.videoTrack < int(data.videoTracks.size()))
        addTrackEvents(events, data.videoTracks[size_t(options.videoTrack)], QStringLiteral("V"), 0,
            data, pool, options);
    if (options.audioTrack1 >= 0 && options.audioTrack1 < int(data.audioTracks.size()))
        addTrackEvents(events, data.audioTracks[size_t(options.audioTrack1)], QStringLiteral("A"),
            1, data, pool, options);
    if (options.audioTrack2 >= 0 && options.audioTrack2 < int(data.audioTracks.size()))
        addTrackEvents(events, data.audioTracks[size_t(options.audioTrack2)], QStringLiteral("A2"),
            2, data, pool, options);
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.recIn != b.recIn ? a.recIn < b.recIn : a.order < b.order;
    });

    QString out = header(data);
    int number = 0;
    for (const Event& e : events) {
        ++number;
        for (const EventLine& l : e.lines)
            out += formatLine(number, l, data.fps, data.startTimecode) + QLatin1Char('\n');
        if (!e.speedLine.isEmpty())
            out += e.speedLine + QLatin1Char('\n');
        for (const QString& c : e.comments)
            out += c + QLatin1Char('\n');
        out += QLatin1Char('\n');
    }
    return out;
}

QString markersToEdl(const TimelineData& data)
{
    QString out = header(data);
    int number = 0;
    for (const Marker& m : data.markers) {
        EventLine l;
        l.reel = QStringLiteral("001");
        l.channel = QStringLiteral("V");
        l.edit = QStringLiteral("C");
        const int64_t start = data.startTimecode;
        l.recIn = m.frame;
        l.recOut = m.end();
        l.srcIn = m.frame + start;
        l.srcOut = m.end() + start;
        out += formatLine(++number, l, data.fps, start) + QLatin1Char('\n');
        out += QStringLiteral("* LOC: %1 %2 %3\n")
                   .arg(timecode(m.frame + start, data.fps), pad(markerColorName(m.color, true), 8),
                       m.name);
        if (!m.note.isEmpty())
            out += QStringLiteral("* COMMENT: %1\n")
                       .arg(QString(m.note).replace(QLatin1Char('\n'), QLatin1Char(' ')));
        out += QLatin1Char('\n');
    }
    return out;
}

QString markersToCsv(const TimelineData& data)
{
    const QString crlf = QStringLiteral("\r\n");
    QString out = QStringLiteral("#,Name,Start,End,Duration,Color,Notes") + crlf;
    int number = 0;
    for (const Marker& m : data.markers) {
        out += QStringList {QString::number(++number), csvField(m.name),
                   timecode(m.frame + data.startTimecode, data.fps),
                   timecode(m.end() + data.startTimecode, data.fps), timecode(m.duration, data.fps),
                   markerColorName(m.color), csvField(m.note)}
                   .join(QLatin1Char(','))
            + crlf;
    }
    return out;
}

bool writeTextFile(const QString& path, const QString& text, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = QCoreApplication::translate("kk::EdlExport", "Cannot write %1: %2")
                         .arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace kk::edl
