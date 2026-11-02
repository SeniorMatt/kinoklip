// SPDX-License-Identifier: GPL-3.0-or-later
// DragAndDrop — shared helpers for every drag source and drop target in the UI: one logging
// category (kk.ui.dnd) and the rules that keep drags working on Wayland.
//
// Wayland rules (also in src/ui/common/readme.md, "Drag and drop"):
//  - a drag can only start while the button that began it is still pressed, from the press /
//    move handlers (wl_data_device.start_drag needs the serial of that press's implicit grab):
//    never from a timer or after the release;
//  - sources offer only CopyAction: the compositor negotiates the action with the target, and
//    "move" made item views delete their rows when a target accepted it;
//  - targets answer every DragEnter *and* DragMove explicitly (acceptProposedAction() or
//    ignore()); Qt turns each answer into wl_data_offer.accept + set_actions.
#pragma once

#include <QLoggingCategory>
#include <Qt>

class QDrag;
class QDropEvent;
class QMimeData;
class QObject;

namespace kk {

Q_DECLARE_LOGGING_CATEGORY(lcDnd)

/// Runs @p drag as a copy (the only action our payloads support) and logs its start and
/// result under kk.ui.dnd. @p source names the drag source in the log. Returns the action the
/// target performed (IgnoreAction if the drop was refused or cancelled).
Qt::DropAction execCopyDrag(QDrag* drag, const char* source);

/// Logs a drag event reaching @p target and whether it was accepted (kk.ui.dnd, debug).
void logDropEvent(const char* target, const QDropEvent* event);

/// Short description of the formats in @p data for logs ("application/x-kinoklip-...").
[[nodiscard]] QString describeMime(const QMimeData* data);

} // namespace kk
