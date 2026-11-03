// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLibrary implementation: listing, the metadata / poster caches and file operations.
#include "projects/ProjectLibrary.h"

#include "pack/ProjectPackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>

namespace kk {

namespace {

constexpr int kCacheVersion = 1;
constexpr int kFlushDelayMs = 1000;
/// Metadata readers in parallel: enough to hide file latency, few enough not to compete with
/// the media cache's decoders for the disk.
constexpr int kWorkerThreads = 2;
constexpr int kPosterJpegQuality = 85;

QString clean(const QString& path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

QJsonObject metaToJson(const ProjectMetadata& m)
{
    QJsonObject o;
    o[QLatin1String("name")] = m.name;
    o[QLatin1String("width")] = m.width;
    o[QLatin1String("height")] = m.height;
    o[QLatin1String("fps")] = QJsonArray {m.fps.num, m.fps.den};
    o[QLatin1String("duration")] = qint64(m.durationFrames);
    o[QLatin1String("timelineFps")] = QJsonArray {m.timelineFps.num, m.timelineFps.den};
    o[QLatin1String("timelines")] = m.timelineCount;
    o[QLatin1String("media")] = m.mediaCount;
    o[QLatin1String("missing")] = m.missingMediaCount;
    if (!m.posterMediaPath.isEmpty()) {
        o[QLatin1String("posterMedia")] = m.posterMediaPath;
        o[QLatin1String("posterUs")] = qint64(m.posterTimeUs);
    }
    if (!m.error.isEmpty())
        o[QLatin1String("error")] = m.error;
    return o;
}

ProjectMetadata metaFromJson(const QJsonObject& o)
{
    ProjectMetadata m;
    const auto fps = [&](const char* key) {
        const QJsonArray a = o.value(QLatin1String(key)).toArray();
        return Rational {a.at(0).toInt(0), a.at(1).toInt(1)};
    };
    m.name = o.value(QLatin1String("name")).toString();
    m.width = o.value(QLatin1String("width")).toInt();
    m.height = o.value(QLatin1String("height")).toInt();
    m.fps = fps("fps");
    m.durationFrames = o.value(QLatin1String("duration")).toInteger();
    m.timelineFps = fps("timelineFps");
    m.timelineCount = o.value(QLatin1String("timelines")).toInt();
    m.mediaCount = o.value(QLatin1String("media")).toInt();
    m.missingMediaCount = o.value(QLatin1String("missing")).toInt();
    m.posterMediaPath = o.value(QLatin1String("posterMedia")).toString();
    m.posterTimeUs = o.value(QLatin1String("posterUs")).toInteger();
    m.error = o.value(QLatin1String("error")).toString();
    return m;
}

QString pathHash(const QString& path)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
}

/// Copies a file or a folder tree.
bool copyRecursively(const QString& from, const QString& to)
{
    const QFileInfo info(from);
    if (!info.isDir())
        return QFile::copy(from, to);
    if (!QDir().mkpath(to))
        return false;
    const QDir dir(from);
    for (const QFileInfo& child :
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!copyRecursively(child.absoluteFilePath(), to + QLatin1Char('/') + child.fileName()))
            return false;
    }
    return true;
}

} // namespace

ProjectLibrary::ProjectLibrary(const QString& root, const QString& cacheDir, QObject* parent)
    : QObject(parent)
    , m_root(clean(root))
    , m_cacheDir(cacheDir.isEmpty()
              ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                  + QStringLiteral("/project-library")
              : clean(cacheDir))
{
    QDir().mkpath(m_root);
    QDir().mkpath(m_cacheDir + QStringLiteral("/posters"));
    m_pool.setMaxThreadCount(kWorkerThreads);
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushDelayMs);
    connect(&m_flushTimer, &QTimer::timeout, this, &ProjectLibrary::flushCache);
    loadCache();
}

ProjectLibrary::~ProjectLibrary()
{
    m_pool.clear();
    m_pool.waitForDone(); // jobs still post results; they are dropped with this object
    flushCache();
}

bool ProjectLibrary::contains(const QString& path) const
{
    const QString p = clean(path);
    return p == m_root || p.startsWith(m_root + QLatin1Char('/'));
}

