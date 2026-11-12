// SPDX-License-Identifier: GPL-3.0-or-later
// MemoryReport — the numbers that show where the memory of the process is: its own resident
// memory and shared memory, what the system has available, and what the GPU driver counts for
// the process (DRM fdinfo; on an integrated GPU that is system memory no process meter shows).
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace kk::memreport {

/// One line per fact, e.g. "RSS: 1020 MiB", "RssShmem: 80 MiB", "MemAvailable: 5120 MiB",
/// "drm i915 total-system: 9800 MiB". Linux /proc; empty elsewhere.
[[nodiscard]] QStringList lines();

/// lines() joined into one line, for the log when a render ends.
[[nodiscard]] QString oneLine();

/// Sums the drm-total-*, drm-resident-* and drm-memory-* values of the fdinfo texts (one per
/// open DRM file), counting each drm-client-id once. Lines like "drm xe total-system: 10 MiB".
[[nodiscard]] QStringList drmLines(const QList<QByteArray>& fdinfoTexts);

} // namespace kk::memreport
