// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectKinds implementation: the registry and the `.kkproj` metadata reader.
#include "projects/ProjectKinds.h"

#include "core/MediaItem.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Serialization.h"
#include "core/TimelineData.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "projects/ProjectLock.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <memory>

namespace kk::projectkinds {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ProjectKinds", text);
}

QVector<ProjectKind> builtinKinds()
{
    ProjectKind kkproj;
    kkproj.id = QStringLiteral("kkproj");
    kkproj.displayName = tr("Linked project");
    kkproj.badge = tr("Linked");
    kkproj.storage = ProjectStorage::Linked;
    kkproj.extensions = {QStringLiteral("kkproj")};
    kkproj.readMetadata = &readKkprojMetadata;
    kkproj.writeName = &writeKkprojName;
    kkproj.create = &createKkproj;

    // The single-file container (src/pack, docs/single-file-project.md).
    ProjectKind single;
    single.id = QStringLiteral("kinoklip");
    single.displayName = tr("Single-file project");
    single.badge = tr("Single file");
    single.storage = ProjectStorage::SingleFile;
    single.extensions = {QLatin1String(pack::kContainerSuffix)};
    single.readMetadata = &readKinoklipMetadata;
    single.writeName = &writeKinoklipName;
    single.create = &createKinoklip;
    return {kkproj, single};
}

QVector<ProjectKind>& registry()
{
    static QVector<ProjectKind> kinds = builtinKinds();
    return kinds;
}

Rational fpsFromJson(const QJsonObject& o, Rational fallback)
{
    const Rational r {o.value(QLatin1String("num")).toInt(fallback.num),
        o.value(QLatin1String("den")).toInt(fallback.den)};
    return r.num > 0 && r.den > 0 ? r : fallback;
}

/// The topmost enabled video track's Media clip at @p frame, or nullptr.
const ClipData* mediaClipAt(const TimelineData& t, int64_t frame)
{
    for (auto track = t.videoTracks.rbegin(); track != t.videoTracks.rend(); ++track) {
        if (!track->enabled)
            continue;
        for (const ClipData& c : track->clips) {
            if (c.kind == ClipKind::Media && c.enabled && frame >= c.start
                && frame < c.start + c.duration)
                return &c;
        }
    }
    return nullptr;
}

/// The earliest Media clip on any video track (lowest track first on ties), or nullptr.
const ClipData* firstMediaClip(const TimelineData& t)
{
    const ClipData* best = nullptr;
    for (const TrackData& track : t.videoTracks) {
        for (const ClipData& c : track.clips) {
            if (c.kind == ClipKind::Media && (!best || c.start < best->start)) {
                best = &c;
                break; // clips are sorted by start: the first Media clip of a track is its earliest
            }
        }
    }
    return best;
}

} // namespace

const QVector<ProjectKind>& all()
{
    return registry();
}

void registerKind(const ProjectKind& kind)
{
    QVector<ProjectKind>& kinds = registry();
    for (ProjectKind& k : kinds) {
        if (k.id == kind.id) {
            k = kind;
            return;
        }
    }
    kinds.append(kind);
}

const ProjectKind* byId(const QString& id)
{
    for (const ProjectKind& k : registry()) {
        if (k.id == id)
            return &k;
    }
    return nullptr;
}

const ProjectKind* byStorage(ProjectStorage storage)
{
    for (const ProjectKind& k : registry()) {
        if (k.storage == storage)
            return &k;
    }
    return nullptr;
}

const ProjectKind* forPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return nullptr;
    for (const ProjectKind& k : registry()) {
        if (k.extensions.contains(suffix))
            return &k;
    }
    return nullptr;
}

QStringList nameFilters()
{
    QStringList filters;
    for (const ProjectKind& k : registry()) {
        for (const QString& ext : k.extensions)
            filters << QStringLiteral("*.") + ext;
    }
    return filters;
}

ProjectMetadata readKkprojMetadata(const QString& path)
{
    ProjectMetadata meta;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        meta.error = tr("Cannot open the file: %1").arg(file.errorString());
        return meta;
    }
    return readDocumentMetadata(file.readAll(), path);
}

ProjectMetadata readKinoklipMetadata(const QString& path)
{
    // Only the document is read (the index and one small entry), never the media.
    ProjectMetadata meta;
    QString error;
    const std::unique_ptr<pack::ProjectPackage> package
        = pack::ProjectPackage::open(path, false, &error);
    if (!package) {
        meta.error = tr("Cannot open the file: %1").arg(error);
        return meta;
    }
    if (!package->hasDocument()) {
        meta.error = tr("The project file holds no project yet");
        return meta;
    }
    const QByteArray document = package->document(&error);
    if (document.isEmpty()) {
        meta.error = tr("Damaged project file: %1").arg(error);
        return meta;
    }
    return readDocumentMetadata(document, path);
}

