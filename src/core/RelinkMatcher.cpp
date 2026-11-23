// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RelinkMatcher.h"

#include "media/SupportedFormats.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace kk::relink {

namespace {

/// Ranks of the match kinds (higher wins when a file is wanted by several items).
int rankOf(MatchKind kind)
{
    switch (kind) {
    case MatchKind::Manual:
        return 6;
    case MatchKind::Exact:
        return 5;
    case MatchKind::CaseInsensitive:
        return 4;
    case MatchKind::OtherExtension:
        return 3;
    case MatchKind::Pattern:
        return 2;
    case MatchKind::Closest:
        return 1;
    case MatchKind::None:
        break;
    }
    return 0;
}

QString extensionOf(const QString& fileName)
{
    const QString base = baseName(fileName);
    return base.size() == fileName.size() ? QString() : fileName.mid(base.size() + 1);
}

/// Edit distance of @p a and @p b, or maxDistance + 1 as soon as it is certainly larger.
int boundedDistance(const QString& a, const QString& b, int maxDistance)
{
    const int la = int(a.size()), lb = int(b.size());
    if (std::abs(la - lb) > maxDistance)
        return maxDistance + 1;
    std::vector<int> prev(size_t(lb) + 1), cur(size_t(lb) + 1);
    for (int j = 0; j <= lb; ++j)
        prev[size_t(j)] = j;
    for (int i = 1; i <= la; ++i) {
        cur[0] = i;
        int rowMin = cur[0];
        for (int j = 1; j <= lb; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[size_t(j)] = std::min({prev[size_t(j)] + 1, cur[size_t(j) - 1] + 1,
                prev[size_t(j) - 1] + cost});
            rowMin = std::min(rowMin, cur[size_t(j)]);
        }
        if (rowMin > maxDistance)
            return maxDistance + 1;
        std::swap(prev, cur);
    }
    return prev[size_t(lb)];
}

/// The replacement text with "$1" groups written as "\1" (what QString::replace understands).
QString normalizedReplacement(const QString& replace)
{
    QString out;
    out.reserve(replace.size());
    for (qsizetype i = 0; i < replace.size(); ++i) {
        if (replace[i] == QLatin1Char('$') && i + 1 < replace.size() && replace[i + 1].isDigit()) {
            out += QLatin1Char('\\');
            out += replace[++i];
        } else {
            out += replace[i];
        }
    }
    return out;
}

struct FileInfoLite
{
    QString path;
    QString name;
    QString base;
    QString ext;
    int category = 0; ///< SupportedFormats::Category as int
};

struct Candidate
{
    int source = 0;
    int file = 0;
    MatchKind kind = MatchKind::None;
    double quality = 0.0;
};

} // namespace

QString patternError(const QString& find)
{
    if (find.isEmpty())
        return {};
    const QRegularExpression re(find);
    return re.isValid() ? QString() : re.errorString();
}

QString baseName(const QString& fileName)
{
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? fileName.left(dot) : fileName;
}

double nameSimilarity(const QString& a, const QString& b, double floor)
{
    const QString x = a.toCaseFolded(), y = b.toCaseFolded();
    if (x == y)
        return 1.0;
    const int longer = int(std::max(x.size(), y.size()));
    if (longer == 0)
        return 1.0;
    const QString& shorter = x.size() <= y.size() ? x : y;
    const QString& other = x.size() <= y.size() ? y : x;
    double score = 0.0;
    if (shorter.size() >= 3 && other.contains(shorter))
        score = 0.5 + 0.5 * double(shorter.size()) / longer;
    // Edit distance only counts when it can beat the floor (and the containment score).
    const double needed = std::max(floor, score);
    const int maxDistance = std::clamp(int(std::floor((1.0 - needed) * longer)), 0, longer);
    const int distance = boundedDistance(x, y, maxDistance);
    if (distance <= maxDistance)
        score = std::max(score, 1.0 - double(distance) / longer);
    return std::min(score, 0.99);
}

