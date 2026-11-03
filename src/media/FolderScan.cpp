// SPDX-License-Identifier: GPL-3.0-or-later
// FolderScan — recursive media folder scan with natural sort and image sequence folding.
#include "media/FolderScan.h"

#include "media/SupportedFormats.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace kk::media {

namespace {

/// The progress callback is asked every this many directory entries (cheap enough for huge
/// folders, often enough for a responsive Cancel).
constexpr int kProgressEvery = 64;

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

/// A listed entry of one folder: the name it sorts by and the path it imports as.
struct Entry
{
    QString sortName;
    QString path;
};

/// Splits @p name at its last run of digits (the frame number, as ImageSequence::detect()
/// reads it). Returns false if the name has no digits.
bool splitAtNumber(const QString& name, QString* prefix, QString* suffix)
{
    qsizetype end = name.size();
    while (end > 0 && !isAsciiDigit(name[end - 1]))
        --end;
    if (end == 0)
        return false;
    qsizetype start = end;
    while (start > 0 && isAsciiDigit(name[start - 1]))
        --start;
    *prefix = name.left(start);
    *suffix = name.mid(end);
    return true;
}

/// Folds numbered stills of one folder into sequence entries. @p stills are file names of
/// one folder (images only). Files sharing prefix and suffix around the number are candidate
/// groups; ImageSequence::detect() then decides (same rules as everywhere else: padding,
/// minimum frame count). One detect() (one directory listing) per sequence found, none for
/// files that have no numbered sibling, so a folder of 10 000 frames costs one listing.
void foldSequences(
    const QString& dir, const QStringList& stills, const Rational& fps, std::vector<Entry>* out)
{
    QHash<QString, QStringList> groups; // prefix '\0' suffix -> names
    QStringList order; // group keys in first-seen order (deterministic output)
    for (const QString& name : stills) {
        QString prefix;
        QString suffix;
        if (!splitAtNumber(name, &prefix, &suffix)) {
            out->push_back({name, dir + QLatin1Char('/') + name});
            continue;
        }
        const QString key = prefix + QChar(0) + suffix;
        auto it = groups.find(key);
        if (it == groups.end()) {
            order.append(key);
            it = groups.insert(key, {});
        }
        it->append(name);
    }
    for (const QString& key : std::as_const(order)) {
        QStringList names = groups.value(key);
        if (names.size() < ImageSequence::kMinFrames) {
            for (const QString& name : std::as_const(names))
                out->push_back({name, dir + QLatin1Char('/') + name});
            continue;
        }
        std::sort(names.begin(), names.end(),
            [](const QString& a, const QString& b) { return naturalLess(a, b); });
        QSet<QString> consumed;
        for (const QString& name : std::as_const(names)) {
            if (consumed.contains(name))
                continue;
            const QString path = dir + QLatin1Char('/') + name;
            const SequenceInfo info = ImageSequence::detect(path);
            bool self = false;
            if (info.found) {
                for (int n = info.firstNumber; n <= info.lastNumber; ++n) {
                    const QString member
                        = QFileInfo(ImageSequence::fileForNumber(info.pattern, n)).fileName();
                    consumed.insert(member);
                    self = self || member == name;
                }
            }
            if (!self) {
                // A numbered file of another padding than the run detect() picked: on its own.
                consumed.insert(name);
                out->push_back({name, path});
                continue;
            }
            out->push_back({name, ImageSequence::makePath(info.pattern, fps)});
        }
    }
}

struct Walker
{
    const FolderScanOptions& options;
    const QSet<QString>& extensions;
    const QSet<QString>& stillExtensions;
    const FolderScanProgress& progress;
    FolderScanResult& result;
    QSet<QString> visited; // canonical paths of the folders entered
    int seen = 0;

    /// False once the user cancelled.
    bool tick()
    {
        if (++seen % kProgressEvery != 0 || !progress)
            return true;
        if (!progress(seen)) {
            result.cancelled = true;
            return false;
        }
        return true;
    }

    /// Fills @p folder (path and name set) and recurses. False if cancelled.
    bool walk(ScannedFolder& folder, int depth)
    {
        const QDir dir(folder.path);
        // Unsorted: the natural sort below decides the order. Hidden and System so that the
        // skipped entries are counted (and broken symlinks are seen at all).
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Unsorted);
        std::vector<Entry> files;
        QStringList stills;
        std::vector<QFileInfo> subdirs;
        for (const QFileInfo& info : entries) {
            if (!tick())
                return false;
            const QString name = info.fileName();
            if (isHiddenOrJunkName(name) || info.isHidden()) {
                ++result.skippedHidden;
                continue;
            }
            if (info.isDir()) {
                if (options.includeSubfolders)
                    subdirs.push_back(info);
                continue;
            }
            if (!info.isFile()) { // broken symlink, socket, device ...
                ++result.skippedUnsupported;
                continue;
            }
            const QString ext = info.suffix().toLower();
            if (!extensions.contains(ext)) {
                ++result.skippedUnsupported;
                continue;
            }
            if (options.detectSequences && stillExtensions.contains(ext))
                stills.append(name);
            else
                files.push_back({name, info.absoluteFilePath()});
        }
        if (!stills.isEmpty())
            foldSequences(dir.absolutePath(), stills, options.sequenceFrameRate, &files);
        std::sort(files.begin(), files.end(),
            [](const Entry& a, const Entry& b) { return naturalLess(a.sortName, b.sortName); });
        folder.files.reserve(qsizetype(files.size()));
        for (Entry& e : files)
            folder.files.append(std::move(e.path));

