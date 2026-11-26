// SPDX-License-Identifier: GPL-3.0-or-later
// MediaMetadataFiles implementation.
#include "app/MediaMetadataFiles.h"

#include "core/MediaMetadataCsv.h"
#include "core/Project.h"
#include "export/EdlExport.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QMessageBox>
#include <QWidget>

namespace kk {

namespace {

constexpr char kLastMetadataDirKey[] = "paths/lastMetadataDir";

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::MediaMetadataFiles", text);
}

void exportMetadata(AppContext& ctx, QWidget* window)
{
    const QString title = tr("Export Metadata From Media Pool");
    const QString base = ctx.project->name().isEmpty() ? tr("Media Pool") : ctx.project->name();
    const QString path = platform::getSaveFileName(window, title, tr("CSV files (*.csv)"),
        QLatin1String(kLastMetadataDirKey), base + QStringLiteral(" Metadata.csv"));
    if (path.isEmpty())
        return;
    QString error;
    if (!edl::writeTextFile(path, mediametadata::toCsv(*ctx.project->mediaPool()), &error))
        QMessageBox::warning(window, title, error);
}

void importMetadata(AppContext& ctx, QWidget* window)
{
    const QString title = tr("Import Metadata To Media Pool");
    const QString path = platform::getOpenFileName(
        window, title, tr("CSV files (*.csv)"), QLatin1String(kLastMetadataDirKey));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(
            window, title, tr("Cannot read %1: %2").arg(path, file.errorString()));
        return;
    }
    const QString text = QString::fromUtf8(file.readAll());
    ctx.project->beginEdit(tr("Import Metadata"));
    const mediametadata::ImportReport report
        = mediametadata::applyCsv(*ctx.project->mediaPool(), text);
    if (!report.ok() || report.updated == 0)
        ctx.project->cancelEdit();
    else
        ctx.project->endEdit();
    if (!report.ok()) {
        QMessageBox::warning(window, title, report.error);
        return;
    }
    QString message = QCoreApplication::translate(
        "kk::MediaMetadataFiles", "Updated the metadata of %n clip(s).", nullptr, report.updated);
    if (report.unchanged > 0)
        message += QLatin1Char('\n') + QCoreApplication::translate(
                "kk::MediaMetadataFiles", "%n clip(s) already matched.", nullptr, report.unchanged);
    if (!report.unmatched.isEmpty())
        message += QLatin1Char('\n')
            + tr("No clip found for: %1").arg(report.unmatched.mid(0, 8).join(QStringLiteral(", ")));
    if (!report.ambiguous.isEmpty())
        message += QLatin1Char('\n')
            + tr("Several clips have the name (add a Clip Directory column): %1")
                  .arg(report.ambiguous.mid(0, 8).join(QStringLiteral(", ")));
    QMessageBox::information(window, title, message);
}

} // namespace

void connectMediaMetadataActions(AppContext& ctx, QWidget* window)
{
    ActionRegistry& actions = *ctx.actions;
    QObject::connect(actions.action(QStringLiteral("file.export_metadata")), &QAction::triggered,
        window, [&ctx, window] { exportMetadata(ctx, window); });
    QObject::connect(actions.action(QStringLiteral("file.import_metadata")), &QAction::triggered,
        window, [&ctx, window] { importMetadata(ctx, window); });
}

} // namespace kk
