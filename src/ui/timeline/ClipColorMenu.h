// SPDX-License-Identifier: GPL-3.0-or-later
// ClipColorMenu — "Clip Color" and "Flags" submenus of the timeline clip context menu.
#pragma once

#include <QVector>

class QMenu;
class QObject;

namespace kk {

struct AppContext;

/// Adds "Clip Color" (sets ClipData::label of the clips @p clipIds of the active timeline;
/// "Clear Color" falls back to the media pool item's colour) and "Flags" (toggles flags on the
/// media pool items of those clips: flags belong to the source, so every instance shows them)
/// to @p menu. Each choice is one undo step; clips on locked tracks keep their colour.
/// @p context owns the menu actions' connections.
void addClipColorMenus(
    QMenu& menu, AppContext& ctx, const QVector<quint64>& clipIds, QObject* context);

} // namespace kk
