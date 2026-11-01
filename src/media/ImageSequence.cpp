// SPDX-License-Identifier: GPL-3.0-or-later
// ImageSequence — pattern parsing, directory scans and numbered-file detection.
#include "media/ImageSequence.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace kk::media {

namespace {

/// Frame numbers longer than this are not parsed (int overflow, and almost certainly a date
/// or hash rather than a frame counter).
constexpr int kMaxNumberDigits = 9;

/// `#fps=<num>[/<den>]` at the very end of a sequence path.
const QRegularExpression& fpsSuffixRegex()
{
    static const QRegularExpression kRegex(QStringLiteral("#fps=(\\d{1,9})(?:/(\\d{1,9}))?$"));
    return kRegex;
}

/// A file-name pattern split at its placeholder.
struct ParsedPattern
{
    QString directory; ///< without trailing '/', "." if none
    QString prefix; ///< literal text before the number (unescaped)
    QString suffix; ///< literal text after the number (unescaped)
    int width = 0; ///< zero-padding digits, 0 = unpadded
};

/// Parses the file name of `pattern` ("%%" → '%', exactly one "%d"/"%0Nd").
bool parsePattern(const QString& pattern, ParsedPattern* out)
{
    const int slash = pattern.lastIndexOf(QLatin1Char('/'));
    const QString name = pattern.mid(slash + 1);
    ParsedPattern parsed;
    parsed.directory = slash >= 0 ? pattern.left(std::max(slash, 1)) : QStringLiteral(".");
    bool havePlaceholder = false;
    QString* text = &parsed.prefix;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        if (c != QLatin1Char('%')) {
            text->append(c);
            continue;
        }
        if (i + 1 < name.size() && name[i + 1] == QLatin1Char('%')) {
            text->append(c); // escaped literal '%'
            ++i;
            continue;
        }
        // %d or %0Nd
        int j = i + 1;
        int width = 0;
        if (j < name.size() && name[j] == QLatin1Char('0')) {
            ++j;
            const int digitsStart = j;
            while (j < name.size() && name[j].isDigit()) {
                ++j;
            }
            width = name.mid(digitsStart, j - digitsStart).toInt();
            if (width <= 0 || width > kMaxNumberDigits) {
                return false;
            }
        }
        if (j >= name.size() || name[j] != QLatin1Char('d') || havePlaceholder) {
            return false;
        }
        havePlaceholder = true;
        parsed.width = width;
        text = &parsed.suffix;
        i = j;
    }
    if (!havePlaceholder) {
        return false;
    }
    *out = parsed;
    return true;
}

QString escapePercent(QString text)
{
    return text.replace(QLatin1Char('%'), QLatin1String("%%"));
}

/// Pattern string for a parsed pattern (inverse of parsePattern()).
QString buildPattern(
    const QString& directory, const QString& prefix, int width, const QString& suffix)
{
    // (Not QString::arg(): "%0" would itself be taken as an arg() placeholder.)
    const QString placeholder = width > 0
        ? QLatin1String("%0") + QString::number(width) + QLatin1Char('d')
        : QStringLiteral("%d");
    return directory + QLatin1Char('/') + escapePercent(prefix) + placeholder
        + escapePercent(suffix);
}

/// Frame number of `name` if it matches `p`, else -1.
int matchNumber(const QString& name, const ParsedPattern& p)
{
    const int digits = int(name.size() - p.prefix.size() - p.suffix.size());
    if (digits <= 0 || digits > kMaxNumberDigits || !name.startsWith(p.prefix)
        || !name.endsWith(p.suffix)) {
        return -1;
    }
    const int start = int(p.prefix.size());
    for (int i = 0; i < digits; ++i) {
        if (!name[start + i].isDigit()) {
            return -1;
        }
    }
    const bool leadingZero = digits > 1 && name[start] == QLatin1Char('0');
    if (p.width > 0) {
        // Padded: exactly `width` digits, or more digits without padding (9999 → 10000).
        if (digits < p.width || (digits > p.width && leadingZero)) {
            return -1;
        }
    } else if (leadingZero) {
        return -1; // unpadded patterns never produce leading zeros
    }
    return QStringView(name).mid(start, digits).toInt();
}

SequenceInfo scanParsed(const ParsedPattern& p, const QString& pattern)
{
    SequenceInfo info;
    info.pattern = pattern;
    // Name filter narrows the listing in the OS call; the exact match is done by matchNumber.
    // Wildcard characters in prefix/suffix would break the filter, so only use it when safe.
    QStringList filters;
    static const QRegularExpression kWildcards(QStringLiteral("[*?\\[\\]]"));
    if (!p.prefix.contains(kWildcards) && !p.suffix.contains(kWildcards)) {
        filters << p.prefix + QLatin1Char('*') + p.suffix;
    }
    const QStringList names = QDir(p.directory).entryList(filters, QDir::Files | QDir::Hidden);
    std::vector<int> numbers;
    numbers.reserve(size_t(names.size()));
    for (const QString& name : names) {
        const int number = matchNumber(name, p);
        if (number >= 0) {
            numbers.push_back(number);
        }
    }
    if (numbers.empty()) {
        return info;
    }
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    info.firstNumber = numbers.front();
    info.lastNumber = numbers.back();
    info.fileCount = int(numbers.size());
    int expected = info.firstNumber;
    for (const int number : numbers) {
        for (; expected < number; ++expected) {
            info.missing.append(expected);
        }
        expected = number + 1;
    }
    info.found = info.fileCount >= ImageSequence::kMinFrames;
    return info;
}

} // namespace

