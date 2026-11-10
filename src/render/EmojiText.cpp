// SPDX-License-Identifier: GPL-3.0-or-later
// EmojiText — see EmojiText.h.
#include "render/EmojiText.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <iterator>

namespace kk::render::emoji {

namespace {

constexpr char32_t kZwj = 0x200D;
constexpr char32_t kVariationText = 0xFE0E;
constexpr char32_t kVariationEmoji = 0xFE0F;
constexpr char32_t kKeycap = 0x20E3;

struct Range
{
    char32_t first;
    char32_t last;
};

/// Code points that show as emoji by themselves (Unicode's Emoji_Presentation), grouped.
constexpr Range kPresentation[] = {{0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
    {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
    {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89}, {0x1FA8F, 0x1FAC6},
    {0x1FACE, 0x1FADC}, {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8}};

/// Code points that are text by default and become emoji with a following U+FE0F.
constexpr Range kTextDefault[] = {{0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C},
    {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x21AA}, {0x2328, 0x2328},
    {0x23CF, 0x23CF}, {0x23ED, 0x23EF}, {0x23F1, 0x23F2}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2},
    {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FC}, {0x2600, 0x27BF},
    {0x2934, 0x2935}, {0x2B05, 0x2B07}, {0x3030, 0x3030}, {0x303D, 0x303D}, {0x3297, 0x3299},
    {0x1F170, 0x1F251}, {0x1F300, 0x1FAFF}};

template<size_t N>
bool inRanges(const Range (&ranges)[N], char32_t cp)
{
    return std::any_of(std::begin(ranges), std::end(ranges),
        [cp](const Range& r) { return cp >= r.first && cp <= r.last; });
}

bool isModifier(char32_t cp)
{
    return (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F)
        || cp == kVariationEmoji || cp == kKeycap;
}

bool isKeycapBase(char32_t cp)
{
    return (cp >= U'0' && cp <= U'9') || cp == U'#' || cp == U'*';
}

/// Does an emoji sequence start at code point @p i of @p u?
bool startsEmoji(const QList<uint>& u, qsizetype i)
{
    const char32_t cp = u[i];
    const char32_t next = i + 1 < u.size() ? u[i + 1] : 0;
    if (isKeycapBase(cp))
        return next == kKeycap
            || (next == kVariationEmoji && i + 2 < u.size() && u[i + 2] == kKeycap);
    if (next == kVariationText)
        return false; // asked to be text
    if (inRanges(kPresentation, cp))
        return true;
    return next == kVariationEmoji && inRanges(kTextDefault, cp);
}

/// End (exclusive) of the emoji sequence starting at @p i.
qsizetype sequenceEnd(const QList<uint>& u, qsizetype i)
{
    qsizetype j = i + 1;
    const bool flag = u[i] >= 0x1F1E6 && u[i] <= 0x1F1FF;
    if (flag && j < u.size() && u[j] >= 0x1F1E6 && u[j] <= 0x1F1FF)
        return j + 1; // two regional indicators are one flag
    while (j < u.size()) {
        if (isModifier(u[j])) {
            ++j;
        } else if (u[j] == kZwj && j + 1 < u.size()
            && (startsEmoji(u, j + 1) || inRanges(kPresentation, u[j + 1])
                || inRanges(kTextDefault, u[j + 1]))) {
            j += 2; // joined by a zero width joiner: the next emoji is part of it
        } else {
            break;
        }
    }
    return j;
}

/// UTF-16 offset of each code point of @p text (and the length at the end).
QList<qsizetype> offsetsOf(const QString& text)
{
    QList<qsizetype> off;
    for (qsizetype i = 0; i < text.size(); ++i) {
        off.append(i);
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate())
            ++i;
    }
    off.append(text.size());
    return off;
}

QMutex g_fontMutex;
bool g_fontRegistered = false;

} // namespace

bool containsEmoji(const QString& text)
{
    // Cheap exit: nothing above U+00A9 means no emoji (keycaps need U+20E3).
    bool wide = false;
    for (const QChar c : text) {
        if (c.unicode() >= 0xA9) {
            wide = true;
            break;
        }
    }
    if (!wide)
        return false;
    const QList<uint> u = text.toUcs4();
    for (qsizetype i = 0; i < u.size(); ++i) {
        if (startsEmoji(u, i))
            return true;
    }
    return false;
}

QVector<Run> split(const QString& line)
{
    QVector<Run> runs;
    const QList<uint> u = line.toUcs4();
    const QList<qsizetype> off = offsetsOf(line);
    auto slice
        = [&](qsizetype from, qsizetype to) { return line.mid(off[from], off[to] - off[from]); };
    qsizetype plainFrom = 0;
    qsizetype i = 0;
    while (i < u.size()) {
        if (!startsEmoji(u, i)) {
            ++i;
            continue;
        }
        if (i > plainFrom)
            runs.append({slice(plainFrom, i), false});
        const qsizetype end = isKeycapBase(u[i]) ? [&] {
            qsizetype j = i + 1;
            while (j < u.size() && (u[j] == kVariationEmoji || u[j] == kKeycap))
                ++j;
            return j;
        }()
                                                 : sequenceEnd(u, i);
        runs.append({slice(i, end), true});
        i = plainFrom = end;
    }
    if (plainFrom < u.size())
        runs.append({slice(plainFrom, u.size()), false});
    return runs;
}

QStringList familiesWithEmoji(const QString& family)
{
    return {family, QStringLiteral("Noto Color Emoji"), QStringLiteral("Apple Color Emoji"),
        QStringLiteral("Segoe UI Emoji"), QStringLiteral("Twemoji Mozilla"),
        QStringLiteral("EmojiOne Color")};
}

bool hasColourFont()
{
    const QStringList families = QFontDatabase::families();
    for (const QString& f : familiesWithEmoji(QString())) {
        if (!f.isEmpty() && families.contains(f))
            return true;
    }
    return false;
}

void registerBundledFont()
{
    QMutexLocker lock(&g_fontMutex);
    if (g_fontRegistered)
        return;
    g_fontRegistered = true;
    if (hasColourFont())
        return; // the system has one: use it
    QStringList candidates;
    if (const QByteArray env = qgetenv("KK_EMOJI_FONT"); !env.isEmpty())
        candidates << QString::fromLocal8Bit(env);
    const QString app = QCoreApplication::applicationDirPath();
    candidates << QDir(app).filePath(QStringLiteral("../share/kinoklip/fonts/NotoColorEmoji.ttf"))
               << QDir(app).filePath(QStringLiteral("fonts/NotoColorEmoji.ttf"))
               << QStringLiteral("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf")
               << QStringLiteral("/usr/share/fonts/noto/NotoColorEmoji.ttf")
               << QStringLiteral(
                      "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf");
    for (const QString& path : std::as_const(candidates)) {
        if (QFile::exists(path) && QFontDatabase::addApplicationFont(path) >= 0)
            return;
    }
}

} // namespace kk::render::emoji
