// SPDX-License-Identifier: GPL-3.0-or-later
// DroppedFiles — the local files carried by a drag from a file manager or the desktop
// (text/uri-list, or a document-portal file transfer from a sandboxed source).
#pragma once

#include <QStringList>

class QMimeData;

namespace kk::platform {

/// MIME type of the XDG document portal's file transfer (org.freedesktop.portal.FileTransfer):
/// the payload is a key that RetrieveFiles() turns into paths. Sandboxed sources (Flatpak /
/// Snap apps, some portal-forwarded drags) offer it, often next to a text/uri-list.
inline constexpr char kPortalFileTransferMime[] = "application/vnd.portal.filetransfer";

/// True if @p data may carry files (text/uri-list or a portal file transfer). Only looks at
/// the offered formats, so it is cheap enough for every DragMove: on Wayland reading the data
/// of another client's drag goes through a pipe and is only done on drop.
[[nodiscard]] bool hasDroppedFiles(const QMimeData* data);

/// The local file paths in @p data, in order, duplicates removed. Reads text/uri-list first
/// (file:// URLs; document-portal paths such as /run/user/1000/doc/<id>/clip.mp4 are ordinary
/// readable files); without local URLs it asks the document portal for the files of the
/// file-transfer key (a blocking D-Bus call, kPortalTimeoutMs at most). Empty if nothing could
/// be resolved. Call on drop only.
[[nodiscard]] QStringList droppedLocalFiles(const QMimeData* data);

/// The local file paths a drag shows while it is still moving (the timeline's ghost clips):
/// application/x-kde4-urilist or text/uri-list, never the document portal (no blocking D-Bus
/// call, and a text/uri-list next to a portal key is skipped because Qt Wayland would resolve
/// it through the portal). Empty when only a portal key is offered: the drop still works then.
[[nodiscard]] QStringList draggedLocalFiles(const QMimeData* data);

/// Upper bound for the portal's RetrieveFiles call.
inline constexpr int kPortalTimeoutMs = 3000;

} // namespace kk::platform