QString kindText(MatchKind kind)
{
    switch (kind) {
    case MatchKind::Exact:
        return QCoreApplication::translate("kk::relink", "Exact name");
    case MatchKind::CaseInsensitive:
        return QCoreApplication::translate("kk::relink", "Other case");
    case MatchKind::OtherExtension:
        return QCoreApplication::translate("kk::relink", "Other extension");
    case MatchKind::Pattern:
        return QCoreApplication::translate("kk::relink", "Pattern");
    case MatchKind::Closest:
        return QCoreApplication::translate("kk::relink", "Closest name");
    case MatchKind::Manual:
        return QCoreApplication::translate("kk::relink", "Chosen by hand");
    case MatchKind::None:
        break;
    }
    return QCoreApplication::translate("kk::relink", "No match");
}

QStringList scanFolder(const QString& folder, bool subfolders, int limit)
{
    const QString root = QDir(folder).absolutePath();
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot,
        subfolders ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    int scanned = 0;
    while (it.hasNext() && scanned++ < limit) {
        const QString path = it.next();
        if (media::SupportedFormats::isLikelyMedia(path))
            files.append(path);
    }
    std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
        const auto da = a.count(QLatin1Char('/')), db = b.count(QLatin1Char('/'));
        return da != db ? da < db : a < b;
    });
    return files;
}

