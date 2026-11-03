// SPDX-License-Identifier: GPL-3.0-or-later
// FileManager — "Show in File Manager": reveal and select a file in the desktop's file manager.
#pragma once

#include <QString>

namespace kk::platform {

/// Opens the desktop's file manager on the folder of @p path with @p path selected, through the
/// freedesktop `org.freedesktop.FileManager1.ShowItems` D-Bus call (Dolphin, Nautilus, Nemo,
/// Thunar, ...). Without that service the folder is opened with openUrl() (nothing selected).
/// A folder @p path is shown selected in its parent. Does not wait for the file manager.
void showInFileManager(const QString& path);

} // namespace kk::platform
