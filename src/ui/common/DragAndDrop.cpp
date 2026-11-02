// SPDX-License-Identifier: GPL-3.0-or-later
// DragAndDrop implementation: copy drags with logging.
#include "ui/common/DragAndDrop.h"

#include <QDrag>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>

namespace kk {

Q_LOGGING_CATEGORY(lcDnd, "kk.ui.dnd")

QString describeMime(const QMimeData* data)
{
    return data ? data->formats().join(QLatin1Char(' ')) : QStringLiteral("(none)");
}

Qt::DropAction execCopyDrag(QDrag* drag, const char* source)
{
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        // On Wayland the compositor refuses a drag without the press's implicit grab.
        qCWarning(lcDnd) << source << "starts a drag without the left button held";
    }
    qCDebug(lcDnd) << source << "drag start:" << describeMime(drag->mimeData()) << "platform"
                   << QGuiApplication::platformName();
    const Qt::DropAction result = drag->exec(Qt::CopyAction, Qt::CopyAction);
    qCDebug(lcDnd) << source << "drag finished, action" << int(result);
    return result;
}

void logDropEvent(const char* target, const QDropEvent* event)
{
    if (!lcDnd().isDebugEnabled())
        return;
    const char* type = event->type() == QEvent::DragEnter ? "enter"
        : event->type() == QEvent::DragMove               ? "move"
                                                          : "drop";
    qCDebug(lcDnd) << target << type << event->position() << "accepted" << event->isAccepted()
                   << "action" << int(event->dropAction()) << describeMime(event->mimeData());
}

} // namespace kk
