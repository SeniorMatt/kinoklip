// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineExport implementation.
#include "app/TimelineExport.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "export/EdlExport.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/SourceTimecode.h"

#include <QAction>
#include <QCoreApplication>
#include <QMessageBox>
#include <QWidget>

#include <functional>

namespace kk {

namespace {

constexpr char kLastExportDirKey[] = "paths/lastExportDir";

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::TimelineExport", text);
}

/// Asks for a file and writes what @p build returns for the active timeline.
void exportText(AppContext& ctx, QWidget* window, const QString& title, const QString& filter,
    const QString& suffix, const std::function<QString(const TimelineData&)>& build)
{
    const TimelineData& data = ctx.project->timeline()->data();
    const QString base = data.name.isEmpty() ? tr("Timeline") : data.name;
    const QString path = platform::getSaveFileName(
        window, title, filter, QLatin1String(kLastExportDirKey), base + suffix);
    if (path.isEmpty())
        return;
    QString error;
    if (!edl::writeTextFile(path, build(data), &error))
        QMessageBox::warning(window, title, error);
}

} // namespace

void connectTimelineExportActions(AppContext& ctx, QWidget* window)
{
    ActionRegistry& actions = *ctx.actions;
    QObject::connect(actions.action(QStringLiteral("file.export_edl")), &QAction::triggered, window,
        [&ctx, window] {
            exportText(ctx, window, tr("Export Timeline as EDL"), tr("EDL files (*.edl)"),
                QStringLiteral(".edl"), [&ctx](const TimelineData& data) {
                    const MediaPool* pool = ctx.project->mediaPool();
                    edl::EdlOptions options;
                    // Source timecodes start at the media's own start timecode.
                    options.sourceStartUs = [pool, fps = data.fps](const ClipData& clip) {
                        return sourceTimecodeBase(clip, pool, fps).startUs;
                    };
                    return edl::timelineToEdl(data, pool, options);
                });
        });
    QObject::connect(actions.action(QStringLiteral("file.export_markers_edl")), &QAction::triggered,
        window, [&ctx, window] {
            exportText(ctx, window, tr("Export Timeline Markers to EDL"), tr("EDL files (*.edl)"),
                QStringLiteral(" Markers.edl"), &edl::markersToEdl);
        });
    QObject::connect(actions.action(QStringLiteral("file.export_markers_csv")), &QAction::triggered,
        window, [&ctx, window] {
            exportText(ctx, window, tr("Export Timeline Markers to CSV"), tr("CSV files (*.csv)"),
                QStringLiteral(" Markers.csv"), &edl::markersToCsv);
        });
}

} // namespace kk