// ---- Listing ----

ProjectEntry ProjectLibrary::entryFor(const QString& path)
{
    const QFileInfo info(path);
    ProjectEntry e;
    e.path = clean(path);
    e.exists = info.exists();
    e.isFolder = info.isDir();
    e.name = e.isFolder ? info.fileName() : info.completeBaseName();
    e.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    e.size = e.isFolder ? 0 : info.size();
    if (const ProjectKind* kind = projectkinds::forPath(path); kind && !e.isFolder)
        e.kindId = kind->id;
    if (!e.exists)
        e.modifiedMs = 0;
    return e;
}

QVector<ProjectEntry> ProjectLibrary::entriesFor(const QStringList& paths)
{
    QVector<ProjectEntry> entries;
    entries.reserve(paths.size());
    for (const QString& p : paths)
        entries.append(entryFor(p));
    return entries;
}

QVector<ProjectEntry> ProjectLibrary::list(const QString& folder) const
{
    // One readdir with the stat data QDirIterator already has: no project file is opened, so
    // hundreds of projects list in a few milliseconds.
    const QString dir = folder.isEmpty() ? m_root : clean(folder);
    QVector<ProjectEntry> folders;
    QVector<ProjectEntry> files;
    QDirIterator it(dir, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        ProjectEntry e;
        e.path = dir + QLatin1Char('/') + info.fileName();
        e.modifiedMs = info.lastModified().toMSecsSinceEpoch();
        if (info.isDir()) {
            e.isFolder = true;
            e.name = info.fileName();
            folders.append(std::move(e));
            continue;
        }
        const ProjectKind* kind = projectkinds::forPath(info.fileName());
        if (!kind)
            continue; // autosave sidecars, media, anything else
        e.name = info.completeBaseName();
        e.size = info.size();
        e.kindId = kind->id;
        files.append(std::move(e));
    }
    folders += files;
    return folders;
}

// ---- Metadata and posters ----

std::optional<ProjectMetadata> ProjectLibrary::metadata(const ProjectEntry& entry)
{
    if (entry.isFolder || !entry.exists)
        return std::nullopt;
    const auto it = m_meta.constFind(entry.path);
    if (it != m_meta.cend() && it->modifiedMs == entry.modifiedMs && it->size == entry.size)
        return it->meta;
    if (m_pendingMeta.contains(entry.path))
        return std::nullopt;
    const ProjectKind* kind = projectkinds::forPath(entry.path);
    if (!kind || !kind->readMetadata)
        return std::nullopt;
    m_pendingMeta.insert(entry.path);
    ++m_inFlight;
    auto reader = kind->readMetadata;
    const ProjectEntry e = entry;
    QPointer<ProjectLibrary> self(this);
    m_pool.start([self, reader, e] {
        ProjectMetadata meta = reader(e.path);
        // `self` is only dereferenced on the GUI thread (the functor's context object).
        QMetaObject::invokeMethod(
            self.data(),
            [self, e, meta = std::move(meta)] {
                if (!self)
                    return;
                --self->m_inFlight;
                self->m_pendingMeta.remove(e.path);
                ++self->m_reads;
                self->m_meta.insert(e.path, {e.modifiedMs, e.size, meta});
                self->scheduleFlush();
                emit self->metadataReady(e.path);
            },
            Qt::QueuedConnection);
    });
    return std::nullopt;
}

ProjectMetadata ProjectLibrary::metadataNow(const ProjectEntry& entry)
{
    if (const auto it = m_meta.constFind(entry.path);
        it != m_meta.cend() && it->modifiedMs == entry.modifiedMs && it->size == entry.size)
        return it->meta;
    const ProjectKind* kind = projectkinds::forPath(entry.path);
    ProjectMetadata meta;
    if (!kind || !kind->readMetadata) {
        meta.error = tr("Unknown project kind");
        return meta;
    }
    meta = kind->readMetadata(entry.path);
    ++m_reads;
    m_meta.insert(entry.path, {entry.modifiedMs, entry.size, meta});
    scheduleFlush();
    return meta;
}

