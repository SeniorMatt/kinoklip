// SPDX-License-Identifier: GPL-3.0-or-later
#include "pack/ProjectArchive.h"

#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace kk::pack {

namespace {

constexpr qint64 kChunk = 4LL * 1024 * 1024;
constexpr char kFormat[] = "kinoklip-archive";
constexpr int kVersion = 1;

QString L(const char* s)
{
    return QString::fromLatin1(s);
}

/// Copies byte by byte (4 MiB chunks) from a plain file or an embedded path; counts into @p done.
struct Copier
{
    Progress progress;
    qint64 done = 0;
    qint64 total = 0;
    bool cancelled = false;

    bool tick(qint64 n)
    {
        done += n;
        if (progress && !progress(done, total)) {
            cancelled = true;
            return false;
        }
        return true;
    }

    /// Copies @p source (plain or embedded path) to @p dest; the modification time is kept.
    bool copy(const QString& source, const QString& dest, QString* error)
    {
        QDir().mkpath(QFileInfo(dest).absolutePath());
        RangeFile in;
        if (!in.openPath(source, error))
            return false;
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Cannot write %1").arg(dest);
            return false;
        }
        std::vector<char> buf(size_t(std::min(kChunk, std::max<qint64>(in.size(), 1))));
        for (qint64 pos = 0; pos < in.size();) {
            const qint64 n = in.read(pos, buf.data(), qint64(buf.size()));
            if (n <= 0 || out.write(buf.data(), n) != n) {
                if (error)
                    *error = QStringLiteral("Cannot copy %1").arg(source);
                return false;
            }
            pos += n;
            if (!tick(n))
                return false;
        }
        qint64 modifiedMs = QFileInfo(source).lastModified().toMSecsSinceEpoch();
        EntryLocation where;
        if (!QFileInfo(source).isFile() && mayBeEmbeddedPath(source)
            && locateEmbedded(source, &where, nullptr))
            modifiedMs = where.modifiedMs;
        out.flush();
        if (modifiedMs > 0)
            out.setFileTime(
                QDateTime::fromMSecsSinceEpoch(modifiedMs), QFileDevice::FileModificationTime);
        out.close();
        return true;
    }
};

qint64 sizeOf(const QString& source)
{
    RangeFile in;
    return in.openPath(source, nullptr) ? in.size() : 0;
}

/// Frames of an image sequence pattern "/dir/name_%04d.png" that exist.
QStringList sequenceFrames(const QString& pattern)
{
    const QFileInfo info(pattern);
    static const QRegularExpression digits(QStringLiteral("%0?\\d*d"));
    const QString name = info.fileName();
    const QRegularExpressionMatch m = digits.match(name);
    if (!m.hasMatch())
        return {};
    const QRegularExpression frame(QLatin1Char('^')
        + QRegularExpression::escape(name.left(m.capturedStart()))
        + QStringLiteral("\\d+")
        + QRegularExpression::escape(name.mid(m.capturedEnd())) + QLatin1Char('$'));
    QStringList frames;
    const QDir dir(info.absolutePath());
    for (const QString& f : dir.entryList(QDir::Files, QDir::Name)) {
        if (frame.match(f).hasMatch())
            frames.append(dir.absoluteFilePath(f));
    }
    return frames;
}

/// One file to copy into the archive.
struct Job
{
    QString source;
    QString destRel; ///< below the archive folder
};

/// Decides where every file goes ("Media/003/clip.mp4") and remembers it once per source.
class Plan
{
public:
    QVector<Job> jobs;

    /// Relative path inside the archive for @p source (plain or embedded path); empty if none.
    QString addFile(const QString& group, const QString& source)
    {
        const QString key = group + QLatin1Char('\n') + source;
        const auto it = m_rel.constFind(key);
        if (it != m_rel.constEnd())
            return it.value();
        const QString rel = nextFolder(group) + QFileInfo(source).fileName();
        jobs.append({source, rel});
        m_rel.insert(key, rel);
        return rel;
    }

    /// Same for an image sequence pattern: every frame goes into one folder.
    QString addSequence(const QString& group, const QString& pattern, const QStringList& frames)
    {
        const QString key = group + QLatin1Char('\n') + pattern;
        const auto it = m_rel.constFind(key);
        if (it != m_rel.constEnd())
            return it.value();
        const QString folder = nextFolder(group);
        for (const QString& f : frames)
            jobs.append({f, folder + QFileInfo(f).fileName()});
        const QString rel = folder + QFileInfo(pattern).fileName();
        m_rel.insert(key, rel);
        return rel;
    }

private:
    QString nextFolder(const QString& group)
    {
        const int n = ++m_counter[group];
        return group + QLatin1Char('/') + QString::number(n).rightJustified(3, u'0')
            + QLatin1Char('/');
    }

