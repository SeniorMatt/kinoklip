// SPDX-License-Identifier: GPL-3.0-or-later
// TimecodeEntry — typed timecode parsing and the key filter collecting it.
#include "ui/common/TimecodeEntry.h"

#include "ui/common/TimecodeFormat.h"

#include <QCoreApplication>
#include <QKeyEvent>

namespace kk {

namespace {
/// Digits of the HH:MM:SS:FF display (more digits go into the hours).
constexpr int kDisplayDigits = 8;

/// Expands what was typed into its plain digits ('.' = "00", ':' ignored); std::nullopt if
/// it contains anything else. @p sign receives +1 / -1 for an offset, 0 for absolute.
std::optional<QString> digitsOf(const QString& typed, int* sign)
{
    QString text = typed.trimmed();
    *sign = 0;
    if (!text.isEmpty() && (text.front() == QLatin1Char('+') || text.front() == QLatin1Char('-'))) {
        *sign = text.front() == QLatin1Char('-') ? -1 : 1;
        text.remove(0, 1);
    }
    QString digits;
    for (const QChar c : text) {
        if (c.isDigit())
            digits += c;
        else if (c == QLatin1Char('.'))
            digits += QStringLiteral("00");
        else if (c != QLatin1Char(':'))
            return std::nullopt;
    }
    return digits;
}
} // namespace

namespace timecode {

std::optional<TypedTimecode> parseTyped(const QString& typed, Rational fps)
{
    int sign = 0;
    const std::optional<QString> digits = digitsOf(typed, &sign);
    if (!digits || digits->isEmpty())
        return std::nullopt;
    // Right to left: frames, seconds, minutes (two digits each), the rest is hours.
    const QString& d = *digits;
    const auto field = [&](int fromRight, int count) -> int64_t {
        const int end = int(d.size()) - fromRight;
        const int begin = std::max(0, end - count);
        return end > 0 ? d.mid(begin, end - begin).toLongLong() : 0;
    };
    const int64_t ff = field(0, 2);
    const int64_t ss = field(2, 2);
    const int64_t mm = field(4, 2);
    const int64_t hh = field(6, int(d.size()));
    const int64_t frames = ((hh * 60 + mm) * 60 + ss) * framesPerSecond(fps) + ff;
    return TypedTimecode {sign != 0, sign < 0 ? -frames : frames};
}

QString typedDisplay(const QString& typed)
{
    int sign = 0;
    QString digits = digitsOf(typed, &sign).value_or(QString());
    if (digits.size() < kDisplayDigits)
        digits = QString(kDisplayDigits - digits.size(), QLatin1Char('0')) + digits;
    const int n = int(digits.size());
    QString out = digits.left(n - 6) + QLatin1Char(':') + digits.mid(n - 6, 2) + QLatin1Char(':')
        + digits.mid(n - 4, 2) + QLatin1Char(':') + digits.mid(n - 2, 2);
    if (sign != 0)
        out.prepend(sign < 0 ? QLatin1Char('-') : QLatin1Char('+'));
    return out;
}

} // namespace timecode

TimecodeEntry::TimecodeEntry(Predicate accepts, FpsSource fps, QObject* parent)
    : QObject(parent)
    , m_accepts(std::move(accepts))
    , m_fps(std::move(fps))
{
    QCoreApplication::instance()->installEventFilter(this);
}

TimecodeEntry::~TimecodeEntry()
{
    if (QCoreApplication* app = QCoreApplication::instance())
        app->removeEventFilter(this);
}

void TimecodeEntry::cancel()
{
    if (m_typed.isEmpty())
        return;
    m_typed.clear();
    Q_EMIT displayChanged(QString());
}

bool TimecodeEntry::wants(const QKeyEvent* key) const
{
    // Typed text only: Shift is allowed (it makes '+' and ':' on many layouts), keypad too.
    const Qt::KeyboardModifiers mods = key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    if (mods != Qt::NoModifier)
        return false;
    const int k = key->key();
    if (k >= Qt::Key_0 && k <= Qt::Key_9)
        return true;
    if (k == Qt::Key_Plus || k == Qt::Key_Minus)
        return m_typed.isEmpty() || m_typed == QLatin1String("+") || m_typed == QLatin1String("-");
    if (m_typed.isEmpty())
        return false; // '.' alone is the nudge key; editing keys need an open entry
    return k == Qt::Key_Period || k == Qt::Key_Colon || k == Qt::Key_Backspace
        || k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Escape;
}

void TimecodeEntry::handle(const QKeyEvent* key)
{
    const int k = key->key();
    if (k == Qt::Key_Escape) {
        cancel();
        return;
    }
    if (k == Qt::Key_Return || k == Qt::Key_Enter) {
        const std::optional<TypedTimecode> entry = timecode::parseTyped(m_typed, m_fps());
        cancel();
        if (entry)
            Q_EMIT entered(*entry);
        return;
    }
    if (k == Qt::Key_Backspace) {
        m_typed.chop(1);
        Q_EMIT displayChanged(m_typed.isEmpty() ? QString() : timecode::typedDisplay(m_typed));
        return;
    }
    if (k == Qt::Key_Plus || k == Qt::Key_Minus) {
        m_typed = k == Qt::Key_Plus ? QStringLiteral("+") : QStringLiteral("-");
    } else if (k == Qt::Key_Period) {
        m_typed += QLatin1Char('.');
    } else if (k == Qt::Key_Colon) {
        m_typed += QLatin1Char(':');
    } else {
        m_typed += QChar(QLatin1Char(char('0' + (k - Qt::Key_0))));
    }
    Q_EMIT displayChanged(timecode::typedDisplay(m_typed));
}

bool TimecodeEntry::eventFilter(QObject* watched, QEvent* event)
{
    // Cheap type test first: this filter sees every event of the application.
    const QEvent::Type type = event->type();
    if (type != QEvent::ShortcutOverride && type != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);
    auto* key = static_cast<QKeyEvent*>(event);
    if (!m_accepts()) {
        cancel();
        return QObject::eventFilter(watched, event);
    }
    if (!wants(key)) {
        // Any other key ends the entry and then does what it always does (a modifier alone,
        // e.g. the Shift before '+', does not).
        const int k = key->key();
        const bool modifierOnly = k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt
            || k == Qt::Key_Meta || k == Qt::Key_AltGr || k == Qt::Key_unknown;
        if (!modifierOnly)
            cancel();
        return QObject::eventFilter(watched, event);
    }
    if (type == QEvent::ShortcutOverride) {
        // Accepted: no shortcut fires; the key comes back as a KeyPress.
        event->accept();
        return true;
    }
    handle(key);
    event->accept();
    return true;
}

} // namespace kk
