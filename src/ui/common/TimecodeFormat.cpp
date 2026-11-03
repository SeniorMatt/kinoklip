// SPDX-License-Identifier: GPL-3.0-or-later
// Timecode formatting and parsing.
#include "ui/common/TimecodeFormat.h"

#include <QRegularExpression>

#include <cmath>
#include <cstdlib>

namespace kk::timecode {

int framesPerSecond(Rational fps)
{
    return fps.isValid() ? std::max(1, int(std::lround(fps.toDouble()))) : 25;
}

QString format(int64_t frames, Rational fps)
{
    // Hot path (Edit Index rows, ruler labels, drag info): written digit by digit into a
    // stack buffer, about ten times cheaper than four QString::arg() calls.
    const int64_t fpsInt = framesPerSecond(fps);
    const bool negative = frames < 0;
    const uint64_t f = negative ? uint64_t(0) - uint64_t(frames) : uint64_t(frames);
    const uint64_t ff = f % uint64_t(fpsInt);
    const uint64_t totalSeconds = f / uint64_t(fpsInt);
    const uint64_t ss = totalSeconds % 60;
    const uint64_t mm = (totalSeconds / 60) % 60;
    const uint64_t hh = totalSeconds / 3600;

    // Fields from the right, each at least two digits (like arg(value, 2, 10, '0')).
    constexpr int kMaxChars = 96; // sign + four fields of up to 20 digits + separators
    char16_t buffer[kMaxChars];
    int at = kMaxChars;
    auto field = [&](uint64_t value) {
        int digits = 0;
        do {
            buffer[--at] = char16_t(u'0' + value % 10);
            value /= 10;
            ++digits;
        } while (value > 0);
        if (digits < 2)
            buffer[--at] = u'0';
    };
    field(ff);
    buffer[--at] = u':';
    field(ss);
    buffer[--at] = u':';
    field(mm);
    buffer[--at] = u':';
    field(hh);
    if (negative)
        buffer[--at] = u'-';
    return QString(reinterpret_cast<const QChar*>(buffer + at), kMaxChars - at);
}

std::optional<int64_t> parse(QStringView text, Rational fps)
{
    QStringView t = text.trimmed();
    bool negative = false;
    if (t.startsWith(QLatin1Char('-'))) {
        negative = true;
        t = t.mid(1);
    }
    if (t.isEmpty())
        return std::nullopt;

    static const QRegularExpression separators(QStringLiteral("[:;.]"));
    const QStringList fields = t.toString().split(separators);
    if (fields.size() > 4)
        return std::nullopt;

    // Rightmost field = frames, then seconds, minutes, hours.
    static constexpr int64_t kUnitSeconds[] = {0, 1, 60, 3600};
    const int64_t fpsInt = framesPerSecond(fps);
    int64_t total = 0;
    for (qsizetype i = 0; i < fields.size(); ++i) {
        bool ok = false;
        const int64_t value = fields[fields.size() - 1 - i].toLongLong(&ok);
        if (!ok || value < 0)
            return std::nullopt;
        total += i == 0 ? value : value * kUnitSeconds[i] * fpsInt;
    }
    return negative ? -total : total;
}

} // namespace kk::timecode