        if (depth >= options.maxDepth)
            return true;
        std::sort(subdirs.begin(), subdirs.end(), [](const QFileInfo& a, const QFileInfo& b) {
            return naturalLess(a.fileName(), b.fileName());
        });
        for (const QFileInfo& info : subdirs) {
            // The canonical path names the real folder behind any chain of links: a folder
            // seen before (an ancestor = a loop, or a second link to it) is not entered again.
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty() || visited.contains(canonical)) {
                ++result.skippedLinks;
                continue;
            }
            visited.insert(canonical);
            ScannedFolder sub;
            sub.name = info.fileName();
            sub.path = info.absoluteFilePath();
            if (!walk(sub, depth + 1))
                return false;
            folder.subfolders.push_back(std::move(sub));
        }
        return true;
    }
};

QSet<QString> toSet(const QStringList& list)
{
    return QSet<QString>(list.cbegin(), list.cend());
}

} // namespace

int ScannedFolder::totalFiles() const
{
    int n = int(files.size());
    for (const ScannedFolder& sub : subfolders)
        n += sub.totalFiles();
    return n;
}

FolderScanResult scanMediaFolder(
    const QString& folder, const FolderScanOptions& options, const FolderScanProgress& progress)
{
    FolderScanResult result;
    const QFileInfo info(folder);
    if (!info.isDir())
        return result;
    static const QSet<QString> kAll = toSet(SupportedFormats::allExtensions());
    static const QSet<QString> kStills
        = toSet(SupportedFormats::extensions(MediaCategory::ImageSequence));
    const QSet<QString>& extensions = options.extensions.isEmpty() ? kAll : options.extensions;
    Walker walker {options, extensions, kStills, progress, result, {}, 0};
    walker.visited.insert(info.canonicalFilePath());
    result.root.path = info.absoluteFilePath();
    result.root.name = info.fileName();
    if (result.root.name.isEmpty()) // the file system root
        result.root.name = result.root.path;
    result.ok = walker.walk(result.root, 0);
    if (!result.ok)
        result.root = {};
    return result;
}

bool isHiddenOrJunkName(QStringView name)
{
    if (name.startsWith(QLatin1Char('.')))
        return true; // also .DS_Store and AppleDouble "._x" files
    for (const char* junk : {"thumbs.db", "desktop.ini", "ehthumbs.db"}) {
        if (name.compare(QLatin1String(junk), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

int naturalCompare(QStringView a, QStringView b)
{
    qsizetype i = 0;
    qsizetype j = 0;
    int zerosTie = 0; // first difference in leading zeros ("1" before "01"), used on a tie
    while (i < a.size() && j < b.size()) {
        if (isAsciiDigit(a[i]) && isAsciiDigit(b[j])) {
            const qsizetype si = i;
            const qsizetype sj = j;
            while (i < a.size() && isAsciiDigit(a[i]))
                ++i;
            while (j < b.size() && isAsciiDigit(b[j]))
                ++j;
            // Compare by value without converting (no overflow for any length): skip leading
            // zeros (keeping one digit), then the longer run is larger, else digit by digit.
            qsizetype zi = si;
            while (zi + 1 < i && a[zi] == QLatin1Char('0'))
                ++zi;
            qsizetype zj = sj;
            while (zj + 1 < j && b[zj] == QLatin1Char('0'))
                ++zj;
            const qsizetype la = i - zi;
            const qsizetype lb = j - zj;
            if (la != lb)
                return la < lb ? -1 : 1;
            for (qsizetype k = 0; k < la; ++k) {
                if (a[zi + k] != b[zj + k])
                    return a[zi + k] < b[zj + k] ? -1 : 1;
            }
            if (zerosTie == 0 && (zi - si) != (zj - sj))
                zerosTie = (zi - si) < (zj - sj) ? -1 : 1;
            continue;
        }
        const QChar ca = a[i].toCaseFolded();
        const QChar cb = b[j].toCaseFolded();
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    if (zerosTie != 0)
        return zerosTie;
    return a.compare(b, Qt::CaseSensitive);
}

} // namespace kk::media