bool ImageSequence::isSequencePath(const QString& path)
{
    QString pattern;
    return parsePath(path, &pattern, nullptr);
}

bool ImageSequence::parsePath(const QString& sequencePath, QString* pattern, Rational* frameRate)
{
    QString base = sequencePath;
    Rational rate = kDefaultFrameRate;
    const QRegularExpressionMatch match = fpsSuffixRegex().match(sequencePath);
    if (match.hasMatch()) {
        const int num = match.captured(1).toInt();
        const int den = match.hasCaptured(2) ? match.captured(2).toInt() : 1;
        if (num <= 0 || den <= 0) {
            return false;
        }
        rate = {num, den};
        base.truncate(match.capturedStart());
    }
    // Cheap reject before the full parse: the file name must contain '%'.
    if (base.indexOf(QLatin1Char('%'), base.lastIndexOf(QLatin1Char('/')) + 1) < 0) {
        return false;
    }
    ParsedPattern parsed;
    if (!parsePattern(base, &parsed)) {
        return false;
    }
    if (pattern) {
        *pattern = base;
    }
    if (frameRate) {
        *frameRate = rate;
    }
    return true;
}

QString ImageSequence::makePath(const QString& pattern, Rational frameRate)
{
    if (!frameRate.isValid() || frameRate == kDefaultFrameRate) {
        return pattern;
    }
    QString path = pattern + QStringLiteral("#fps=%1").arg(frameRate.num);
    if (frameRate.den != 1) {
        path += QStringLiteral("/%1").arg(frameRate.den);
    }
    return path;
}

QString ImageSequence::fileForNumber(const QString& pattern, int number)
{
    ParsedPattern p;
    if (!parsePattern(pattern, &p)) {
        return {};
    }
    return p.directory + QLatin1Char('/') + p.prefix
        + QStringLiteral("%1").arg(number, p.width, 10, QLatin1Char('0')) + p.suffix;
}

SequenceInfo ImageSequence::scan(const QString& sequencePath)
{
    QString pattern;
    ParsedPattern parsed;
    if (!parsePath(sequencePath, &pattern, nullptr) || !parsePattern(pattern, &parsed)) {
        return {};
    }
    return scanParsed(parsed, pattern);
}

SequenceInfo ImageSequence::detect(const QString& filePath)
{
    const QFileInfo file(filePath);
    const QString name = file.fileName();
    // The frame number is the last run of digits in the file name (extension included in the
    // search, so "clip.0001.exr" and "0001.jpg" work; "frame_0001.mp4v2"-like suffixes too).
    int end = int(name.size());
    while (end > 0 && !name[end - 1].isDigit()) {
        --end;
    }
    int start = end;
    while (start > 0 && name[start - 1].isDigit()) {
        --start;
    }
    if (end == 0 || end - start > kMaxNumberDigits) {
        return {};
    }
    ParsedPattern p;
    p.directory = file.absolutePath();
    p.prefix = name.left(start);
    p.suffix = name.mid(end);
    const int digits = end - start;
    const bool leadingZero = digits > 1 && name[start] == QLatin1Char('0');

    // Zero-padded numbers → %0Nd. Otherwise both padded-to-this-width (1000..9999) and unpadded
    // (1, 2, ... 10) readings are possible: take the one matching more files (padded on a tie,
    // it is the more specific pattern).
    p.width = digits;
    SequenceInfo padded = scanParsed(p, buildPattern(p.directory, p.prefix, digits, p.suffix));
    if (leadingZero || digits == 1) {
        if (digits == 1) {
            p.width = 0; // single digit: "%d" also covers 10, 11, ... (and "%01d" == "%d")
            padded = scanParsed(p, buildPattern(p.directory, p.prefix, 0, p.suffix));
        }
        return padded;
    }
    p.width = 0;
    SequenceInfo unpadded = scanParsed(p, buildPattern(p.directory, p.prefix, 0, p.suffix));
    return unpadded.fileCount > padded.fileCount ? unpadded : padded;
}

QString ImageSequence::displayName(const QString& sequencePath)
{
    QString pattern;
    ParsedPattern p;
    if (!parsePath(sequencePath, &pattern, nullptr) || !parsePattern(pattern, &p)) {
        return QFileInfo(sequencePath).fileName();
    }
    const SequenceInfo info = scanParsed(p, pattern);
    const auto number
        = [&](int n) { return QStringLiteral("%1").arg(n, p.width, 10, QLatin1Char('0')); };
    const QString range = info.fileCount > 0
        ? number(info.firstNumber) + QLatin1Char('-') + number(info.lastNumber)
        : QStringLiteral("#");
    return p.prefix + QLatin1Char('[') + range + QLatin1Char(']') + p.suffix;
}

} // namespace kk::media
