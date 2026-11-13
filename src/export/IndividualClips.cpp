// SPDX-License-Identifier: GPL-3.0-or-later
// IndividualClips implementation.
#include "export/IndividualClips.h"

#include "core/TimelineData.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <set>
#include <vector>

namespace kk {

namespace {

struct Segment
{
    const ClipData* clip = nullptr;
    int64_t first = 0, end = 0;
};

/// A file name stem without the characters file systems dislike.
QString cleaned(QString s)
{
    for (QChar& c : s) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')
            || c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('"')
            || c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('|')
            || c.unicode() < 32)
            c = QLatin1Char('_');
    }
    s = s.trimmed();
    while (s.startsWith(QLatin1Char('.')))
        s.remove(0, 1);
    return s;
}

/// "A001.mov" -> "A001" (a clip is often named after its file).
QString withoutExtension(const QString& name)
{
    const QString suffix = QFileInfo(name).suffix();
    if (suffix.size() >= 2 && suffix.size() <= 4 && suffix.size() < name.size() - 1) {
        bool alnum = true;
        for (QChar c : suffix)
            alnum = alnum && c.isLetterOrNumber();
        if (alnum)
            return name.left(name.size() - suffix.size() - 1);
    }
    return name;
}

} // namespace

QList<ClipRenderJob> individualClipJobs(
    const TimelineData& timeline, const ExportSettings& settings, const MediaPathMap& media)
{
    QList<ClipRenderJob> jobs;
    const auto [rangeFirst, rangeEnd] = settings.frameRange(timeline);
    if (rangeEnd <= rangeFirst)
        return jobs;
    // Cut points: the range edges and every clip edge inside it.
    std::set<int64_t> cuts {rangeFirst, rangeEnd};
    for (const TrackData& t : timeline.videoTracks) {
        if (!t.enabled)
            continue;
        for (const ClipData& c : t.clips) {
            if (!c.enabled)
                continue;
            for (int64_t f : {c.start, c.start + c.duration}) {
                if (f > rangeFirst && f < rangeEnd)
                    cuts.insert(f);
            }
        }
    }
    // The topmost enabled clip of every interval; equal neighbours join.
    std::vector<Segment> segments;
    for (auto it = cuts.begin(); std::next(it) != cuts.end(); ++it) {
        const int64_t a = *it, b = *std::next(it);
        const ClipData* top = nullptr;
        for (const TrackData& t : timeline.videoTracks) { // later tracks are higher
            if (!t.enabled)
                continue;
            for (const ClipData& c : t.clips) {
                if (c.enabled && c.start <= a && c.start + c.duration >= b)
                    top = &c;
            }
        }
        if (!top)
            continue;
        if (!segments.empty() && segments.back().clip == top && segments.back().end == a)
            segments.back().end = b;
        else
            segments.push_back({top, a, b});
    }
    const int64_t total = timeline.duration();
    const int handles = std::max(0, settings.clipHandles);
    const QFileInfo target(settings.outputPath);
    const QString folder = target.absolutePath();
    const QString extension = target.suffix().isEmpty() ? QString()
                                                        : QLatin1Char('.') + target.suffix();
    QSet<QString> used;
    int number = 0;
    for (const Segment& s : segments) {
        ++number;
        const auto fromSource = [&] {
            const auto it = media.constFind(s.clip->mediaId);
            return it == media.constEnd() ? QString() : QFileInfo(*it).completeBaseName();
        };
        const QString clipName = withoutExtension(s.clip->name);
        QString stem;
        if (settings.clipNaming == ClipNaming::SourceName)
            stem = fromSource();
        if (stem.isEmpty())
            stem = clipName.isEmpty() ? fromSource() : clipName;
        stem = cleaned(stem);
        if (stem.isEmpty())
            stem = QStringLiteral("clip");
        if (settings.clipNaming == ClipNaming::NumberedClipName)
            stem = QStringLiteral("%1_%2").arg(number, 3, 10, QLatin1Char('0')).arg(stem);
        QString unique = stem;
        for (int n = 2; used.contains(unique.toLower()); ++n)
            unique = QStringLiteral("%1_%2").arg(stem).arg(n);
        used.insert(unique.toLower());
        ClipRenderJob job;
        job.settings = settings;
        job.settings.individualClips = false;
        job.settings.range = RangeMode::Custom;
        job.settings.rangeIn = std::max<int64_t>(0, s.first - handles);
        job.settings.rangeOut = std::min(std::max(total, s.end), s.end + handles);
        job.settings.outputPath = QDir(folder).filePath(unique + extension);
        job.name = QStringLiteral("%1 - %2").arg(
            timeline.name.isEmpty() ? QStringLiteral("Timeline") : timeline.name, unique);
        jobs.append(job);
    }
    return jobs;
}

} // namespace kk
