// SPDX-License-Identifier: GPL-3.0-or-later
// AudioBounce, the interactive part: Timeline > Bounce Mix to Track / Bounce Selected Tracks.
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/audio/AudioBounce.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaStore.h"

#include <QApplication>
#include <QMessageBox>
#include <QProgressDialog>

namespace kk::audiobounce {

int runInteractive(AppContext& ctx, QWidget* parent, bool mix)
{
    const QString title
        = mix ? QObject::tr("Bounce Mix to Track") : QObject::tr("Bounce Selected Tracks");
    Options options;
    options.mix = mix;
    if (!mix) {
        options.trackIds = selectedAudioTracks(ctx.project->timeline()->data(), *ctx.selection);
        if (options.trackIds.isEmpty()) {
            QMessageBox::information(parent, title,
                QObject::tr("Select clips on the audio tracks to bounce, or switch on the track "
                            "selector of those tracks."));
            return 0;
        }
    }
    const bool stored = ctx.mediaStore && ctx.mediaStore->storesMedia();
    options.folder = bounceFolder(*ctx.project, stored);
    if (stored) {
        // A single-file project keeps its media inside the file: copy each bounce in first.
        options.store = [&ctx, parent](const QString& wav) {
            QStringList paths {wav};
            return ctx.mediaStore->storeMedia(paths, parent) ? paths.value(0) : QString();
        };
    }
    QProgressDialog dialog(
        QObject::tr("Rendering the audio…"), QObject::tr("Cancel"), 0, 1000, parent);
    dialog.setWindowTitle(title);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(300);
    std::atomic<bool> cancel(false);
    const Result result = bounce(*ctx.project, options, &cancel, [&](double fraction) {
        dialog.setValue(int(fraction * 1000.0));
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents | QEventLoop::ExcludeSocketNotifiers);
        if (dialog.wasCanceled())
            cancel = true;
    });
    dialog.close();
    if (result.cancelled)
        return 0;
    if (!result.ok) {
        QMessageBox::warning(parent, title, result.error);
        return 0;
    }
    return int(result.newTrackIds.size());
}

} // namespace kk::audiobounce
