// SPDX-License-Identifier: GPL-3.0-or-later
#include "pack/ProjectPackage.h"

#include "pack/Crc32.h"
#include "pack/EmbeddedMedia.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

namespace kk::pack {

namespace {

constexpr qint64 kChunk = 4LL * 1024 * 1024;

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

QString absolute(const QString& path)
{
    return QFileInfo(path).absoluteFilePath();
}

/// Where the bytes of a source file are: a plain file, or a range of another container.
struct Source
{
    QString filePath; ///< file to read
    qint64 offset = 0;
    qint64 size = 0;
    qint64 modifiedMs = 0;
    QString identity; ///< absolute path as given (hashed into MediaRecord::sourceKey)
    QString fileName; ///< for the entry name
};

bool resolveSource(const QString& path, Source* s, QString* error)
{
    const QFileInfo info(path);
    s->identity = info.absoluteFilePath();
    s->fileName = info.fileName();
    if (info.isFile()) {
        s->filePath = s->identity;
        s->offset = 0;
        s->size = info.size();
        s->modifiedMs = info.lastModified().toMSecsSinceEpoch();
        return true;
    }
    EntryLocation where;
    if (mayBeEmbeddedPath(path) && locateEmbedded(path, &where, nullptr)) {
        s->filePath = where.containerPath;
        s->offset = where.dataOffset;
        s->size = where.size;
        s->modifiedMs = where.modifiedMs;
        return true;
    }
    setError(error, QStringLiteral("File not found: %1").arg(path));
    return false;
}

/// CRC-32 of a byte range; false when cancelled or unreadable.
bool crcOfRange(const QString& file, qint64 offset, qint64 size, uint32_t* crc,
    const std::function<bool(qint64)>& tick)
{
    RangeFile in;
    if (!in.open(file, offset, size))
        return false;
    std::vector<char> buf(size_t(std::min(kChunk, std::max<qint64>(size, 1))));
    uint32_t c = 0;
    for (qint64 pos = 0; pos < size;) {
        const qint64 n = in.read(pos, buf.data(), qint64(buf.size()));
        if (n <= 0)
            return false;
        c = crc32Update(c, buf.data(), size_t(n));
        pos += n;
        if (tick && !tick(n))
            return false;
    }
    *crc = c;
    return true;
}

/// Byte-wise comparison of two ranges of equal size.
bool equalRanges(const QString& fileA, qint64 offA, const QString& fileB, qint64 offB, qint64 size)
{
    RangeFile a;
    RangeFile b;
    if (!a.open(fileA, offA, size) || !b.open(fileB, offB, size))
        return false;
    std::vector<char> ba(size_t(std::min(kChunk, std::max<qint64>(size, 1))));
    std::vector<char> bb(ba.size());
    for (qint64 pos = 0; pos < size;) {
        const qint64 n = a.read(pos, ba.data(), qint64(ba.size()));
        if (n <= 0 || b.read(pos, bb.data(), n) != n
            || std::memcmp(ba.data(), bb.data(), size_t(n)))
            return false;
        pos += n;
    }
    return true;
}

/// Media pool items that refer to a file (not timelines / compound / multicam clips).
bool isFileItem(const QJsonObject& item)
{
    const QString kind = item.value(QLatin1String("kind")).toString();
    return kind.isEmpty() || kind == QLatin1String("subtitle");
}

/// Calls @p fn for every file item of the document's media pool, writing changes back.
QByteArray rewriteItems(const QByteArray& json, const std::function<void(QJsonObject&)>& fn,
    QJsonObject* rootOut = nullptr)
{
    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject())
        return {};
    QJsonObject root = doc.object();
    QJsonObject pool = root.value(QLatin1String("mediaPool")).toObject();
    QJsonArray items = pool.value(QLatin1String("items")).toArray();
    for (qsizetype i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        if (!isFileItem(item))
            continue;
        fn(item);
        items.replace(i, item);
    }
    pool[QLatin1String("items")] = items;
    root[QLatin1String("mediaPool")] = pool;
    if (rootOut)
        *rootOut = root;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

/// Atomic replace of @p target by @p temp (same folder), then fsync of the folder.
bool replaceFile(const QString& temp, const QString& target, QString* error)
{
    // std::filesystem::rename replaces an existing target atomically (rename(2) on POSIX,
    // MoveFileExW with MOVEFILE_REPLACE_EXISTING on Windows); QFile::rename refuses to.
    const auto native = [](const QString& p) {
#ifdef Q_OS_WIN
        return std::filesystem::path(p.toStdWString());
#else
        return std::filesystem::path(QFile::encodeName(p).toStdString());
#endif
    };
    std::error_code ec;
    std::filesystem::rename(native(temp), native(target), ec);
    if (ec) {
        setError(error,
            QStringLiteral("Cannot replace %1: %2")
                .arg(target, QString::fromStdString(ec.message())));
        return false;
    }
    syncDirectory(QFileInfo(target).absolutePath());
    return true;
}

QString generator()
{
#ifdef KK_APP_NAME
    return QStringLiteral(KK_APP_NAME " " KK_APP_VERSION);
#else
    return QStringLiteral("Kinoklip");
#endif
}

} // namespace

bool isContainerFile(const QString& path)
{
    return QFileInfo(path).suffix().compare(QLatin1String(kContainerSuffix), Qt::CaseInsensitive)
        == 0;
}

// ---- Document forms ----------------------------------------------------------------------------

QByteArray documentToRuntime(const QByteArray& stored, const QString& containerPath)
{
    const QString container = absolute(containerPath);
    return rewriteItems(stored, [&container](QJsonObject& item) {
        const QString entry = item.value(QLatin1String("embedded")).toString();
        if (entry.isEmpty() || !isValidEntryName(entry))
            return;
        item[QLatin1String("path")] = embeddedPath(container, entry);
        item.remove(QLatin1String("relativePath"));
        item.remove(QLatin1String("embedded"));
    });
}

QByteArray documentToStorage(const QByteArray& runtime, const QString& containerPath)
{
    const QString prefix = absolute(containerPath) + QLatin1Char('/');
    return rewriteItems(runtime, [&prefix](QJsonObject& item) {
        const QString path = item.value(QLatin1String("path")).toString();
        if (!path.startsWith(prefix))
            return;
        const QString entry = path.mid(prefix.size());
        item[QLatin1String("embedded")] = entry;
        item[QLatin1String("path")] = entry;
        item[QLatin1String("relativePath")] = entry;
    });
}

QStringList embeddedEntries(const QByteArray& storedDocument)
{
    QStringList names;
    rewriteItems(storedDocument, [&names](QJsonObject& item) {
        const QString entry = item.value(QLatin1String("embedded")).toString();
        if (!entry.isEmpty() && !names.contains(entry))
            names.append(entry);
    });
    return names;
}

// ---- ProjectPackage ----------------------------------------------------------------------------

ProjectPackage::~ProjectPackage() = default;

std::unique_ptr<ProjectPackage> ProjectPackage::create(const QString& path, QString* error)
{
    std::unique_ptr<ProjectPackage> p(new ProjectPackage);
    p->m_path = absolute(path);
    p->m_container = Container::create(p->m_path, error);
    if (!p->m_container)
        return {};
    QByteArray salt(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(salt.data()), 4);
    p->m_salt = salt.toHex();
    invalidateIndexCache(p->m_path);
    if (!p->m_container->addData(
            QLatin1String(kManifestEntry), p->manifestJson(), QDateTime::currentDateTime(), error)
        || !p->m_container->commit(error)) {
        return {};
    }
    return p;
}

std::unique_ptr<ProjectPackage> ProjectPackage::open(
    const QString& path, bool writable, QString* error)
{
    std::unique_ptr<ProjectPackage> p(new ProjectPackage);
    p->m_path = absolute(path);
    p->m_container = Container::open(
        p->m_path, writable ? Container::Mode::ReadWrite : Container::Mode::ReadOnly, error);
    if (!p->m_container || !p->loadManifest(error))
        return {};
    return p;
}

bool ProjectPackage::loadManifest(QString* error)
{
    m_media.clear();
    const ContainerEntry* e = m_container->find(QLatin1String(kManifestEntry));
    if (!e) {
        setError(error, QStringLiteral("%1 has no manifest").arg(m_path));
        return false;
    }
    const QByteArray bytes = m_container->read(*e, error);
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    if (root.value(QLatin1String("format")).toString() != QLatin1String("kinoklip-project")) {
        setError(error, QStringLiteral("%1: unknown manifest format").arg(m_path));
        return false;
    }
    if (root.value(QLatin1String("version")).toInt() > kManifestVersion) {
        setError(error, QStringLiteral("%1 was written by a newer Kinoklip").arg(m_path));
        return false;
    }
    m_salt = root.value(QLatin1String("salt")).toString().toLatin1();
    if (m_salt.isEmpty())
        m_salt = QByteArray::number(QRandomGenerator::system()->generate64(), 16);
    for (const QJsonValue& v : root.value(QLatin1String("media")).toArray()) {
        const QJsonObject o = v.toObject();
        MediaRecord r;
        r.entry = o.value(QLatin1String("entry")).toString();
        r.sourceKey = o.value(QLatin1String("sourceKey")).toString();
        r.size = o.value(QLatin1String("size")).toInteger();
        r.modifiedMs = o.value(QLatin1String("modifiedMs")).toInteger();
        r.crc32 = uint32_t(o.value(QLatin1String("crc32")).toInteger());
        r.addedMs = o.value(QLatin1String("addedMs")).toInteger();
        if (m_container->find(r.entry))
            m_media.append(r);
    }
    return true;
}

QByteArray ProjectPackage::manifestJson() const
{
    QJsonArray media;
    for (const MediaRecord& r : m_media) {
        media.append(QJsonObject {{QLatin1String("entry"), r.entry},
            {QLatin1String("sourceKey"), r.sourceKey}, {QLatin1String("size"), r.size},
            {QLatin1String("modifiedMs"), r.modifiedMs}, {QLatin1String("crc32"), qint64(r.crc32)},
            {QLatin1String("addedMs"), r.addedMs}});
    }
    const QJsonObject root {{QLatin1String("format"), QStringLiteral("kinoklip-project")},
        {QLatin1String("version"), kManifestVersion}, {QLatin1String("generator"), generator()},
        {QLatin1String("document"), QLatin1String(kDocumentEntry)},
        {QLatin1String("salt"), QString::fromLatin1(m_salt)}, {QLatin1String("media"), media}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QString ProjectPackage::sourceKey(const QString& absolutePath, qint64 size, qint64 modifiedMs) const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(m_salt);
    hash.addData(QByteArrayView("\n"));
    hash.addData(absolutePath.toUtf8());
    hash.addData(QByteArrayView("\n"));
    hash.addData(QByteArray::number(size) + '\n' + QByteArray::number(modifiedMs));
    return QString::fromLatin1(hash.result().toHex());
}

const MediaRecord* ProjectPackage::record(const QString& entry) const
{
    for (const MediaRecord& r : m_media) {
        if (r.entry == entry)
            return &r;
    }
    return nullptr;
}

bool ProjectPackage::hasDocument() const
{
    return m_container->find(QLatin1String(kDocumentEntry)) != nullptr;
}

QByteArray ProjectPackage::storedDocument(QString* error) const
{
    const ContainerEntry* e = m_container->find(QLatin1String(kDocumentEntry));
    if (!e) {
        setError(error, QStringLiteral("%1 contains no project yet").arg(m_path));
        return {};
    }
    return m_container->read(*e, error);
}

QByteArray ProjectPackage::document(QString* error) const
{
    const QByteArray stored = storedDocument(error);
    if (stored.isEmpty())
        return {};
    const QByteArray runtime = documentToRuntime(stored, m_path);
    if (runtime.isEmpty())
        setError(error, QStringLiteral("The project document in %1 is not valid JSON").arg(m_path));
    return runtime;
}

bool ProjectPackage::saveDocument(const QByteArray& runtimeDocument, QString* error)
{
    const QByteArray stored = documentToStorage(runtimeDocument, m_path);
    if (stored.isEmpty()) {
        setError(error, QStringLiteral("The project document is not valid JSON"));
        return false;
    }
    return saveStoredDocument(stored, error);
}

namespace {

/// Removes the proxy / optimized-media links of embedded items: they are caches outside the
/// file (and their absolute paths would reveal the user's folders); the application finds
/// proxies again in its proxy folder after loading.
QByteArray withoutEmbeddedCacheLinks(const QByteArray& stored)
{
    QJsonObject root = QJsonDocument::fromJson(stored).object();
    const QJsonArray links = root.value(QLatin1String("mediaLinks")).toArray();
    if (links.isEmpty())
        return stored;
    QSet<QString> embedded;
    for (const QJsonValue& v :
        root.value(QLatin1String("mediaPool")).toObject().value(QLatin1String("items")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.contains(QLatin1String("embedded")))
            embedded.insert(o.value(QLatin1String("id")).toString());
    }
    QJsonArray kept;
    for (const QJsonValue& v : links) {
        if (!embedded.contains(v.toObject().value(QLatin1String("mediaId")).toString()))
            kept.append(v);
    }
    if (kept.size() == links.size())
        return stored;
    root[QLatin1String("mediaLinks")] = kept;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

} // namespace

bool ProjectPackage::saveStoredDocument(const QByteArray& storedDocument, QString* error)
{
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_container->addData(
            QLatin1String(kDocumentEntry), withoutEmbeddedCacheLinks(storedDocument), now, error)
        || !m_container->addData(QLatin1String(kManifestEntry), manifestJson(), now, error)
        || !m_container->commit(error)) {
        m_container->rollback();
        return false;
    }
    invalidateIndexCache(m_path);
    return true;
}

bool ProjectPackage::saveManifest(QString* error)
{
    if (!m_container->addData(
            QLatin1String(kManifestEntry), manifestJson(), QDateTime::currentDateTime(), error)
        || !m_container->commit(error)) {
        m_container->rollback();
        return false;
    }
    invalidateIndexCache(m_path);
    return true;
}

QString ProjectPackage::uniqueEntryName(const QString& fileName) const
{
    // "media/<id>/<original file name>": stable for the life of the entry, independent of bins
    // and clip renames (those live in the document only), and the original name survives for
    // Unpack and standard unzip tools. The id is 8 random hex digits, unique in this file.
    QString base = fileName;
    base.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (base.isEmpty() || base == QLatin1String(".") || base == QLatin1String(".."))
        base = QStringLiteral("media");
    for (;;) {
        const QString id
            = QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, u'0');
        const QString folder = QLatin1String(kMediaPrefix) + id + QLatin1Char('/');
        const bool taken
            = std::any_of(m_container->entries().cbegin(), m_container->entries().cend(),
                [&folder](const ContainerEntry& e) { return e.name.startsWith(folder); });
        if (!taken)
            return folder + base;
    }
}

Status ProjectPackage::addMedia(
    const QStringList& sources, QVector<Added>* added, const Progress& progress, QString* error)
{
    QVector<Added> results;
    QVector<Source> resolved;
    qint64 total = 0;
    for (const QString& path : sources) {
        Source s;
        if (!resolveSource(path, &s, error))
            return Status::Failed;
        resolved.append(s);
        total += s.size;
    }
    const QVector<MediaRecord> mediaBefore = m_media;
    qint64 done = 0;
    bool cancelled = false;
    const auto tick = [&](qint64 n) {
        done += n;
        if (progress && !progress(done, total)) {
            cancelled = true;
            return false;
        }
        return true;
    };
    const auto fail = [&](Status status) {
        m_container->rollback();
        m_media = mediaBefore;
        if (status == Status::Cancelled)
            setError(error, QStringLiteral("Cancelled"));
        return status;
    };
    for (qsizetype i = 0; i < resolved.size(); ++i) {
        const Source& s = resolved[i];
        Added a;
        a.source = sources[i];
        // Already in this very project?
        QString ownContainer;
        QString ownEntry;
        if (s.filePath == m_path && splitEmbeddedPath(sources[i], &ownContainer, &ownEntry)
            && absolute(ownContainer) == m_path) {
            a.entry = ownEntry;
            a.duplicate = true;
        }
        // 1. Same original file (path, size, time) as a stored one, by its salted key.
        const QString key = sourceKey(s.identity, s.size, s.modifiedMs);
        for (const MediaRecord& r : std::as_const(m_media)) {
            if (a.entry.isEmpty() && r.sourceKey == key) {
                a.entry = r.entry;
                a.duplicate = true;
            }
        }
        // 2. Same content under another name: size, then CRC, then the bytes themselves.
        if (a.entry.isEmpty()) {
            uint32_t crc = 0;
            bool haveCrc = false;
            for (const ContainerEntry& e : m_container->entries()) {
                if (!e.name.startsWith(QLatin1String(kMediaPrefix)) || e.size != s.size)
                    continue;
                if (!haveCrc) {
                    if (!crcOfRange(s.filePath, s.offset, s.size, &crc, {}))
                        break;
                    haveCrc = true;
                }
                if (crc == e.crc32
                    && equalRanges(s.filePath, s.offset, m_path, e.dataOffset, s.size)) {
                    a.entry = e.name;
                    a.duplicate = true;
                    break;
                }
            }
        }
        if (a.duplicate) {
            if (!tick(s.size))
                return fail(Status::Cancelled);
        } else {
            a.entry = uniqueEntryName(s.fileName);
            uint32_t crc = 0;
            const Status st = m_container->addFile(a.entry, s.filePath,
                QDateTime::fromMSecsSinceEpoch(s.modifiedMs), progress, error, s.offset, s.size,
                &crc, done, total);
            if (st != Status::Ok)
                return fail(st);
            done += s.size;
            MediaRecord r;
            r.entry = a.entry;
            r.sourceKey = key;
            r.size = s.size;
            r.modifiedMs = s.modifiedMs;
            r.crc32 = crc;
            r.addedMs = QDateTime::currentMSecsSinceEpoch();
            m_media.append(r);
        }
        a.path = embeddedPath(m_path, a.entry);
        results.append(a);
    }
    if (cancelled)
        return fail(Status::Cancelled);
    if (m_container->hasPendingChanges()) {
        if (!m_container->addData(
                QLatin1String(kManifestEntry), manifestJson(), QDateTime::currentDateTime(), error)
            || !m_container->commit(error)) {
            return fail(Status::Failed);
        }
        invalidateIndexCache(m_path);
    }
    if (added)
        *added = results;
    return Status::Ok;
}

QStringList ProjectPackage::unusedMedia() const
{
    const QStringList used = embeddedEntries(storedDocument());
    QStringList unused;
    for (const ContainerEntry& e : m_container->entries()) {
        if (e.name.startsWith(QLatin1String(kMediaPrefix)) && !used.contains(e.name))
            unused.append(e.name);
    }
    return unused;
}

qint64 ProjectPackage::compactedSizeEstimate() const
{
    const QStringList unused = unusedMedia();
    qint64 size = m_container->liveSize();
    for (const QString& name : unused)
        size -= m_container->find(name)->footprint() + 46 + name.toUtf8().size();
    return size;
}

// ---- Pack --------------------------------------------------------------------------------------

PackResult packProject(
    const QString& projectFile, const QString& containerPath, const Progress& progress)
{
    PackResult result;
    QFile file(projectFile);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot read %1: %2").arg(projectFile, file.errorString());
        return result;
    }
    const QByteArray json = file.readAll();
    const QDir projectDir = QFileInfo(projectFile).absoluteDir();