    QHash<QString, QString> m_rel;
    QHash<QString, int> m_counter;
};

bool isFileItem(const QJsonObject& item)
{
    const QString kind = item.value(QLatin1String("kind")).toString();
    return kind.isEmpty() || kind == QLatin1String("subtitle");
}

/// The file behind a stored path: the path itself, else @p relative against @p baseDir.
/// Plain files, embedded paths and (with a '%' in the name) image sequences.
QString existingSource(const QString& path, const QString& relative, const QDir& baseDir)
{
    const auto usable = [](const QString& p) {
        if (p.isEmpty())
            return false;
        if (p.contains(QLatin1Char('%')))
            return !sequenceFrames(p).isEmpty();
        return QFileInfo(p).isFile() || (mayBeEmbeddedPath(p) && splitEmbeddedPath(p, nullptr, nullptr));
    };
    if (usable(path))
        return path;
    if (!relative.isEmpty()) {
        const QString candidate = QDir::cleanPath(baseDir.absoluteFilePath(relative));
        if (usable(candidate))
            return candidate;
    }
    return {};
}

QString addSource(Plan& plan, const QString& group, const QString& source)
{
    if (source.contains(QLatin1Char('%')))
        return plan.addSequence(group, source, sequenceFrames(source));
    return plan.addFile(group, source);
}

} // namespace

bool isArchiveFolder(const QString& folder)
{
    QFile f(QDir(folder).absoluteFilePath(QLatin1String(kArchiveManifest)));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("format")).toString()
        == QLatin1String(kFormat);
}