QVector<MatchRow> plan(
    const QVector<Source>& sources, const QStringList& files, const Options& options)
{
    QVector<MatchRow> rows;
    rows.reserve(sources.size());
    for (const Source& s : sources) {
        MatchRow r;
        r.itemId = s.itemId;
        r.oldPath = s.oldPath;
        rows.append(r);
    }
    if (sources.isEmpty() || files.isEmpty())
        return rows;

    auto key = [&](const QString& s) { return options.ignoreCase ? s.toCaseFolded() : s; };
    std::vector<FileInfoLite> list;
    list.reserve(size_t(files.size()));
    QHash<QString, QVector<int>> byName; // exact name
    QHash<QString, QVector<int>> byFolded; // case-folded name
    QHash<QString, QVector<int>> byBase; // key(base name)
    for (const QString& path : files) {
        FileInfoLite f;
        f.path = path;
        f.name = QFileInfo(path).fileName();
        f.base = baseName(f.name);
        f.ext = extensionOf(f.name);
        f.category = int(media::SupportedFormats::categoryFor(path));
        const int index = int(list.size());
        list.push_back(f);
        byName[f.name].append(index);
        byFolded[f.name.toCaseFolded()].append(index);
        byBase[key(f.base)].append(index);
    }

    const QRegularExpression re(options.find);
    const bool usePattern = !options.find.isEmpty() && re.isValid();
    const QString replacement = normalizedReplacement(options.replace);
    std::vector<Candidate> candidates;
    std::vector<QString> oldNames(size_t(sources.size())), oldBases(size_t(sources.size())),
        oldExts(size_t(sources.size()));
    auto usable = [&](const Source& s, const FileInfoLite& f) {
        // A file without a picture cannot stand in for a clip with one.
        return !(s.hasVideo && f.category == int(media::MediaCategory::Audio));
    };

    for (int i = 0; i < sources.size(); ++i) {
        const Source& s = sources[i];
        const QString name = QFileInfo(s.oldPath).fileName();
        oldNames[size_t(i)] = name;
        oldBases[size_t(i)] = baseName(name);
        oldExts[size_t(i)] = extensionOf(name);
        auto offer = [&](const QVector<int>& fs, MatchKind kind, double quality) {
            for (const int f : fs) {
                if (usable(s, list[size_t(f)]))
                    candidates.push_back({i, f, kind, quality});
            }
        };
        offer(byName.value(name), MatchKind::Exact, 1.0);
        if (options.ignoreCase)
            offer(byFolded.value(name.toCaseFolded()), MatchKind::CaseInsensitive, 0.98);
        if (options.ignoreExtension) {
            const QVector<int> same = byBase.value(key(oldBases[size_t(i)]));
            for (const int f : same) {
                if (!usable(s, list[size_t(f)]))
                    continue;
                const FileInfoLite& file = list[size_t(f)];
                // The same extension in another case was offered above; here the extension
                // differs. The same kind of media (video for video) is the safer guess.
                const bool sameKind = media::SupportedFormats::categoryFor(s.oldPath)
                    == media::MediaCategory(file.category);
                candidates.push_back(
                    {i, f, MatchKind::OtherExtension, sameKind ? 0.95 : 0.93});
            }
        }
        if (usePattern) {
            QString target = oldBases[size_t(i)];
            target.replace(re, replacement);
            if (target != oldBases[size_t(i)] && !target.isEmpty()) {
                if (options.ignoreExtension) {
                    offer(byBase.value(key(target)), MatchKind::Pattern, 0.9);
                } else {
                    const QString full = oldExts[size_t(i)].isEmpty()
                        ? target
                        : target + QLatin1Char('.') + oldExts[size_t(i)];
                    offer(options.ignoreCase ? byFolded.value(full.toCaseFolded())
                                             : byName.value(full),
                        MatchKind::Pattern, 0.9);
                }
            }
        }
    }

    // Greedy assignment: the best match overall first, so a file goes to the item that wants
    // it most; equal quality keeps the folder order (shallow files first).
    auto assign = [&](std::vector<Candidate>& cs, std::vector<bool>& taken) {
        std::stable_sort(cs.begin(), cs.end(), [](const Candidate& a, const Candidate& b) {
            if (rankOf(a.kind) != rankOf(b.kind))
                return rankOf(a.kind) > rankOf(b.kind);
            if (a.quality != b.quality)
                return a.quality > b.quality;
            if (a.file != b.file)
                return a.file < b.file;
            return a.source < b.source;
        });
        // Ambiguous: another file of the same kind and quality was possible for the item.
        std::map<std::tuple<int, int, long>, int> ties;
        for (const Candidate& c : cs)
            ++ties[{c.source, rankOf(c.kind), std::lround(c.quality * 1e6)}];
        for (const Candidate& c : cs) {
            MatchRow& row = rows[c.source];
            if (taken[size_t(c.file)] || !row.newPath.isEmpty())
                continue;
            taken[size_t(c.file)] = true;
            row.newPath = list[size_t(c.file)].path;
            row.kind = c.kind;
            row.quality = c.quality;
            row.ambiguous = ties[{c.source, rankOf(c.kind), std::lround(c.quality * 1e6)}] > 1;
        }
    };
    std::vector<bool> taken(list.size(), false);
    assign(candidates, taken);

    if (options.closestName) {
        std::vector<Candidate> near;
        const double minimum = std::clamp(options.minimumSimilarity, 0.0, 1.0);
        for (int i = 0; i < sources.size(); ++i) {
            if (!rows[i].newPath.isEmpty())
                continue;
            QString target = oldBases[size_t(i)];
            if (usePattern)
                target.replace(re, replacement);
            const QString a = options.ignoreExtension ? target : target + QLatin1Char('.') + oldExts[size_t(i)];
            const QString a2 = options.ignoreExtension ? oldBases[size_t(i)] : oldNames[size_t(i)];
            for (size_t f = 0; f < list.size(); ++f) {
                if (taken[f] || !usable(sources[i], list[f]))
                    continue;
                const QString b = options.ignoreExtension ? list[f].base : list[f].name;
                double sim = nameSimilarity(a, b, minimum);
                if (a != a2)
                    sim = std::max(sim, nameSimilarity(a2, b, minimum));
                if (sim >= minimum)
                    near.push_back({i, int(f), MatchKind::Closest, sim});
            }
        }
        assign(near, taken);
    }
    return rows;
}

} // namespace kk::relink