    // Which file does every item use? (The same resolution order as the project loader.)
    QStringList sources;
    QHash<QString, QString> sourceOfPath; // item path -> source path
    const QByteArray parsed = rewriteItems(json, [&](QJsonObject& item) {
        const QString path = item.value(QLatin1String("path")).toString();
        const QString name = item.value(QLatin1String("name")).toString(path);
        if (path.isEmpty())
            return;
        if (path.contains(QLatin1Char('%'))) {
            // TODO(pack): image sequences (one entry per frame + sequence routing over entries).
            result.skipped.append(QStringLiteral("%1: image sequence (stays linked)").arg(name));
            return;
        }
        QString source;
        if (QFileInfo(path).isFile()
            || (mayBeEmbeddedPath(path) && splitEmbeddedPath(path, nullptr, nullptr))) {
            source = path;
        } else {
            const QString rel = item.value(QLatin1String("relativePath")).toString();
            const QString candidate = rel.isEmpty() ? QString() : projectDir.absoluteFilePath(rel);
            if (!candidate.isEmpty() && QFileInfo(candidate).isFile())
                source = candidate;
        }
        if (source.isEmpty()) {
            result.skipped.append(QStringLiteral("%1: media offline (stays linked)").arg(name));
            return;
        }
        sourceOfPath.insert(path, source);
        if (!sources.contains(source))
            sources.append(source);
    });
    if (parsed.isEmpty()) {
        result.error = QStringLiteral("%1 is not a project file").arg(projectFile);
        return result;
    }