ArchiveResult exportArchive(const QByteArray& projectJson, const QString& baseDirPath,
    const QString& projectName, const QString& folderPath, const ArchiveOptions& options,
    const Progress& progress)
{
    ArchiveResult result;
    const QJsonDocument parsed = QJsonDocument::fromJson(projectJson);
    if (!parsed.isObject()) {
        result.error = QStringLiteral("The project document is not valid JSON");
        return result;
    }
    const QDir baseDir(baseDirPath);
    const QDir folder(QFileInfo(folderPath).absoluteFilePath());
    if (folder.exists() && !folder.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        result.error = QStringLiteral("%1 already exists").arg(folder.absolutePath());
        return result;
    }
    const bool createdFolder = !folder.exists();

    QJsonObject root = parsed.object();
    Plan plan;

    // Media pool items.
    QJsonObject pool = root.value(QLatin1String("mediaPool")).toObject();
    QJsonArray items = pool.value(QLatin1String("items")).toArray();
    for (qsizetype i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        const QString path = item.value(QLatin1String("path")).toString();
        if (!isFileItem(item) || path.isEmpty())
            continue;
        const QString source
            = existingSource(path, item.value(QLatin1String("relativePath")).toString(), baseDir);
        if (source.isEmpty()) {
            result.skipped.append(QStringLiteral("%1: media offline (stays linked)")
                                      .arg(item.value(QLatin1String("name")).toString(path)));
            continue;
        }
        const QString rel = addSource(plan, QStringLiteral("Media"), source);
        item[QLatin1String("path")] = folder.absoluteFilePath(rel);
        item[QLatin1String("relativePath")] = rel;
        item.remove(QLatin1String("embedded"));
        items.replace(i, item);
    }
    pool[QLatin1String("items")] = items;
    root[QLatin1String("mediaPool")] = pool;

    // Proxy and optimized media: only when asked for, else the links are dropped.
    QJsonArray links;
    for (const QJsonValue& v : root.value(QLatin1String("mediaLinks")).toArray()) {
        QJsonObject link = v.toObject();
        const struct
        {
            const char* key;
            const char* group;
            bool wanted;
        } kinds[] = {{"proxy", "Proxy", options.proxyMedia},
            {"optimized", "Optimized", options.optimizedMedia}};
        for (const auto& k : kinds) {
            QJsonObject one = link.value(L(k.key)).toObject();
            if (one.isEmpty())
                continue;
            const QString path = one.value(QLatin1String("path")).toString();
            if (!k.wanted || !QFileInfo(path).isFile()) {
                link.remove(L(k.key));
                continue;
            }
            const QString rel = plan.addFile(L(k.group), path);
            one[QLatin1String("path")] = folder.absoluteFilePath(rel);
            one[QLatin1String("archiveRelative")] = rel;
            link[L(k.key)] = one;
        }
        if (link.contains(QLatin1String("proxy")) || link.contains(QLatin1String("optimized")))
            links.append(link);
    }
    if (root.contains(QLatin1String("mediaLinks")))
        root[QLatin1String("mediaLinks")] = links;

    // LUT files.
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    json = rewriteLutKeys(json, [&](QJsonObject& obj, const QString& key) {
        const QString source = existingSource(
            obj.value(key).toString(), obj.value(key + QLatin1String("Relative")).toString(), baseDir);
        if (source.isEmpty()) {
            result.skipped.append(QStringLiteral("%1: LUT file missing (stays linked)")
                                      .arg(QFileInfo(obj.value(key).toString()).fileName()));
            return;
        }
        const QString rel = plan.addFile(QStringLiteral("Luts"), source);
        obj[key] = folder.absoluteFilePath(rel);
        obj[key + QLatin1String("Relative")] = rel;
    });
    if (json.isEmpty()) {
        result.error = QStringLiteral("The project document is not valid JSON");
        return result;
    }

    // Render cache chunks.
    for (const QString& f : options.renderCacheFiles) {
        if (QFileInfo(f).isFile())
            plan.jobs.append({f, QStringLiteral("Cache/") + QFileInfo(f).fileName()});
    }

    // Copy.
    Copier copier;
    copier.progress = progress;
    for (const Job& job : std::as_const(plan.jobs))
        copier.total += sizeOf(job.source);
    const auto fail = [&](Status status, const QString& why) {
        if (createdFolder)
            QDir(folder).removeRecursively();
        else
            for (const QString& sub : {L("Media"), L("Luts"), L("Proxy"), L("Optimized"), L("Cache")})
                QDir(folder.absoluteFilePath(sub)).removeRecursively();
        result.status = status;
        result.error = why;
        return result;
    };
    if (!QDir().mkpath(folder.absolutePath()))
        return fail(Status::Failed, QStringLiteral("Cannot create %1").arg(folder.absolutePath()));
    for (const Job& job : std::as_const(plan.jobs)) {
        QString error;
        if (!copier.copy(job.source, folder.absoluteFilePath(job.destRel), &error))
            return fail(copier.cancelled ? Status::Cancelled : Status::Failed,
                copier.cancelled ? QStringLiteral("Cancelled") : error);
        if (job.destRel.startsWith(QLatin1String("Media/")))
            ++result.mediaFiles;
        else if (job.destRel.startsWith(QLatin1String("Luts/")))
            ++result.lutFiles;
        else if (job.destRel.startsWith(QLatin1String("Cache/")))
            ++result.cacheFiles;
        else
            ++result.proxyFiles;
    }
    result.bytesCopied = copier.done;

    // The project and the manifest.
    const QString projectFileName = QFileInfo(folder.absolutePath()).completeBaseName()
        + QLatin1String(".kkproj");
    result.projectFile = folder.absoluteFilePath(projectFileName);
    QSaveFile project(result.projectFile);
    if (!project.open(QIODevice::WriteOnly) || project.write(json) != json.size()
        || !project.commit())
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(result.projectFile));
    const QJsonObject manifest {{QLatin1String("format"), L(kFormat)},
        {QLatin1String("version"), kVersion}, {QLatin1String("name"), projectName},
        {QLatin1String("project"), projectFileName},
        {QLatin1String("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QLatin1String("media"), result.mediaFiles}, {QLatin1String("luts"), result.lutFiles},
        {QLatin1String("proxies"), result.proxyFiles},
        {QLatin1String("renderCache"), result.cacheFiles}};
    QSaveFile info(folder.absoluteFilePath(QLatin1String(kArchiveManifest)));
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!info.open(QIODevice::WriteOnly) || info.write(manifestBytes) != manifestBytes.size()
        || !info.commit())
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(info.fileName()));
    result.status = Status::Ok;
    return result;
}