ProjectMetadata readDocumentMetadata(const QByteArray& json, const QString& path)
{
    ProjectMetadata meta;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        meta.error = tr("Not a valid project file: %1").arg(parseError.errorString());
        return meta;
    }
    const QJsonObject root = doc.object();
    if (!root.contains(QLatin1String("formatVersion"))) {
        meta.error = tr("Not a " KK_APP_NAME " project file");
        return meta;
    }
    const int version = root.value(QLatin1String("formatVersion")).toInt();
    if (version > serialization::kFormatVersion) {
        meta.error = tr("Created by a newer version (format %1)").arg(version);
        return meta;
    }

    // Timelines: only the active one is decoded (duration, poster); the others are counted.
    // Versions 1 and 2 had a single "timeline" object (uid 1), see Project::fromJson().
    const QJsonObject project = root.value(QLatin1String("project")).toObject();
    QJsonArray timelines;
    if (version < 3)
        timelines.append(root.value(QLatin1String("timeline")).toObject());
    else
        timelines = root.value(QLatin1String("timelines")).toArray();
    meta.timelineCount = int(timelines.size());
    if (timelines.isEmpty()) {
        meta.error = tr("The project has no timeline");
        return meta;
    }
    const qint64 activeUid = root.value(QLatin1String("activeTimeline")).toInteger(0);
    QJsonObject activeJson = timelines.first().toObject();
    for (const QJsonValue& v : std::as_const(timelines)) {
        if (v.toObject().value(QLatin1String("uid")).toInteger() == activeUid) {
            activeJson = v.toObject();
            break;
        }
    }
    TimelineData timeline;
    QString why;
    if (!serialization::timelineFromJson(activeJson, timeline, &why)) {
        meta.error = tr("Damaged timeline: %1").arg(why);
        return meta;
    }

    meta.name = project.value(QLatin1String("name")).toString(QFileInfo(path).completeBaseName());
    meta.width = project.value(QLatin1String("width")).toInt(timeline.width);
    meta.height = project.value(QLatin1String("height")).toInt(timeline.height);
    meta.fps = fpsFromJson(project.value(QLatin1String("fps")).toObject(), timeline.fps);
    meta.timelineFps = timeline.fps;
    meta.durationFrames = timeline.duration();

    // Media pool: relative paths resolve against the project's folder (like Project::load()).
    const QDir projectDir = QFileInfo(path).absoluteDir();
    const MediaPoolState pool = serialization::mediaPoolFromJson(
        root.value(QLatin1String("mediaPool")).toObject(), projectDir);
    for (const MediaItem& m : pool.items) {
        if (m.isTimeline() || m.path.isEmpty())
            continue;
        ++meta.mediaCount;
        if (!media::mediaExists(m.path))
            ++meta.missingMediaCount;
    }

    // Poster: the frame under the saved playhead of the active timeline, else the first video
    // clip's first frame (what the timeline shows when it opens).
    int64_t playhead = 0;
    for (const QJsonValue& v : root.value(QLatin1String("timelineViews")).toArray()) {
        const QJsonObject o = v.toObject();
        if (quint64(o.value(QLatin1String("uid")).toInteger()) == timeline.uid)
            playhead = o.value(QLatin1String("playhead")).toInteger(0);
    }
    const ClipData* clip = mediaClipAt(timeline, playhead);
    int64_t frame = playhead;
    if (!clip) {
        clip = firstMediaClip(timeline);
        frame = clip ? clip->start : 0;
    }
    if (clip) {
        for (const MediaItem& m : pool.items) {
            if (m.id == clip->mediaId && !m.path.isEmpty() && !m.isTimeline()) {
                meta.posterMediaPath = m.path;
                meta.posterTimeUs = m.info.isStillImage
                    ? 0
                    : std::max<int64_t>(0,
                          retime::sourceTimeUs(*clip, double(frame - clip->start), timeline.fps));
                break;
            }
        }
    }
    return meta;
}

bool createKkproj(const QString& path, const QString& name, QString* error)
{
    Project project;
    ProjectSettings settings = project.settings();
    settings.name = name;
    project.setSettings(settings);
    project.setName(name);
    return project.save(path, error);
}

bool createKinoklip(const QString& path, const QString& name, QString* error)
{
    Project project;
    ProjectSettings settings = project.settings();
    settings.name = name;
    project.setSettings(settings);
    project.setName(name);
    const QString absolute = QFileInfo(path).absoluteFilePath();
    std::unique_ptr<pack::ProjectPackage> package = pack::ProjectPackage::create(absolute, error);
    if (!package)
        return false;
    if (!package->saveDocument(project.documentJson(absolute), error)) {
        package.reset();
        QFile::remove(absolute);
        return false;
    }
    pack::invalidateIndexCache(absolute);
    return true;
}

bool writeKinoklipName(const QString& path, const QString& name, QString* error)
{
    // The window that has the project open writes it; nobody else may (one writer per file).
    const std::unique_ptr<ProjectLock> lock = ProjectLock::acquire(path);
    if (!lock) {
        if (error)
            *error = tr("The project is open in another window or program");
        return false;
    }
    std::unique_ptr<pack::ProjectPackage> package = pack::ProjectPackage::open(path, true, error);
    if (!package)
        return false;
    const QByteArray stored = package->storedDocument(error);
    if (stored.isEmpty())
        return false;
    QJsonObject root = QJsonDocument::fromJson(stored).object();
    QJsonObject project = root.value(QLatin1String("project")).toObject();
    if (project.value(QLatin1String("name")).toString() == name)
        return true; // nothing to change: keep the file as it is
    project[QLatin1String("name")] = name;
    root[QLatin1String("project")] = project;
    const bool ok
        = package->saveStoredDocument(QJsonDocument(root).toJson(QJsonDocument::Indented), error);
    pack::invalidateIndexCache(package->path());
    return ok;
}

bool writeKkprojName(const QString& path, const QString& name, QString* error)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error)
            *error = in.errorString();
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &parseError);
    in.close();
    if (!doc.isObject()) {
        if (error)
            *error = tr("Not a valid project file: %1").arg(parseError.errorString());
        return false;
    }
    QJsonObject root = doc.object();
    QJsonObject project = root.value(QLatin1String("project")).toObject();
    if (project.value(QLatin1String("name")).toString() == name)
        return true; // nothing to change: keep the file (and its modification time) as it is
    project[QLatin1String("name")] = name;
    root[QLatin1String("project")] = project;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }
    return true;
}

} // namespace kk::projectkinds