    const QString target = absolute(containerPath);
    const QString temp = target + QStringLiteral(".part");
    {
        std::unique_ptr<ProjectPackage> package = ProjectPackage::create(temp, &result.error);
        if (!package)
            return result;
        QVector<ProjectPackage::Added> added;
        result.status = package->addMedia(sources, &added, progress, &result.error);
        if (result.status != Status::Ok) {
            package.reset();
            QFile::remove(temp);
            return result;
        }
        QHash<QString, QString> entryOfSource;
        for (const ProjectPackage::Added& a : std::as_const(added)) {
            entryOfSource.insert(a.source, a.entry);
            if (!a.duplicate) {
                ++result.embedded;
                result.bytesCopied += package->container().find(a.entry)->size;
            }
        }
        const QByteArray stored = rewriteItems(json, [&](QJsonObject& item) {
            const QString source = sourceOfPath.value(item.value(QLatin1String("path")).toString());
            const QString entry = entryOfSource.value(source);
            if (entry.isEmpty())
                return;
            item[QLatin1String("embedded")] = entry;
            item[QLatin1String("path")] = entry;
            item[QLatin1String("relativePath")] = entry;
        });
        if (!package->saveStoredDocument(stored, &result.error)) {
            result.status = Status::Failed;
            package.reset();
            QFile::remove(temp);
            return result;
        }
    }
    if (!replaceFile(temp, target, &result.error)) {
        QFile::remove(temp);
        result.status = Status::Failed;
        return result;
    }
    invalidateIndexCache(target);
    result.status = Status::Ok;
    return result;
}

