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
    const int64_t fpsInt = framesPerSecond(fps);
    const bool negative = frames < 0;
    const int64_t f = std::llabs(frames);
    const int64_t ff = f % fpsInt;
    const int64_t totalSeconds = f / fpsInt;
    const int64_t ss = totalSeconds % 60;
    const int64_t mm = (totalSeconds / 60) % 60;
    const int64_t hh = totalSeconds / 3600;
    const QString text = QStringLiteral("%1:%2:%3:%4")
                             .arg(hh, 2, 10, QLatin1Char('0'))
                             .arg(mm, 2, 10, QLatin1Char('0'))
                             .arg(ss, 2, 10, QLatin1Char('0'))
                             .arg(ff, 2, 10, QLatin1Char('0'));
    return negative ? QLatin1Char('-') + text : text;
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