QString ProjectLibrary::posterFile(const QString& path, qint64 modifiedMs) const
{
    return m_cacheDir + QStringLiteral("/posters/") + pathHash(path) + QLatin1Char('-')
        + QString::number(modifiedMs) + QStringLiteral(".jpg");
}

QImage ProjectLibrary::poster(const ProjectEntry& entry, PosterState* state)
{
    const auto report = [&](PosterState s) {
        if (state)
            *state = s;
    };
    if (entry.isFolder || !entry.exists) {
        report(PosterState::Absent);
        return {};
    }
    auto it = m_posters.find(entry.path);
    if (it != m_posters.end() && it->modifiedMs == entry.modifiedMs) {
        report(it->state);
        return it->image;
    }
    // Not asked for this version yet: look for a cached file on a worker thread.
    m_posters.insert(entry.path, {entry.modifiedMs, PosterState::Loading, {}});
    report(PosterState::Loading);
    ++m_inFlight;
    const QString file = posterFile(entry.path, entry.modifiedMs);
    const QString path = entry.path;
    const qint64 modified = entry.modifiedMs;
    QPointer<ProjectLibrary> self(this);
    m_pool.start([self, file, path, modified] {
        QImage image;
        if (QFileInfo::exists(file))
            image.load(file);
        QMetaObject::invokeMethod(
            self.data(),
            [self, path, modified, image] {
                if (!self)
                    return;
                --self->m_inFlight;
                auto p = self->m_posters.find(path);
                if (p == self->m_posters.end() || p->modifiedMs != modified
                    || p->state != PosterState::Loading)
                    return; // forgotten, replaced by storePoster() or a newer version meanwhile
                p->state = image.isNull() ? PosterState::Absent : PosterState::Ready;
                p->image = image;
                emit self->posterReady(path);
            },
            Qt::QueuedConnection);
    });
    return {};
}

void ProjectLibrary::storePoster(const ProjectEntry& entry, const QImage& image)
{
    if (image.isNull() || entry.isFolder)
        return;
    m_posters.insert(entry.path, {entry.modifiedMs, PosterState::Ready, image});
    emit posterReady(entry.path);
    const QString file = posterFile(entry.path, entry.modifiedMs);
    const QString prefix = pathHash(entry.path) + QLatin1Char('-');
    const QString dir = m_cacheDir + QStringLiteral("/posters");
    ++m_inFlight;
    QPointer<ProjectLibrary> self(this);
    m_pool.start([self, file, prefix, dir, image] {
        // Older posters of the same project (earlier modification times) are stale now.
        for (const QString& old : QDir(dir).entryList({prefix + QLatin1Char('*')}, QDir::Files))
            QFile::remove(dir + QLatin1Char('/') + old);
        image.save(file, "JPG", kPosterJpegQuality);
        QMetaObject::invokeMethod(
            self.data(),
            [self] {
                if (self)
                    --self->m_inFlight;
            },
            Qt::QueuedConnection);
    });
}

