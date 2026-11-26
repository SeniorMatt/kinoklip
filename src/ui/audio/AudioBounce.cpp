// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/audio/AudioBounce.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "media/WavWriter.h"
#include "render/AudioMixer.h"
#include "render/AudioNormalize.h"
#include "render/RenderTypes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

namespace kk::audiobounce {

namespace {

constexpr int kBlockFrames = 4800; ///< 100 ms per mixer call
constexpr char kBinName[] = "Bounced Audio";

QString txt(const char* text)
{
    return QCoreApplication::translate("kk::AudioBounce", text);
}

QString safeFileName(QString name)
{
    for (const QChar c : {u'/', u'\\', u':', u'*', u'?', u'"', u'<', u'>', u'|'})
        name.replace(c, QLatin1Char('_'));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("Audio") : name;
}

QString uniquePath(const QDir& dir, const QString& base)
{
    QString path = dir.filePath(base + QStringLiteral(".wav"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = dir.filePath(base + QStringLiteral(" (%1).wav").arg(n));
    return path;
}

/// One source to bounce: the main mix (track 0) or an audio track.
struct Job
{
    quint64 trackId = 0;
    QString name; ///< track name / "Mix"
    QString path; ///< the WAV file
};

} // namespace

QVector<quint64> selectedAudioTracks(const TimelineData& timeline, const SelectionModel& selection)
{
    QVector<quint64> ids;
    for (const TrackData& track : timeline.audioTracks) {
        for (const ClipData& clip : track.clips) {
            if (selection.isSelected(clip.id)) {
                ids.append(track.id);
                break;
            }
        }
    }
    if (!ids.isEmpty())
        return ids;
    for (const TrackData& track : timeline.audioTracks) {
        if (track.autoSelect && !track.clips.empty())
            ids.append(track.id);
    }
    return ids;
}

QString bounceFolder(const Project& project, bool stored)
{
    if (stored)
        return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + QStringLiteral("/kinoklip-bounce");
    const QString file = project.filePath();
    if (!file.isEmpty())
        return QFileInfo(file).absolutePath() + QStringLiteral("/Bounced Audio");
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/Bounced Audio");
}

Result bounce(Project& project, const Options& options, const std::atomic<bool>* cancel,
    const std::function<void(double)>& progress)
{
    Result result;
    const std::shared_ptr<const TimelineData> snapshot = project.timeline()->snapshot();
    const TimelineData& timeline = *snapshot;
    const auto [first, end] = audionorm::mixRange(timeline);
    if (!timeline.fps.isValid() || end <= first) {
        result.error = txt("The timeline has nothing to bounce.");
        return result;
    }
    if (!QDir().mkpath(options.folder)) {
        result.error = txt("The folder for the bounced audio could not be created: %1").arg(options.folder);
        return result;
    }
    const QDir folder(options.folder);

    std::vector<Job> jobs;
    if (options.mix) {
        jobs.push_back({0, txt("Mix"), {}});
    } else {
        for (const quint64 id : options.trackIds) {
            for (size_t i = 0; i < timeline.audioTracks.size(); ++i) {
                if (timeline.audioTracks[i].id == id)
                    jobs.push_back({id, trackDisplayName(timeline.audioTracks[i], int(i)), {}});
            }
        }
    }
    if (jobs.empty()) {
        result.error = txt("There is no audio track to bounce.");
        return result;
    }
    const QString projectName = safeFileName(project.name());
    for (Job& job : jobs) {
        job.path = uniquePath(folder,
            projectName + QLatin1Char(' ')
                + (options.mix ? txt("Mix Bounce") : safeFileName(job.name) + QLatin1Char(' ') + txt("Bounce")));
        result.files.append(job.path); // reserved names stay unique across the loop below
    }
    // uniquePath() only looks at the disk: make the names of this call differ from each other.
    for (size_t i = 0; i < jobs.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (jobs[i].path == jobs[j].path) {
                const QString base = QFileInfo(jobs[i].path).completeBaseName();
                jobs[i].path = uniquePath(folder, base + QStringLiteral(" (%1)").arg(i + 1));
                result.files[int(i)] = jobs[i].path;
            }
        }
    }

    const auto cleanup = [&] {
        for (const QString& f : std::as_const(result.files))
            QFile::remove(f);
        result.files.clear();
    };

    // Render every source from the snapshot.
    const auto media = makeMediaPathMap(project.mediaPool()->state());
    const int64_t firstSample = framesToSamples48k(first, timeline.fps);
    const int64_t lastSample = framesToSamples48k(end, timeline.fps);
    const int64_t total = lastSample - firstSample;
    constexpr int ch = AudioMixer::kChannels;
    for (size_t j = 0; j < jobs.size(); ++j) {
        AudioMixer mixer;
        mixer.setMediaPaths(media);
        std::vector<float> out(size_t(kBlockFrames) * ch), tap(size_t(kBlockFrames) * ch);
        if (!options.mix)
            mixer.setTap(MixTarget::track(jobs[j].trackId), tap.data());
        media::WavWriter writer;
        if (!writer.open(jobs[j].path, AudioMixer::kSampleRate, ch, 24)) {
            result.error = txt("The bounced audio could not be written: %1").arg(writer.error());
            cleanup();
            return result;
        }
        for (int64_t at = 0; at < total; at += kBlockFrames) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                writer.close();
                result.cancelled = true;
                result.error = txt("Cancelled");
                cleanup();
                return result;
            }
            const int frames = int(std::min<int64_t>(kBlockFrames, total - at));
            mixer.mix(timeline, firstSample + at, frames, out.data());
            const float* heard = options.mix ? out.data() : tap.data();
            if (!writer.write(heard, frames)) {
                result.error = txt("The bounced audio could not be written: %1").arg(writer.error());
                writer.close();
                cleanup();
                return result;
            }
            if (progress)
                progress((double(j) + double(at + frames) / double(total)) / double(jobs.size()));
        }
        if (!writer.close()) {
            result.error = txt("The bounced audio could not be written: %1").arg(writer.error());
            cleanup();
            return result;
        }
    }

