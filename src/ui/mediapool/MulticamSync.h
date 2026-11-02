// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamSync — computes the sync offsets of media pool clips (waveform, timecode, in
// points) for multicam clips and "Sync and Place on Timeline", with a progress dialog.
#pragma once

#include "core/Multicam.h"
#include "media/MediaInfo.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>

class QWidget;

namespace kk {

/// One clip to sync.
struct SyncInput
{
    QString mediaId;
    QString name; ///< for messages
    QString path; ///< media file (waveform sync decodes its audio)
    media::MediaInfo info;
    int64_t inPointUs = 0; ///< source in point (SyncMethod::InPoints); 0 = clip start
};

/// Result of computeSync(): one offset per input (AngleSource::offsetUs semantics), or
/// nullopt where that clip could not be synced.
struct SyncReport
{
    QVector<std::optional<int64_t>> offsetsUs;
    /// Waveform: confidence 0..1 per clip (1 for the reference clip), -1 = not applicable.
    QVector<double> confidence;
    int reference = -1; ///< Waveform: the clip the others were matched against
    QStringList problems; ///< human-readable, one per clip that could not be synced
    bool cancelled = false;

    /// Number of inputs that got an offset.
    [[nodiscard]] int syncedCount() const;
};

/// Computes the offsets (blocking; call from a worker thread for Waveform). Waveform: the
/// longest clip with audio is the reference; every other clip is matched against it with
/// media::findSyncOffset() (features of all clips decoded in parallel). Timecode: the start
/// timecode. InPoints: the in points line up. Start: all zero.
/// @p progress (optional) is called with (done, total) from worker threads.
[[nodiscard]] SyncReport computeSync(const QVector<SyncInput>& inputs, multicam::SyncMethod method,
    const std::atomic<bool>* cancel = nullptr,
    const std::function<void(int done, int total)>& progress = {});

/// computeSync() on a worker thread behind a modal progress dialog with a Cancel button
/// (only Waveform takes long; the others return at once). Asks the user whether to go on when
/// some clips could not be synced (they are placed at the start of the reference); nullopt =
/// cancelled. The result has an offset for every input.
[[nodiscard]] std::optional<QVector<int64_t>> runSyncWithFeedback(QWidget* parent,
    const QVector<SyncInput>& inputs, multicam::SyncMethod method, const QString& title);

/// Display name of a sync method ("Waveform", "Timecode", ...).
[[nodiscard]] QString syncMethodName(multicam::SyncMethod method);

} // namespace kk