void ProjectLibrary::waitForIdle()
{
    while (m_inFlight > 0) {
        m_pool.waitForDone();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
}

void ProjectLibrary::loadCache()
{
    QFile file(m_cacheDir + QStringLiteral("/metadata.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QLatin1String("version")).toInt() != kCacheVersion)
        return; // another layout: rebuilt from the project files
    const QJsonObject entries = root.value(QLatin1String("entries")).toObject();
    m_meta.reserve(entries.size());
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        m_meta.insert(it.key(),
            {o.value(QLatin1String("mtime")).toInteger(),
                o.value(QLatin1String("size")).toInteger(),
                metaFromJson(o.value(QLatin1String("meta")).toObject())});
    }
}

void ProjectLibrary::scheduleFlush()
{
    m_cacheDirty = true;
    m_flushTimer.start();
}

void ProjectLibrary::flushCache()
{
    m_flushTimer.stop();
    if (!m_cacheDirty)
        return;
    m_cacheDirty = false;
    QJsonObject entries;
    for (auto it = m_meta.cbegin(); it != m_meta.cend(); ++it) {
        if (!QFileInfo::exists(it.key()))
            continue; // deleted outside the library: do not carry it forever
        entries.insert(it.key(),
            QJsonObject {{QLatin1String("mtime"), it->modifiedMs},
                {QLatin1String("size"), it->size}, {QLatin1String("meta"), metaToJson(it->meta)}});
    }
    QSaveFile file(m_cacheDir + QStringLiteral("/metadata.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject {{QLatin1String("version"), kCacheVersion},
                                 {QLatin1String("entries"), entries}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

void ProjectLibrary::forget(const QString& path)
{
    const QString prefix = path + QLatin1Char('/');
    const auto drop = [&](auto& hash) {
        for (auto it = hash.begin(); it != hash.end();) {
            if (it.key() == path || it.key().startsWith(prefix))
                it = hash.erase(it);
            else
                ++it;
        }
    };
    drop(m_meta);
    drop(m_posters);
    scheduleFlush();
}

// ---- The open project ----

void ProjectLibrary::setOpenProjectPath(const QString& path)
{
    m_openPath = clean(path);
}

bool ProjectLibrary::isOpenOrContainsOpen(const QString& path) const
{
    if (m_openPath.isEmpty())
        return false;
    const QString p = clean(path);
    return m_openPath == p || m_openPath.startsWith(p + QLatin1Char('/'));
}

bool ProjectLibrary::refuseOpen(const QString& path, const char* what, QString* error) const
{
    if (!isOpenOrContainsOpen(path))
        return false;
    setError(error,
        QFileInfo(path).isDir()
            ? tr("This folder contains the open project, so it cannot be %1.").arg(tr(what))
            : tr("This project is open, so it cannot be %1. Open another project first.")
                  .arg(tr(what)));
    return true;
}

// ---- Operations ----

bool ProjectLibrary::isValidName(const QString& name, QString* error)
{
    const QString n = name.trimmed();
    if (n.isEmpty())
        return setError(error, tr("The name is empty."));
    if (n == QLatin1String(".") || n == QLatin1String("..") || n.contains(QLatin1Char('/'))
        || n.contains(QLatin1Char('\\')) || n.contains(QChar(0)))
        return setError(error, tr("“%1” cannot be used as a name.").arg(n));
    if (n.startsWith(QLatin1Char('.')))
        return setError(error, tr("A name cannot start with a dot (it would be hidden)."));
    return true;
}

QString ProjectLibrary::uniquePath(const QString& folder, const QString& base, const QString& ext)
{
    const QString suffix = ext.isEmpty() ? QString() : QLatin1Char('.') + ext;
    QString candidate = folder + QLatin1Char('/') + base + suffix;
    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate
            = folder + QLatin1Char('/') + base + QLatin1Char(' ') + QString::number(n) + suffix;
    return candidate;
}

QString ProjectLibrary::createProject(
    const QString& folder, const QString& name, const QString& kindId, QString* error)
{
    if (!isValidName(name, error))
        return {};
    const ProjectKind* kind = projectkinds::byId(kindId);
    if (!kind || !kind->canCreate()) {
        setError(error,
            tr("This build cannot create %1s.").arg(kind ? kind->displayName.toLower() : kindId));
        return {};
    }
    const QString dir = folder.isEmpty() ? m_root : clean(folder);
    const QString path
        = dir + QLatin1Char('/') + name.trimmed() + QLatin1Char('.') + kind->extensions.first();
    if (QFileInfo::exists(path)) {
        setError(error, tr("A project named “%1” already exists here.").arg(name.trimmed()));
        return {};
    }
    QString why;
    if (!QDir().mkpath(dir) || !kind->create(path, name.trimmed(), &why)) {
        setError(error, tr("Cannot create the project: %1").arg(why));
        return {};
    }
    emit changed();
    return path;
}

QString ProjectLibrary::createFolder(const QString& folder, const QString& name, QString* error)
{
    if (!isValidName(name, error))
        return {};
    const QString dir = folder.isEmpty() ? m_root : clean(folder);
    const QString path = dir + QLatin1Char('/') + name.trimmed();
    if (QFileInfo::exists(path)) {
        setError(error, tr("“%1” already exists here.").arg(name.trimmed()));
        return {};
    }
    if (!QDir().mkpath(path)) {
        setError(error, tr("Cannot create the folder %1.").arg(QDir::toNativeSeparators(path)));
        return {};
    }
    emit changed();
    return path;
}

QString ProjectLibrary::rename(const QString& path, const QString& newName, QString* error)
{
    const QString from = clean(path);
    if (!isValidName(newName, error) || refuseOpen(from, "renamed", error))
        return {};
    const QFileInfo info(from);
    if (!info.exists()) {
        setError(error, tr("%1 no longer exists.").arg(QDir::toNativeSeparators(from)));
        return {};
    }
    const QString name = newName.trimmed();
    const QString to = info.absolutePath() + QLatin1Char('/') + name
        + (info.isDir() ? QString() : QLatin1Char('.') + info.suffix());
    if (to != from) {
        // A case-only rename on a case-insensitive file system reports `to` as existing.
        if (QFileInfo::exists(to) && to.compare(from, Qt::CaseInsensitive) != 0) {
            setError(error, tr("“%1” already exists here.").arg(name));
            return {};
        }
        if (!QFile::rename(from, to)) {
            setError(error, tr("Cannot rename %1.").arg(QDir::toNativeSeparators(from)));
            return {};
        }
    }
    if (!info.isDir()) {
        // The name inside the file follows (the editor's title and Save As use it).
        if (const ProjectKind* kind = projectkinds::forPath(to); kind && kind->writeName) {
            QString why;
            if (!kind->writeName(to, name, &why))
                qWarning(
                    "ProjectLibrary: renamed, but the stored name was kept: %s", qPrintable(why));
        }
    }
    forget(from);
    emit changed();
    return to;
}

QString ProjectLibrary::duplicate(const QString& path, QString* error)
{
    const QString from = clean(path);
    const QFileInfo info(from);
    if (!info.isFile()) {
        setError(error, tr("Only projects can be duplicated."));
        return {};
    }
    const QString to
        = uniquePath(info.absolutePath(), info.completeBaseName() + tr(" copy"), info.suffix());
    if (!QFile::copy(from, to)) {
        setError(error, tr("Cannot copy %1.").arg(QDir::toNativeSeparators(from)));
        return {};
    }
    if (const ProjectKind* kind = projectkinds::forPath(to); kind && kind->writeName)
        kind->writeName(to, QFileInfo(to).completeBaseName(), nullptr);
    emit changed();
    return to;
}

QString ProjectLibrary::move(const QString& path, const QString& folder, QString* error)
{
    const QString from = clean(path);
    const QString dir = folder.isEmpty() ? m_root : clean(folder);
    const QFileInfo info(from);
    if (info.absolutePath() == dir)
        return from;
    if (refuseOpen(from, "moved", error))
        return {};
    if (info.isDir() && (dir == from || dir.startsWith(from + QLatin1Char('/')))) {
        setError(error, tr("A folder cannot be moved into itself."));
        return {};
    }
    const QString to = dir + QLatin1Char('/') + info.fileName();
    if (QFileInfo::exists(to)) {
        setError(error, tr("“%1” already exists in the target folder.").arg(info.fileName()));
        return {};
    }
    if (!QFile::rename(from, to)) {
        setError(error, tr("Cannot move %1.").arg(QDir::toNativeSeparators(from)));
        return {};
    }
    // A rename keeps the modification time, so the cached metadata stays valid: re-key it.
    if (!info.isDir()) {
        if (auto it = m_meta.constFind(from); it != m_meta.cend())
            m_meta.insert(to, *it);
        if (auto it = m_posters.constFind(from);
            it != m_posters.cend() && it->state == PosterState::Ready)
            storePoster(entryFor(to), it->image);
    }
    forget(from);
    emit changed();
    return to;
}

bool ProjectLibrary::moveToTrash(const QString& path, QString* error)
{
    const QString p = clean(path);
    if (refuseOpen(p, "deleted", error))
        return false;
    if (!QFileInfo::exists(p))
        return setError(error, tr("%1 no longer exists.").arg(QDir::toNativeSeparators(p)));
    if (!QFile::moveToTrash(p))
        return setError(error, tr("Cannot move %1 to the trash.").arg(QDir::toNativeSeparators(p)));
    // Drop the poster files of the trashed project(s) too.
    const QString posters = m_cacheDir + QStringLiteral("/posters");
    for (auto it = m_posters.cbegin(); it != m_posters.cend(); ++it) {
        if (it.key() == p || it.key().startsWith(p + QLatin1Char('/'))) {
            for (const QString& f :
                QDir(posters).entryList({pathHash(it.key()) + QStringLiteral("-*")}, QDir::Files))
                QFile::remove(posters + QLatin1Char('/') + f);
        }
    }
    forget(p);
    emit changed();
    return true;
}

QString ProjectLibrary::importProject(const QString& source, const QString& folder, QString* error)
{
    const QFileInfo info(source);
    if (!info.isFile() || !projectkinds::forPath(source)) {
        setError(error, tr("%1 is not a project file.").arg(QDir::toNativeSeparators(source)));
        return {};
    }
    const QString dir = folder.isEmpty() ? m_root : clean(folder);
    const QString to = uniquePath(dir, info.completeBaseName(), info.suffix());
    // The copy keeps the media paths: they are stored absolute (relative ones are only the
    // fallback when the absolute path is gone), so the copy finds the same media.
    if (!copyRecursively(info.absoluteFilePath(), to)) {
        setError(
            error, tr("Cannot copy %1 into the library.").arg(QDir::toNativeSeparators(source)));
        return {};
    }
    emit changed();
    return to;
}

bool ProjectLibrary::exportProject(const QString& path, const QString& target, QString* error,
    const std::function<bool(qint64, qint64)>& progress, bool* cancelled,
    QStringList* keptLinked) const
{
    const QString from = clean(path);
    const QString to = clean(target);
    if (cancelled)
        *cancelled = false;
    if (from == to)
        return true;
    const bool fromSingle = pack::isContainerFile(from);
    const bool toSingle = pack::isContainerFile(to);
    if (!fromSingle && toSingle) {
        // Export as a single file: Pack (writes "<to>.part", renamed over `to` when complete).
        const pack::PackResult r = pack::packProject(from, to, progress);
        if (cancelled)
            *cancelled = r.status == pack::Status::Cancelled;
        if (keptLinked)
            *keptLinked = r.skipped;
        if (r.status != pack::Status::Ok)
            return setError(error, r.error);
        return true;
    }
    if (fromSingle && !toSingle) {
        // Export as a linked project: Unpack next to the target, then give it the chosen name.
        const QString folder = QFileInfo(to).absolutePath();
        const QString unpacked = QDir(folder).absoluteFilePath(
            QFileInfo(from).completeBaseName() + QStringLiteral(".kkproj"));
        if (QFileInfo::exists(to) && to != unpacked)
            return setError(error, tr("%1 already exists.").arg(QDir::toNativeSeparators(to)));
        if (unpacked != to && QFileInfo::exists(unpacked))
            return setError(
                error, tr("%1 already exists.").arg(QDir::toNativeSeparators(unpacked)));
        if (unpacked == to && QFileInfo::exists(to) && !QFile::remove(to))
            return setError(error, tr("Cannot replace %1.").arg(QDir::toNativeSeparators(to)));
        const pack::UnpackResult r = pack::unpackProject(from, folder, progress);
        if (cancelled)
            *cancelled = r.status == pack::Status::Cancelled;
        if (r.status != pack::Status::Ok)
            return setError(error, r.error);
        if (r.projectFile != to && !QFile::rename(r.projectFile, to))
            return setError(error, tr("Cannot write %1.").arg(QDir::toNativeSeparators(to)));
        if (const ProjectKind* kind = projectkinds::forPath(to); kind && kind->writeName)
            kind->writeName(to, QFileInfo(to).completeBaseName(), nullptr);
        return true;
    }
    if (QFileInfo::exists(to) && !QFile::remove(to))
        return setError(error, tr("Cannot replace %1.").arg(QDir::toNativeSeparators(to)));
    if (!QFile::copy(from, to))
        return setError(error, tr("Cannot write %1.").arg(QDir::toNativeSeparators(to)));
    return true;
}

} // namespace kk