    // Into the project: media pool, then one new track and one clip each.
    QStringList paths;
    for (const Job& job : jobs) {
        QString path = job.path;
        if (options.store) {
            path = options.store(job.path);
            if (path.isEmpty()) {
                result.error = txt("The bounced audio could not be added to the project.");
                cleanup();
                return result;
            }
            if (path != job.path)
                QFile::remove(job.path);
        }
        paths.append(path);
    }
    project.beginEdit(options.mix ? txt("Bounce Mix to Track") : txt("Bounce Selected Tracks"));
    MediaPool* pool = project.mediaPool();
    QString binId;
    for (const Bin& b : pool->bins()) {
        if (b.name == QLatin1String(kBinName) && b.parentId.isEmpty()) {
            binId = b.id;
            break;
        }
    }
    if (binId.isEmpty())
        binId = pool->addBin(QString::fromLatin1(kBinName));
    const ImportResult imported = project.importMedia(paths, binId);
    if (imported.importedIds.size() != int(jobs.size())) {
        project.cancelEdit();
        result.error = txt("The bounced audio could not be added to the media pool: %1")
                           .arg(imported.warnings.join(QLatin1Char(' ')));
        cleanup();
        return result;
    }
    TimelineData& edit = project.timeline()->mutableData();
    for (size_t j = 0; j < jobs.size(); ++j) {
        const MediaItem* item = pool->item(imported.importedIds[int(j)]);
        const int index = editops::addTrack(edit, TrackType::Audio);
        TrackData& track = edit.audioTracks[size_t(index)];
        track.name = options.mix ? txt("Mix Bounce") : jobs[j].name + QLatin1Char(' ') + txt("Bounce");
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = imported.importedIds[int(j)];
        clip.name = QFileInfo(paths[int(j)]).completeBaseName();
        clip.start = first;
        clip.sourceInUs = 0;
        clip.sourceDurationUs = item ? item->info.durationUs : 0;
        clip.duration = end - first;
        if (editops::overwriteClip(edit, TrackRef {TrackType::Audio, index}, clip) == 0) {
            project.cancelEdit();
            result.error = txt("The bounced audio could not be placed on a track.");
            cleanup();
            return result;
        }
        result.newTrackIds.append(edit.audioTracks[size_t(index)].id);
    }
    project.endEdit();
    result.ok = true;
    return result;
}

} // namespace kk::audiobounce
