// SPDX-License-Identifier: GPL-3.0-or-later
// PanelInfo — the one-line description of a panel or page area, read by the menu and panel
// reference (src/app/MenuReference.h, docs/menu-reference.md).
#pragma once

#include <QString>

class QWidget;

namespace kk {

/// Names @p widget (a panel, dock or page area) @p name and says in one sentence what it is
/// for. Call it where the panel is built, with tr() text, so the reference is written from the
/// panels themselves and cannot go stale. Stored as dynamic properties; nothing else changes.
void setPanelInfo(QWidget* widget, const QString& name, const QString& description);

/// The name given with setPanelInfo(), or empty.
[[nodiscard]] QString panelInfoName(const QWidget* widget);
/// The description given with setPanelInfo(), or empty.
[[nodiscard]] QString panelInfoDescription(const QWidget* widget);

} // namespace kk