// ---- Unpack ------------------------------------------------------------------------------------

ExtractResult extractMedia(const QString& containerPath, const QString& entryName,
    const QString& target, const Progress& progress)
{
    ExtractResult result;
    EntryLocation loc;
    if (!locateEntry(containerPath, entryName, &loc, &result.error))
        return result;
    const QString temp = target + QStringLiteral(".part");
    const auto fail = [&](Status status, const QString& why) {
        QFile::remove(temp);
        result.status = status;
        result.error = why;
        return result;
    };
    RangeFile in;
    QFile out(temp);
    if (!in.open(loc.containerPath, loc.dataOffset, loc.size)
        || !out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
    std::vector<char> buf(size_t(std::min(kChunk, std::max<qint64>(loc.size, 1))));
    uint32_t crc = 0;
    for (qint64 pos = 0; pos < loc.size;) {
        const qint64 n = in.read(pos, buf.data(), qint64(buf.size()));
        if (n <= 0 || out.write(buf.data(), n) != n)
            return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
        crc = crc32Update(crc, buf.data(), size_t(n));
        pos += n;
        if (progress && !progress(pos, loc.size))
            return fail(Status::Cancelled, QStringLiteral("Cancelled"));
    }
    if (crc != loc.crc32)
        return fail(Status::Failed,
            QStringLiteral("%1 is damaged (CRC mismatch)").arg(QFileInfo(entryName).fileName()));
    if (!syncFile(out))
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
    if (loc.modifiedMs > 0)
        out.setFileTime(
            QDateTime::fromMSecsSinceEpoch(loc.modifiedMs), QFileDevice::FileModificationTime);
    out.close();
    QFile::remove(target);
    if (!QFile::rename(temp, target))
        return fail(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
    result.status = Status::Ok;
    return result;
}

UnpackResult unpackProject(
    const QString& containerPath, const QString& folder, const Progress& progress)
{
    UnpackResult result;
    const std::unique_ptr<ProjectPackage> package
        = ProjectPackage::open(containerPath, false, &result.error);
    if (!package)
        return result;
    const QByteArray stored = package->storedDocument(&result.error);
    if (stored.isEmpty())
        return result;
    const QDir dir(QFileInfo(folder).absoluteFilePath());
    const QString projectFile = dir.absoluteFilePath(
        QFileInfo(containerPath).completeBaseName() + QStringLiteral(".kkproj"));

    QVector<ContainerEntry> media;
    qint64 total = 0;
    for (const ContainerEntry& e : package->container().entries()) {
        if (!e.name.startsWith(QLatin1String(kMediaPrefix)))
            continue;
        media.append(e);
        total += e.size;
    }
    // Never overwrite: check every target first.
    if (QFileInfo::exists(projectFile)) {
        result.error = QStringLiteral("%1 already exists").arg(projectFile);
        return result;
    }
    for (const ContainerEntry& e : std::as_const(media)) {
        if (QFileInfo::exists(dir.absoluteFilePath(e.name))) {
            result.error = QStringLiteral("%1 already exists").arg(dir.absoluteFilePath(e.name));
            return result;
        }
    }
    qint64 done = 0;
    std::vector<char> buf(static_cast<size_t>(kChunk));
    QStringList written;
    const auto abort = [&](Status status, const QString& why) {
        for (const QString& f : std::as_const(written))
            QFile::remove(f);
        result.status = status;
        result.error = why;
        return result;
    };
    for (const ContainerEntry& e : std::as_const(media)) {
        const QString target = dir.absoluteFilePath(e.name);
        const QString temp = target + QStringLiteral(".part");
        QDir().mkpath(QFileInfo(target).absolutePath());
        RangeFile in;
        QFile out(temp);
        if (!in.open(package->path(), e.dataOffset, e.size)
            || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return abort(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
        }
        written.append(temp);
        uint32_t crc = 0;
        for (qint64 pos = 0; pos < e.size;) {
            const qint64 n = in.read(pos, buf.data(), qint64(buf.size()));
            if (n <= 0 || out.write(buf.data(), n) != n)
                return abort(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
            crc = crc32Update(crc, buf.data(), size_t(n));
            pos += n;
            done += n;
            if (progress && !progress(done, total))
                return abort(Status::Cancelled, QStringLiteral("Cancelled"));
        }
        if (crc != e.crc32)
            return abort(
                Status::Failed, QStringLiteral("%1 is damaged (CRC mismatch)").arg(e.name));
        if (!syncFile(out))
            return abort(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
        const MediaRecord* r = package->record(e.name);
        if (r && r->modifiedMs > 0)
            out.setFileTime(
                QDateTime::fromMSecsSinceEpoch(r->modifiedMs), QFileDevice::FileModificationTime);
        out.close();
        if (!QFile::rename(temp, target))
            return abort(Status::Failed, QStringLiteral("Cannot write %1").arg(target));
        written.removeLast();
        written.append(target);
        ++result.extracted;
    }
    // The linked project: absolute paths into the folder, relative paths as a fall-back.
    const QByteArray linked = rewriteItems(stored, [&dir](QJsonObject& item) {
        const QString entry = item.value(QLatin1String("embedded")).toString();
        if (entry.isEmpty())
            return;
        item[QLatin1String("path")] = dir.absoluteFilePath(entry);
        item[QLatin1String("relativePath")] = entry;
        item.remove(QLatin1String("embedded"));
    });
    QFile out(projectFile);
    if (linked.isEmpty() || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || out.write(linked) != linked.size() || !syncFile(out)) {
        return abort(Status::Failed, QStringLiteral("Cannot write %1").arg(projectFile));
    }
    result.projectFile = projectFile;
    result.status = Status::Ok;
    return result;
}

// ---- Compact -----------------------------------------------------------------------------------

CompactResult compactProject(const QString& containerPath, const Progress& progress)
{
    CompactResult result;
    const QString target = absolute(containerPath);
    result.sizeBefore = QFileInfo(target).size();
    const QFileInfo targetInfo(target);
    const QString temp = targetInfo.absolutePath() + QStringLiteral("/.") + targetInfo.fileName()
        + QStringLiteral(".compact-") + QString::number(QCoreApplication::applicationPid())
        + QStringLiteral(".part");
    {
        const std::unique_ptr<ProjectPackage> source
            = ProjectPackage::open(target, false, &result.error);
        if (!source)
            return result;
        const QByteArray stored = source->storedDocument(&result.error);
        if (stored.isNull() && source->hasDocument())
            return result;
        const QStringList used = embeddedEntries(stored);

        QVector<ContainerEntry> keep;
        qint64 total = 0;
        for (const ContainerEntry& e : source->container().entries()) {
            if (e.name == QLatin1String("mimetype") || e.name == QLatin1String(kManifestEntry)
                || e.name == QLatin1String(kDocumentEntry)) {
                continue; // written fresh below
            }
            if (e.name.startsWith(QLatin1String(kMediaPrefix)) && !used.contains(e.name)) {
                result.removedMedia.append(e.name);
                continue;
            }
            keep.append(e); // used media, and entries of later format versions we do not know
            total += e.size;
        }
        std::unique_ptr<ProjectPackage> out = ProjectPackage::create(temp, &result.error);
        if (!out)
            return result;
        const auto abort = [&](Status status) {
            out.reset();
            QFile::remove(temp);
            result.status = status;
            return result;
        };
        qint64 done = 0;
        for (const ContainerEntry& e : std::as_const(keep)) {
            uint32_t crc = 0;
            const Status st = out->container().addFile(e.name, target, e.modified(), progress,
                &result.error, e.dataOffset, e.size, &crc, done, total);
            if (st != Status::Ok)
                return abort(st);
            if (crc != e.crc32) {
                result.error = QStringLiteral("%1 is damaged (CRC mismatch)").arg(e.name);
                return abort(Status::Failed);
            }
            done += e.size;
        }
        QVector<MediaRecord> records;
        for (const MediaRecord& r : source->media()) {
            if (used.contains(r.entry))
                records.append(r);
        }
        out->setMediaRecords(records, source->salt());
        const bool saved = source->hasDocument() ? out->saveStoredDocument(stored, &result.error)
                                                 : out->saveManifest(&result.error);
        if (!saved)
            return abort(Status::Failed);
    }
    QFile::setPermissions(temp, QFile::permissions(target));
    if (!replaceFile(temp, target, &result.error)) {
        QFile::remove(temp);
        result.status = Status::Failed;
        return result;
    }
    invalidateIndexCache(target);
    result.sizeAfter = QFileInfo(target).size();
    result.status = Status::Ok;
    return result;
}

// ---- Verify / repair -------------------------------------------------------------------------

VerifyResult verifyContainer(const QString& containerPath, bool checkData, const Progress& progress)
{
    VerifyResult v;
    QString error;
    const std::unique_ptr<ProjectPackage> package
        = ProjectPackage::open(containerPath, false, &error);
    if (!package) {
        v.problems.append(error);
        return v;
    }
    const Container& c = package->container();
    v.entries = int(c.entries().size());
    v.recovery = c.recovery();
    v.ignoredTailBytes = c.ignoredTailBytes();
    if (v.recovery != Recovery::Clean) {
        v.notes.append(QStringLiteral("The last save was interrupted; the previous saved state "
                                      "is used (%1 bytes ignored). Repair rewrites the end record.")
                           .arg(v.ignoredTailBytes));
    }
    // Entries must not overlap each other or the central directory.
    QVector<ContainerEntry> sorted = c.entries();
    std::sort(sorted.begin(), sorted.end(), [](const ContainerEntry& a, const ContainerEntry& b) {
        return a.headerOffset < b.headerOffset;
    });
    for (qsizetype i = 1; i < sorted.size(); ++i) {
        const ContainerEntry& prev = sorted[i - 1];
        if (prev.dataOffset + prev.size > sorted[i].headerOffset)
            v.problems.append(QStringLiteral("%1 overlaps %2").arg(prev.name, sorted[i].name));
    }
    // Document and manifest.
    const QByteArray stored = package->storedDocument(&error);
    QStringList used;
    if (!package->hasDocument()) {
        v.notes.append(QStringLiteral("No project document saved yet"));
    } else if (stored.isEmpty() || !QJsonDocument::fromJson(stored).isObject()) {
        v.problems.append(QStringLiteral("The project document is damaged: %1").arg(error));
    } else {
        used = embeddedEntries(stored);
        for (const QString& entry : std::as_const(used)) {
            if (!c.find(entry))
                v.problems.append(QStringLiteral("Missing media: %1").arg(entry));
        }
    }
    for (const ContainerEntry& e : c.entries()) {
        if (!e.name.startsWith(QLatin1String(kMediaPrefix)))
            continue;
        const MediaRecord* r = package->record(e.name);
        if (!r)
            v.notes.append(QStringLiteral("%1 is not listed in the manifest").arg(e.name));
        else if (r->size != e.size || r->crc32 != e.crc32)
            v.problems.append(QStringLiteral("%1 does not match its manifest record").arg(e.name));
        if (package->hasDocument() && !used.contains(e.name))
            v.notes.append(
                QStringLiteral("%1 is not used by the project (Compact removes it)").arg(e.name));
    }
    if (checkData) {
        qint64 total = 0;
        for (const ContainerEntry& e : c.entries())
            total += e.size;
        bool cancelled = false;
        for (const ContainerEntry& e : c.entries()) {
            uint32_t crc = 0;
            const bool ok = crcOfRange(package->path(), e.dataOffset, e.size, &crc, [&](qint64 n) {
                v.checkedBytes += n;
                if (progress && !progress(v.checkedBytes, total)) {
                    cancelled = true;
                    return false;
                }
                return true;
            });
            if (cancelled) {
                v.notes.append(QStringLiteral("Cancelled: data check incomplete"));
                break;
            }
            if (!ok)
                v.problems.append(QStringLiteral("%1 cannot be read").arg(e.name));
            else if (crc != e.crc32)
                v.problems.append(QStringLiteral("%1 is damaged (CRC mismatch)").arg(e.name));
        }
    }
    v.ok = v.problems.isEmpty();
    return v;
}

bool repairContainer(const QString& containerPath, QString* error)
{
    const std::unique_ptr<Container> c
        = Container::open(containerPath, Container::Mode::ReadWrite, error);
    if (!c)
        return false;
    if (c->recovery() == Recovery::Clean && c->ignoredTailBytes() == 0)
        return true;
    const bool ok = c->commit(error);
    invalidateIndexCache(absolute(containerPath));
    return ok;
}

} // namespace kk::pack
