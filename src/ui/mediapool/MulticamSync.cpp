// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamSync — sync offsets of media pool clips + the progress / problem dialogs.
#include "ui/mediapool/MulticamSync.h"

#include "core/Plural.h"
#include "media/AudioSync.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <future>

namespace kk {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("kk::MulticamSync", text, disambiguation, n);
}

/// Progress dialog refresh while waiting for the worker (ms).
constexpr int kPollMs = 30;

} // namespace

int SyncReport::syncedCount() const
{
    return int(std::count_if(
        offsetsUs.cbegin(), offsetsUs.cend(), [](const auto& o) { return o.has_value(); }));
}

QString syncMethodName(multicam::SyncMethod method)
{
    switch (method) {
    case multicam::SyncMethod::Waveform:
        return tr("Waveform");
    case multicam::SyncMethod::Timecode:
        return tr("Timecode");
    case multicam::SyncMethod::InPoints:
        return tr("In Points");
    case multicam::SyncMethod::Start:
        break;
    }
    return tr("Clip Start");
}

SyncReport computeSync(const QVector<SyncInput>& inputs, multicam::SyncMethod method,
    const std::atomic<bool>* cancel, const std::function<void(int, int)>& progress)
{
    SyncReport report;
    const qsizetype n = inputs.size();
    report.offsetsUs.resize(n);
    report.confidence.fill(-1.0, n);
    switch (method) {
    case multicam::SyncMethod::Start:
        report.offsetsUs.fill(int64_t(0));
        return report;
    case multicam::SyncMethod::InPoints: {
        QVector<int64_t> in;
        for (const SyncInput& i : inputs)
            in.append(i.inPointUs);
        const QVector<int64_t> offsets = multicam::offsetsFromInPoints(in);
        for (qsizetype k = 0; k < n; ++k)
            report.offsetsUs[k] = offsets[k];
        return report;
    }
    case multicam::SyncMethod::Timecode: {
        QVector<media::MediaInfo> infos;
        for (const SyncInput& i : inputs)
            infos.append(i.info);
        report.offsetsUs = multicam::offsetsFromTimecode(infos);
        for (qsizetype k = 0; k < n; ++k) {
            if (!report.offsetsUs[k])
                report.problems.append(tr("%1: no start timecode in the file").arg(inputs[k].name));
        }
        return report;
    }
    case multicam::SyncMethod::Waveform:
        break;
    }

    // Waveform. Reference = the longest clip with audio (most likely to overlap every other).
    int reference = -1;
    for (qsizetype k = 0; k < n; ++k) {
        if (inputs[k].info.hasAudio
            && (reference < 0 || inputs[k].info.durationUs > inputs[reference].info.durationUs))
            reference = int(k);
    }
    report.reference = reference;
    if (reference < 0) {
        for (const SyncInput& i : inputs)
            report.problems.append(tr("%1: no audio").arg(i.name));
        return report;
    }
    // Features of every clip with audio, decoded in parallel (each decode is one thread).
    std::vector<std::optional<media::SyncFeatures>> features(static_cast<size_t>(n));
    std::vector<QString> errors(static_cast<size_t>(n));
    std::atomic<int> done {0};
    QVector<QFuture<void>> jobs;
    for (qsizetype k = 0; k < n; ++k) {
        if (!inputs[k].info.hasAudio)
            continue;
        jobs.append(QtConcurrent::run([&, k] {
            features[size_t(k)]
                = media::syncFeaturesFromFile(inputs[k].path, cancel, &errors[size_t(k)]);
            if (progress)
                progress(++done, int(n));
        }));
    }
    for (QFuture<void>& job : jobs)
        job.waitForFinished();
    if (cancel && cancel->load()) {
        report.cancelled = true;
        return report;
    }
    if (!features[size_t(reference)]) {
        report.problems.append(tr("%1: the audio could not be read (%2)")
                                   .arg(inputs[reference].name, errors[size_t(reference)]));
        return report;
    }
    report.offsetsUs[reference] = 0;
    report.confidence[reference] = 1.0;
    for (qsizetype k = 0; k < n; ++k) {
        if (k == reference)
            continue;
        if (!inputs[k].info.hasAudio) {
            report.problems.append(tr("%1: no audio").arg(inputs[k].name));
            continue;
        }
        if (!features[size_t(k)]) {
            report.problems.append(
                tr("%1: the audio could not be read (%2)").arg(inputs[k].name, errors[size_t(k)]));
            continue;
        }
        const media::SyncMatch m
            = media::findSyncOffset(*features[size_t(reference)], *features[size_t(k)]);
        report.confidence[k] = m.confidence;
        if (!m.found) {
            report.problems.append(tr("%1: no sound in common with %2 was found (confidence %3 %)")
                                       .arg(inputs[k].name, inputs[reference].name)
                                       .arg(int(std::lround(m.confidence * 100))));
            continue;
        }
        report.offsetsUs[k] = int64_t(std::llround(m.offsetSeconds * 1e6));
    }
    return report;
}

std::optional<QVector<int64_t>> runSyncWithFeedback(QWidget* parent,
    const QVector<SyncInput>& inputs, multicam::SyncMethod method, const QString& title)
{
    SyncReport report;
    if (method == multicam::SyncMethod::Waveform) {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto progressValue = std::make_shared<std::atomic<int>>(0);
        QProgressDialog dialog(plural(inputs.size(), tr("Analysing the audio of %n clip…"),
                                   tr("Analysing the audio of %n clips…")),
            tr("Cancel"), 0, int(inputs.size()), parent);
        dialog.setWindowTitle(title);
        dialog.setWindowModality(Qt::WindowModal);
        dialog.setMinimumDuration(300);
        dialog.setValue(0);
        std::future<SyncReport> future
            = std::async(std::launch::async, [inputs, method, cancel, progressValue] {
                  return computeSync(inputs, method, cancel.get(),
                      [progressValue](int done, int) { progressValue->store(done); });
              });
        while (future.wait_for(std::chrono::milliseconds(kPollMs)) != std::future_status::ready) {
            dialog.setValue(progressValue->load());
            QApplication::processEvents(QEventLoop::AllEvents, kPollMs);
            if (dialog.wasCanceled())
                cancel->store(true);
        }
        report = future.get();
        dialog.reset();
        if (report.cancelled || dialog.wasCanceled())
            return std::nullopt;
    } else {
        report = computeSync(inputs, method);
    }
    if (report.syncedCount() == 0 && !inputs.isEmpty()) {
        QMessageBox::warning(parent, title,
            tr("The clips could not be synced by %1.").arg(syncMethodName(method).toLower())
                + QStringLiteral("\n\n") + report.problems.join(QLatin1Char('\n')));
        return std::nullopt;
    }
    if (!report.problems.isEmpty()) {
        const auto answer = QMessageBox::question(parent, title,
            tr("%1 of %2 clips were synced. These were not and will start with the first "
               "clip:")
                    .arg(report.syncedCount())
                    .arg(inputs.size())
                + QStringLiteral("\n\n") + report.problems.join(QLatin1Char('\n'))
                + QStringLiteral("\n\n") + tr("Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return std::nullopt;
    }
    // Unsynced clips start with the earliest synced one.
    int64_t earliest = 0;
    bool any = false;
    for (const auto& o : report.offsetsUs) {
        if (o && (!any || *o < earliest)) {
            earliest = *o;
            any = true;
        }
    }
    QVector<int64_t> offsets;
    for (const auto& o : report.offsetsUs)
        offsets.append(o.value_or(earliest));
    return offsets;
}

} // namespace kk
