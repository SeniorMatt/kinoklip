// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/CloneTool.h"

#include "pack/Crc32.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <memory>
#include <vector>

namespace kk::clone {

namespace {

constexpr qint64 kBlock = 4 * 1024 * 1024;

/// Job messages stay English: they are a technical report (checksums, paths), like log lines.
QString tr(const char* text)
{
    return QString::fromUtf8(text);
}

/// One running checksum of any of the algorithms.
class Hasher
{
public:
    explicit Hasher(Checksum c)
    {
        if (c != Checksum::Crc32)
            m_hash = std::make_unique<QCryptographicHash>(algorithm(c));
    }
    void add(const char* data, qint64 size)
    {
        if (m_hash)
            m_hash->addData(QByteArrayView(data, size));
        else
            m_crc = pack::crc32Update(m_crc, data, size_t(size));
    }
    [[nodiscard]] QString hex() const
    {
        if (m_hash)
            return QString::fromLatin1(m_hash->resultView().toByteArray().toHex());
        return QStringLiteral("%1").arg(m_crc, 8, 16, QLatin1Char('0'));
    }

private:
    static QCryptographicHash::Algorithm algorithm(Checksum c)
    {
        switch (c) {
        case Checksum::Sha1:
            return QCryptographicHash::Sha1;
        case Checksum::Sha256:
            return QCryptographicHash::Sha256;
        default:
            return QCryptographicHash::Md5;
        }
    }
    std::unique_ptr<QCryptographicHash> m_hash;
    uint32_t m_crc = 0;
};

struct Item
{
    QString sourcePath;
    QString relative; ///< including the name of the source folder
    qint64 size = 0;
    QDateTime modified;
};

/// All regular files of the sources; @p roots receives, per source, the name of its top entry.
QList<Item> collect(const QStringList& sources, QStringList* roots, QString* error)
{
    QList<Item> items;
    for (const QString& source : sources) {
        const QFileInfo info(source);
        if (!info.exists()) {
            if (error)
                *error = tr("%1 does not exist").arg(source);
            return {};
        }
        if (roots)
            roots->append(info.fileName());
        if (info.isFile()) {
            items.append(
                {info.absoluteFilePath(), info.fileName(), info.size(), info.lastModified()});
            continue;
        }
        const QDir base(info.absoluteFilePath());
        QDirIterator it(base.absolutePath(), QDir::Files | QDir::Hidden | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        QList<Item> found;
        while (it.hasNext()) {
            it.next();
            const QFileInfo f = it.fileInfo();
            found.append({f.absoluteFilePath(),
                info.fileName() + QLatin1Char('/') + base.relativeFilePath(f.absoluteFilePath()),
                f.size(), f.lastModified()});
        }
        std::sort(found.begin(), found.end(),
            [](const Item& a, const Item& b) { return a.relative < b.relative; });
        items += found;
    }
    return items;
}

} // namespace

QString checksumName(Checksum c)
{
    switch (c) {
    case Checksum::Md5:
        return QStringLiteral("MD5");
    case Checksum::Sha1:
        return QStringLiteral("SHA-1");
    case Checksum::Sha256:
        return QStringLiteral("SHA-256");
    case Checksum::Crc32:
        return QStringLiteral("CRC-32");
    }
    return {};
}

QString checksumSuffix(Checksum c)
{
    switch (c) {
    case Checksum::Md5:
        return QStringLiteral("md5");
    case Checksum::Sha1:
        return QStringLiteral("sha1");
    case Checksum::Sha256:
        return QStringLiteral("sha256");
    case Checksum::Crc32:
        return QStringLiteral("crc32");
    }
    return {};
}

bool parseChecksum(const QString& text, Checksum& out)
{
    const QString t = text.trimmed().toLower().remove(QLatin1Char('-'));
    for (Checksum c : {Checksum::Md5, Checksum::Sha1, Checksum::Sha256, Checksum::Crc32}) {
        if (t == checksumSuffix(c)) {
            out = c;
            return true;
        }
    }
    return false;
}

QString fileChecksum(const QString& path, Checksum c)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    Hasher h(c);
    QByteArray buf(int(kBlock), Qt::Uninitialized);
    for (;;) {
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return {};
        if (n == 0)
            break;
        h.add(buf.constData(), n);
    }
    return h.hex();
}

QList<FileResult> plan(const Job& job, QString* error)
{
    QList<FileResult> out;
    for (const Item& item : collect(job.sources, nullptr, error))
        out.append({item.relative, item.size, {}, {}});
    return out;
}

Result run(const Job& job, const Progress& progress)
{
    Result result;
    if (job.sources.isEmpty()) {
        result.error = tr("Add a source folder or file.");
        return result;
    }
    if (job.destinations.isEmpty()) {
        result.error = tr("Add a destination folder.");
        return result;
    }
    QString error;
    QStringList roots;
    const QList<Item> items = collect(job.sources, &roots, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    qint64 sourceBytes = 0;
    for (const Item& i : items)
        sourceBytes += i.size;
    const qint64 copies = job.destinations.size();
    // Planned work: one read of the source + a write per destination + a read-back per copy.
    const qint64 total = sourceBytes * (1 + copies + (job.verify ? copies : 0));
    qint64 done = 0;
    auto report = [&](qint64 n) {
        done += n;
        if (progress && !progress(done, total))
            result.cancelled = true;
        return !result.cancelled;
    };
    for (const QString& d : job.destinations) {
        if (!QDir().mkpath(d)) {
            result.error = tr("Cannot create %1").arg(d);
            return result;
        }
    }

    struct Target
    {
        QString destination;
        QString path;
        std::unique_ptr<QSaveFile> file;
    };
    QByteArray buf(int(kBlock), Qt::Uninitialized);
    for (const Item& item : items) {
        FileResult fr;
        fr.relative = item.relative;
        fr.size = item.size;
        QFile in(item.sourcePath);
        if (!in.open(QIODevice::ReadOnly)) {
            fr.failures << tr("source: cannot read %1").arg(item.sourcePath);
            for (const QString& f : std::as_const(fr.failures))
                result.failures << QStringLiteral("%1: %2").arg(item.relative, f);
            result.results.append(fr);
            continue;
        }
        // Targets that can take the file: a file that is already there is never overwritten.
        std::vector<Target> targets;
        for (const QString& d : job.destinations) {
            const QString path = QDir(d).filePath(item.relative);
            if (QFileInfo::exists(path)) {
                fr.failures << tr("%1: already exists").arg(d);
                continue;
            }
            QDir().mkpath(QFileInfo(path).absolutePath());
            auto file = std::make_unique<QSaveFile>(path);
            file->setDirectWriteFallback(false);
            if (!file->open(QIODevice::WriteOnly)) {
                fr.failures << tr("%1: cannot write (%2)").arg(d, file->errorString());
                continue;
            }
            targets.push_back({d, path, std::move(file)});
        }
        Hasher hash(job.checksum);
        bool readOk = true;
        for (;;) {
            const qint64 n = in.read(buf.data(), buf.size());
            if (n < 0) {
                readOk = false;
                break;
            }
            if (n == 0)
                break;
            hash.add(buf.constData(), n);
            for (auto it = targets.begin(); it != targets.end();) {
                if (it->file->write(buf.constData(), n) != n) {
                    fr.failures << tr("%1: write failed (%2)")
                                       .arg(it->destination, it->file->errorString());
                    it->file->cancelWriting();
                    it = targets.erase(it);
                } else {
                    ++it;
                }
            }
            if (!report(n * (1 + qint64(targets.size())))) {
                for (Target& t : targets)
                    t.file->cancelWriting();
                return result;
            }
        }
        if (!readOk) {
            for (Target& t : targets)
                t.file->cancelWriting();
            fr.failures << tr("source: read error in %1").arg(item.sourcePath);
            for (const QString& f : std::as_const(fr.failures))
                result.failures << QStringLiteral("%1: %2").arg(item.relative, f);
            result.results.append(fr);
            continue;
        }
        fr.checksum = hash.hex();
        // Commit (the rename of the finished file), keep the time of the source, read back.
        for (Target& t : targets) {
            if (!t.file->commit()) {
                fr.failures
                    << tr("%1: cannot finish (%2)").arg(t.destination, t.file->errorString());
                continue;
            }
            QFile copy(t.path);
            if (copy.open(QIODevice::ReadWrite)) {
                copy.setFileTime(item.modified, QFileDevice::FileModificationTime);
                copy.close();
            }
            if (!job.verify)
                continue;
            const QString again = fileChecksum(t.path, job.checksum);
            if (!report(item.size))
                return result;
            if (again == fr.checksum)
                ++result.verified;
            else
                fr.failures << tr("%1: checksum differs after copying").arg(t.destination);
        }
        for (const QString& f : std::as_const(fr.failures))
            result.failures << QStringLiteral("%1: %2").arg(item.relative, f);
        ++result.files;
        result.bytes += item.size;
        result.results.append(fr);
    }

    // The checksum list of each source next to its copy: "<hash>  <path inside the source>".
    if (job.writeList && !result.cancelled) {
        for (int s = 0; s < roots.size(); ++s) {
            const QString root = roots.at(s);
            QByteArray list;
            for (const FileResult& fr : std::as_const(result.results)) {
                if (fr.checksum.isEmpty()
                    || !(fr.relative == root || fr.relative.startsWith(root + QLatin1Char('/'))))
                    continue;
                const QString inside
                    = fr.relative == root ? root : fr.relative.mid(root.size() + 1);
                list += fr.checksum.toUtf8() + "  " + inside.toUtf8() + '\n';
            }
            if (list.isEmpty())
                continue;
            const bool isFolder = QFileInfo(job.sources.at(s)).isDir();
            for (const QString& d : job.destinations) {
                const QString dir = isFolder ? QDir(d).filePath(root) : d;
                QFile f(QDir(dir).filePath(root + QLatin1Char('.') + checksumSuffix(job.checksum)));
                if (!f.exists() && f.open(QIODevice::WriteOnly))
                    f.write(list);
            }
        }
    }
    result.ok = result.failures.isEmpty() && result.error.isEmpty();
    return result;
}

} // namespace kk::clone