RestoreResult restoreArchive(const QString& archiveFolder, const QString& targetFolder,
    const QString& cacheFolder, const Progress& progress)
{
    RestoreResult result;
    const QDir source(QFileInfo(archiveFolder).absoluteFilePath());
    QFile manifestFile(source.absoluteFilePath(QLatin1String(kArchiveManifest)));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("%1 is not a project archive").arg(source.absolutePath());
        return result;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    if (manifest.value(QLatin1String("format")).toString() != QLatin1String(kFormat)) {
        result.error = QStringLiteral("%1 is not a project archive").arg(source.absolutePath());
        return result;
    }
    if (manifest.value(QLatin1String("version")).toInt() > kVersion) {
        result.error = QStringLiteral("%1 was written by a newer Kinoklip").arg(source.absolutePath());
        return result;
    }
    const QString projectName = manifest.value(QLatin1String("project")).toString();
    if (projectName.isEmpty() || !QFileInfo(source.absoluteFilePath(projectName)).isFile()) {
        result.error = QStringLiteral("The archive holds no project file");
        return result;
    }
    const QDir target(QFileInfo(targetFolder).absoluteFilePath());
    if (target.exists() && !target.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        result.error = QStringLiteral("%1 already exists").arg(target.absolutePath());
        return result;
    }
    const bool createdFolder = !target.exists();

    // Everything but the cache goes below the new folder; chunks into the cache folder.
    QStringList files;
    QStringList cacheFiles;
    QDirIterator it(source.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString rel = source.relativeFilePath(it.next());
        if (rel.startsWith(QLatin1String("Cache/")))
            cacheFiles.append(rel);
        else
            files.append(rel);
    }
    files.sort();
    Copier copier;
    copier.progress = progress;
    for (const QString& rel : std::as_const(files))
        copier.total += sizeOf(source.absoluteFilePath(rel));
    if (!cacheFolder.isEmpty()) {
        for (const QString& rel : std::as_const(cacheFiles))
            copier.total += sizeOf(source.absoluteFilePath(rel));
    }
    const auto fail = [&](Status status, const QString& why) {
        if (createdFolder)
            QDir(target).removeRecursively();
        result.status = status;
        result.error = why;
        return result;
    };
    if (!QDir().mkpath(target.absolutePath()))
        return fail(Status::Failed, QStringLiteral("Cannot create %1").arg(target.absolutePath()));
    for (const QString& rel : std::as_const(files)) {
        QString error;
        if (!copier.copy(source.absoluteFilePath(rel), target.absoluteFilePath(rel), &error))
            return fail(copier.cancelled ? Status::Cancelled : Status::Failed,
                copier.cancelled ? QStringLiteral("Cancelled") : error);
        ++result.files;
    }
    if (!cacheFolder.isEmpty()) {
        QDir().mkpath(cacheFolder);
        for (const QString& rel : std::as_const(cacheFiles)) {
            const QString dest = QDir(cacheFolder).absoluteFilePath(QFileInfo(rel).fileName());
            if (QFileInfo::exists(dest)) {
                copier.tick(sizeOf(source.absoluteFilePath(rel)));
                continue;
            }
            QString error;
            if (!copier.copy(source.absoluteFilePath(rel), dest, &error)) {
                QFile::remove(dest);
                return fail(copier.cancelled ? Status::Cancelled : Status::Failed,
                    copier.cancelled ? QStringLiteral("Cancelled") : error);
            }
            ++result.cacheFiles;
        }
    }

    // Point the restored project at the copies.
    const QString projectFile = target.absoluteFilePath(projectName);
    QFile in(projectFile);
    if (!in.open(QIODevice::ReadOnly))
        return fail(Status::Failed, QStringLiteral("Cannot read %1").arg(projectFile));
    QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    QJsonObject pool = root.value(QLatin1String("mediaPool")).toObject();
    QJsonArray items = pool.value(QLatin1String("items")).toArray();
    for (qsizetype i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        const QString rel = item.value(QLatin1String("relativePath")).toString();
        if (rel.isEmpty() || QDir::isAbsolutePath(rel)
            || !(rel.startsWith(QLatin1String("Media/"))))
            continue;
        item[QLatin1String("path")] = target.absoluteFilePath(rel);
        items.replace(i, item);
    }
    pool[QLatin1String("items")] = items;
    root[QLatin1String("mediaPool")] = pool;
    QJsonArray links = root.value(QLatin1String("mediaLinks")).toArray();
    for (qsizetype i = 0; i < links.size(); ++i) {
        QJsonObject link = links.at(i).toObject();
        for (const char* key : {"proxy", "optimized"}) {
            QJsonObject one = link.value(L(key)).toObject();
            const QString rel = one.value(QLatin1String("archiveRelative")).toString();
            if (rel.isEmpty())
                continue;
            one[QLatin1String("path")] = target.absoluteFilePath(rel);
            one.remove(QLatin1String("archiveRelative"));
            link[L(key)] = one;
        }
        links.replace(i, link);
    }
    if (root.contains(QLatin1String("mediaLinks")))
        root[QLatin1String("mediaLinks")] = links;
    QByteArray json = rewriteLutKeys(
        QJsonDocument(root).toJson(QJsonDocument::Indented), [&](QJsonObject& obj, const QString& key) {
            const QString rel = obj.value(key + QLatin1String("Relative")).toString();
            if (rel.startsWith(QLatin1String("Luts/")))
                obj[key] = target.absoluteFilePath(rel);
        });
    QSaveFile out(projectFile);
    if (json.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(json) != json.size()
        || !out.commit())
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(projectFile));
    result.projectFile = projectFile;
    result.status = Status::Ok;
    return result;
}

} // namespace kk::pack
