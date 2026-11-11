// SPDX-License-Identifier: GPL-3.0-or-later
#include "matting/MatteCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>

namespace kk::matting {

MatteCache::MatteCache(QString root)
    : m_root(std::move(root))
{ }

QString MatteCache::folderKey(const QString& mediaPath, qint64 fileSize, qint64 modifiedMs,
    const QString& modelId, int quality)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(mediaPath.toUtf8());
    hash.addData(QByteArray::number(fileSize));
    hash.addData(QByteArray::number(modifiedMs));
    hash.addData(modelId.toUtf8());
    hash.addData(QByteArray::number(quality));
    return QString::fromLatin1(hash.result().toHex().left(24));
}

QString MatteCache::filePath(const QString& folderKey, int64_t ms) const
{
    return m_root + QLatin1Char('/') + folderKey + QLatin1Char('/')
        + QString::number(ms).rightJustified(9, QLatin1Char('0')) + QStringLiteral(".png");
}

QImage MatteCache::load(const QString& folderKey, int64_t ms) const
{
    QImage image(filePath(folderKey, ms));
    if (image.isNull())
        return {};
    return image.convertToFormat(QImage::Format_Grayscale8);
}

void MatteCache::store(const QString& folderKey, int64_t ms, const QImage& matte) const
{
    if (matte.isNull() || matte.format() != QImage::Format_Grayscale8)
        return;
    const QString path = filePath(folderKey, ms);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString tmp = path + QStringLiteral(".tmp");
    if (matte.save(tmp, "PNG", 30))
        QFile::rename(tmp, path);
    else
        QFile::remove(tmp);
}

bool MatteCache::contains(const QString& folderKey, int64_t ms) const
{
    return QFileInfo::exists(filePath(folderKey, ms));
}

QList<int64_t> MatteCache::times(const QString& folderKey) const
{
    QList<int64_t> out;
    const QDir dir(m_root + QLatin1Char('/') + folderKey);
    for (const QString& name : dir.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        bool ok = false;
        const int64_t ms = name.chopped(4).toLongLong(&ok);
        if (ok)
            out.push_back(ms);
    }
    std::sort(out.begin(), out.end());
    return out;
}

qint64 MatteCache::sizeBytes() const
{
    qint64 total = 0;
    QDirIterator it(m_root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

bool MatteCache::clear() const
{
    QDir dir(m_root);
    return !dir.exists() || dir.removeRecursively();
}

void MatteCache::trim(qint64 limitBytes) const
{
    struct Folder
    {
        QString path;
        QDateTime modified;
        qint64 bytes = 0;
    };
    QList<Folder> folders;
    qint64 total = 0;
    const QDir root(m_root);
    for (const QFileInfo& d : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Folder f;
        f.path = d.absoluteFilePath();
        f.modified = d.lastModified();
        QDirIterator it(f.path, QDir::Files);
        while (it.hasNext()) {
            it.next();
            f.bytes += it.fileInfo().size();
        }
        total += f.bytes;
        folders.push_back(f);
    }
    std::sort(folders.begin(), folders.end(),
        [](const Folder& a, const Folder& b) { return a.modified < b.modified; });
    for (const Folder& f : folders) {
        if (total <= limitBytes)
            break;
        QDir(f.path).removeRecursively();
        total -= f.bytes;
    }
}

} // namespace kk::matting
