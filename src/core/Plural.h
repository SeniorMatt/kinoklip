// SPDX-License-Identifier: GPL-3.0-or-later
// Plural — the singular or plural English text of a counted message ("1 clip", "3 clips").
// tr("%n clip(s)", nullptr, n) only picks a plural form through a translation, and Kinoklip
// ships none, so counted messages name both forms: plural(n, tr("%n clip"), tr("%n clips")).
#pragma once

#include <QString>

namespace kk {

/// @p one when @p n is 1, else @p many, with every "%n" replaced by @p n.
[[nodiscard]] inline QString plural(qint64 n, const QString& one, const QString& many)
{
    QString text = n == 1 ? one : many;
    return text.replace(QLatin1String("%n"), QString::number(n));
}

} // namespace kk
