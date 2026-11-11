// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPaste — Ctrl+V in the media pool: import files or an image from the clipboard (spec 79).
#include "core/Project.h"
#include "platform/DroppedFiles.h"
#include "ui/common/AppContext.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMainWindow>
#include <QMimeData>
#include <QStandardPaths>
#include <QStatusBar>

namespace kk {

QString MediaPoolPanel::pastedMediaFolder() const
{
    const QString project = m_ctx.project ? m_ctx.project->filePath() : QString();
    if (!project.isEmpty())
        return QFileInfo(project).absoluteDir().filePath(QStringLiteral("Pasted Media"));
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/Pasted Media");
}

bool MediaPoolPanel::hasFocusInside() const
{
    const QWidget* w = QApplication::focusWidget();
    return w && (w == this || isAncestorOf(w));
}

QStringList MediaPoolPanel::pasteFromClipboard()
{
    const QMimeData* data = QApplication::clipboard()->mimeData();
    QStringList paths;
    if (data) {
        // Files first (no duplicate copy of a picture that already is a file).
        if (platform::hasDroppedFiles(data))
            paths = platform::droppedLocalFiles(data);
        if (paths.isEmpty() && data->hasImage()) {
            const QImage image = qvariant_cast<QImage>(data->imageData());
            if (!image.isNull()) {
                const QString folder = pastedMediaFolder();
                const QString stamp
                    = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
                QString file;
                if (QDir().mkpath(folder)) {
                    for (int n = 1; n < 10000; ++n) {
                        const QString name = n == 1
                            ? QStringLiteral("Pasted image %1.png").arg(stamp)
                            : QStringLiteral("Pasted image %1 (%2).png").arg(stamp).arg(n);
                        if (!QFileInfo::exists(QDir(folder).filePath(name))) {
                            file = QDir(folder).filePath(name);
                            break;
                        }
                    }
                }
                if (!file.isEmpty() && image.save(file, "PNG"))
                    paths << file;
            }
        }
    }
    if (paths.isEmpty()) {
        const QString text = tr("Nothing to paste: the clipboard has no files or picture.");
        if (auto* win = qobject_cast<QMainWindow*>(window()))
            win->statusBar()->showMessage(text, 4000);
        return {};
    }
    return importFiles(paths, importTargetKey());
}

} // namespace kk
