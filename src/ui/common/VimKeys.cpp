// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimKeys.h"

#include <QHash>
#include <QKeyEvent>
#include <QRegularExpression>

namespace kk::vimkeys {

namespace {

struct Named
{
    const char* name;
    int key;
};

const Named kNamed[] = {
    {"Esc", Qt::Key_Escape},
    {"CR", Qt::Key_Return},
    {"Tab", Qt::Key_Tab},
    {"BS", Qt::Key_Backspace},
    {"Del", Qt::Key_Delete},
    {"Ins", Qt::Key_Insert},
    {"Left", Qt::Key_Left},
    {"Right", Qt::Key_Right},
    {"Up", Qt::Key_Up},
    {"Down", Qt::Key_Down},
    {"Home", Qt::Key_Home},
    {"End", Qt::Key_End},
    {"PageUp", Qt::Key_PageUp},
    {"PageDown", Qt::Key_PageDown},
};

int namedKey(const QString& name)
{
    for (const Named& n : kNamed) {
        if (name.compare(QLatin1String(n.name), Qt::CaseInsensitive) == 0)
            return n.key;
    }
    return 0;
}

QString nameOfKey(int key)
{
    if (key == Qt::Key_Enter)
        key = Qt::Key_Return;
    if (key == Qt::Key_Backtab)
        key = Qt::Key_Tab;
    for (const Named& n : kNamed) {
        if (n.key == key)
            return QLatin1String(n.name);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    return {};
}

/// "C-h", "A-x", "C-S-h", "Space", "F5", "Left", "lt": the inside of <...>; the normalised
/// token, or an empty string when it is not a key name.
QString normaliseNamed(const QString& inner)
{
    static const QRegularExpression fkey(QStringLiteral("^[Ff]([0-9]{1,2})$"));
    QString mods;
    QString rest = inner;
    while (rest.size() > 2 && rest.at(1) == QLatin1Char('-')) {
        const QChar m = rest.at(0).toUpper();
        if (m != QLatin1Char('C') && m != QLatin1Char('A') && m != QLatin1Char('S')
            && m != QLatin1Char('M'))
            return {};
        if (m == QLatin1Char('M'))
            return {}; // Alt is <A-x>; Meta chords are not keys of the layer
        if (!mods.contains(m))
            mods += m;
        rest = rest.mid(2);
    }
    // Canonical order C, A, S.
    QString ordered;
    for (const QChar m : {QLatin1Char('C'), QLatin1Char('A'), QLatin1Char('S')}) {
        if (mods.contains(m))
            ordered += m + QStringLiteral("-");
    }
    QString name;
    if (rest.compare(QLatin1String("Space"), Qt::CaseInsensitive) == 0)
        name = QStringLiteral("Space");
    else if (rest.compare(QLatin1String("lt"), Qt::CaseInsensitive) == 0)
        name = QStringLiteral("lt");
    else if (rest.compare(QLatin1String("gt"), Qt::CaseInsensitive) == 0)
        name = QStringLiteral("gt");
    else if (rest.compare(QLatin1String("Enter"), Qt::CaseInsensitive) == 0
        || rest.compare(QLatin1String("Return"), Qt::CaseInsensitive) == 0)
        name = QStringLiteral("CR");
    else if (rest.compare(QLatin1String("Backspace"), Qt::CaseInsensitive) == 0)
        name = QStringLiteral("BS");
    else if (const auto m = fkey.match(rest);
        m.hasMatch() && m.captured(1).toInt() >= 1 && m.captured(1).toInt() <= 24)
        name = QStringLiteral("F") + m.captured(1);
    else if (const int key = namedKey(rest); key != 0)
        name = nameOfKey(key);
    else if (rest.size() == 1 && !ordered.isEmpty())
        name = rest.at(0).isLetter() ? rest.toLower() : rest;
    else
        return {};
    if (ordered.contains(QLatin1Char('S')) && ordered.size() == 2 && name.size() == 1)
        return {}; // <S-x> is just X
    // <lt> and <gt> without modifiers are written as characters.
    if (ordered.isEmpty() && name == QLatin1String("lt"))
        return QStringLiteral("<");
    if (ordered.isEmpty() && name == QLatin1String("gt"))
        return QStringLiteral(">");
    // A lone <Esc> style name keeps its brackets; modifiers go inside.
    return QStringLiteral("<%1%2>").arg(ordered, name);
}

} // namespace

bool isClassToken(const QString& token)
{
    return token.size() >= 5 && token.startsWith(QLatin1Char('{'))
        && token.endsWith(QLatin1Char('}'));
}

bool classMatches(const QString& classToken, const QString& typed)
{
    if (!isClassToken(classToken) || typed.size() != 1)
        return false;
    const QString body = classToken.mid(1, classToken.size() - 2);
    const QChar c = typed.at(0);
    for (qsizetype i = 0; i < body.size(); ++i) {
        if (i + 2 < body.size() && body.at(i + 1) == QLatin1Char('-')) {
            if (c >= body.at(i) && c <= body.at(i + 2))
                return true;
            i += 2;
        } else if (c == body.at(i)) {
            return true;
        }
    }
    return false;
}

QStringList parseKeys(const QString& spec, const QString& leader, QString* error)
{
    QStringList tokens;
    qsizetype i = 0;
    while (i < spec.size()) {
        const QChar c = spec.at(i);
        if (c == QLatin1Char('<')) {
            const qsizetype close = spec.indexOf(QLatin1Char('>'), i + 1);
            if (close > i + 1) {
                const QString inner = spec.mid(i + 1, close - i - 1);
                if (inner.compare(QLatin1String(kLeaderName), Qt::CaseInsensitive) == 0) {
                    tokens.append(leader);
                    i = close + 1;
                    continue;
                }
                const QString normal = normaliseNamed(inner);
                if (!normal.isEmpty()) {
                    tokens.append(normal);
                    i = close + 1;
                    continue;
                }
            }
            tokens.append(QStringLiteral("<")); // a literal <
            ++i;
            continue;
        }
        if (c == QLatin1Char('{')) {
            const qsizetype close = spec.indexOf(QLatin1Char('}'), i + 1);
            const QString cls = close > i ? spec.mid(i, close - i + 1) : QString();
            if (cls.size() >= 5 && cls.contains(QLatin1Char('-'))) {
                tokens.append(cls); // "{a-z}": one typed character of the class
                i = close + 1;
                continue;
            }
        }
        if (c.isSpace()) {
            if (error)
                *error = QObject::tr("Use <Space> for the space key");
            return {};
        }
        if (!c.isPrint()) {
            if (error)
                *error = QObject::tr("Not a key: %1").arg(spec);
            return {};
        }
        tokens.append(QString(c));
        ++i;
    }
    if (tokens.isEmpty() && error)
        *error = QObject::tr("No keys");
    return tokens;
}

QString joinKeys(const QStringList& tokens)
{
    QString out;
    for (const QString& t : tokens)
        out += t == QLatin1String("<") ? QStringLiteral("<lt>") : t;
    return out;
}

QString displayToken(const QString& token)
{
    if (token == QLatin1String("<Space>"))
        return QStringLiteral("SPC");
    if (token == QLatin1String("<CR>"))
        return QStringLiteral("CR");
    if (token == QLatin1String("<Esc>"))
        return QStringLiteral("Esc");
    return token;
}

bool isCharToken(const QString& token)
{
    return token.size() == 1 && token.at(0).isPrint();
}

KeyInfo KeyInfo::from(const QKeyEvent* event)
{
    KeyInfo info;
    info.key = event->key();
    info.modifiers = event->modifiers();
    info.text = event->text();
    info.nativeScanCode = event->nativeScanCode();
    info.nativeVirtualKey = event->nativeVirtualKey();
    return info;
}

QString usCharForEvdev(int evdev, bool shift)
{
    // The main block of a US-QWERTY keyboard: unshifted and shifted characters by evdev code
    // (linux/input-event-codes.h: KEY_1 = 2 ... KEY_SLASH = 53).
    static const QHash<int, QPair<char, char>> table = [] {
        QHash<int, QPair<char, char>> t;
        auto row = [&t](int first, const char* plain, const char* shifted) {
            for (int i = 0; plain[i]; ++i)
                t.insert(first + i, {plain[i], shifted[i]});
        };
        row(2, "1234567890-=", "!@#$%^&*()_+");
        row(16, "qwertyuiop[]", "QWERTYUIOP{}");
        row(30, "asdfghjkl;'`", "ASDFGHJKL:\"~");
        t.insert(43, {'\\', '|'});
        row(44, "zxcvbnm,./", "ZXCVBNM<>?");
        return t;
    }();
    // Evdev 41 is the grave key, which sits left of 1; the row above put it after the quote.
    if (evdev == 40)
        return shift ? QStringLiteral("\"") : QStringLiteral("'");
    if (evdev == 41)
        return shift ? QStringLiteral("~") : QStringLiteral("`");
    const auto it = table.constFind(evdev);
    if (it == table.cend())
        return {};
    return QString(QLatin1Char(shift ? it->second : it->first));
}

int evdevFromNative(quint32 nativeScanCode)
{
    if (nativeScanCode == 0)
        return -1;
#if defined(Q_OS_WIN)
    return int(nativeScanCode & 0xff);
#elif defined(Q_OS_MACOS)
    return -1; // macOS key codes differ: the typed character is used
#else
    return nativeScanCode >= 8 ? int(nativeScanCode) - 8 : -1;
#endif
}

QString KeyTranslator::token(const KeyInfo& k)
{
    const bool ctrl = k.modifiers & Qt::ControlModifier;
    const bool alt = k.modifiers & Qt::AltModifier;
    const bool meta = k.modifiers & Qt::MetaModifier;
    const bool shift = k.modifiers & Qt::ShiftModifier;
    if (meta)
        return {};
    if (k.key == Qt::Key_Shift || k.key == Qt::Key_Control || k.key == Qt::Key_Alt
        || k.key == Qt::Key_Meta || k.key == Qt::Key_AltGr || k.key == Qt::Key_CapsLock)
        return {};
    QString prefix;
    if (ctrl)
        prefix += QStringLiteral("C-");
    if (alt)
        prefix += QStringLiteral("A-");

    // Named keys.
    if (const QString name = nameOfKey(k.key); !name.isEmpty()) {
        if (shift || k.key == Qt::Key_Backtab)
            prefix += QStringLiteral("S-");
        return QStringLiteral("<%1%2>").arg(prefix, name);
    }
    if (k.key == Qt::Key_Space) {
        if (shift && (ctrl || alt))
            prefix += QStringLiteral("S-");
        return QStringLiteral("<%1Space>").arg(prefix);
    }

    // The character the key produced, and where the key is.
    const int evdev = evdevFromNative(k.nativeScanCode);
    QChar typed;
    if (k.text.size() == 1 && k.text.at(0).isPrint() && !k.text.at(0).isSpace())
        typed = k.text.at(0);
    const bool asciiTyped = !typed.isNull() && typed.unicode() < 128;
    const QString usPlain = evdev >= 0 ? usCharForEvdev(evdev, false) : QString();
    const bool letterPosition = !usPlain.isEmpty() && usPlain.at(0).isLetter();
    if (letterPosition && !ctrl && !alt && !typed.isNull())
        m_nonLatin = !asciiTyped;

    // Physical position: when the layout is not Latin (the typed character is not ASCII, or a
    // letter key recently produced one), the US character of the key is the one that counts.
    const bool byPosition = m_physical && evdev >= 0
        && (!typed.isNull() ? (!asciiTyped || m_nonLatin) : (ctrl || alt) && m_nonLatin);
    QChar ch;
    if (byPosition) {
        const QString us = usCharForEvdev(evdev, shift);
        if (!us.isEmpty())
            ch = us.at(0);
    }
    if (ch.isNull() && !typed.isNull() && !(ctrl || alt))
        ch = typed;
    if (ch.isNull() && (ctrl || alt)) {
        // Ctrl / Alt chords: the key code names the letter (the text is a control character).
        if (k.key >= Qt::Key_A && k.key <= Qt::Key_Z)
            ch = QChar(QLatin1Char(char('a' + (k.key - Qt::Key_A))));
        else if (k.key >= 0x20 && k.key < 0x7f)
            ch = QChar(char16_t(k.key));
        else if (!typed.isNull())
            ch = typed;
        if (!ch.isNull() && ch.isLetter()) {
            ch = ch.toLower();
            if (shift)
                prefix += QStringLiteral("S-");
        }
    }
    if (ch.isNull())
        return {};
    if (ctrl || alt)
        return QStringLiteral("<%1%2>").arg(prefix, QString(ch));
    // A plain key: the character carries the Shift state itself.
    return QString(ch);
}

} // namespace kk::vimkeys
