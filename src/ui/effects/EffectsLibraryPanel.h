// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLibraryPanel — PLACEHOLDER list of effects, transitions, titles and generators.
// The panels agent adds drag & drop onto the timeline, search and favourites.
#pragma once

#include "ui/common/PanelFrame.h"

class QTreeWidget;

namespace cw {

struct AppContext;

/// Browses the EffectRegistry catalogue grouped by category.
class EffectsLibraryPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel and fills it from EffectRegistry::instance().
    EffectsLibraryPanel(AppContext& ctx, QWidget* parent = nullptr);

signals:
    /// The user double-clicked descriptor @p descriptorId (apply to the selected clips).
    void effectActivated(const QString& descriptorId);

private:
    AppContext& m_ctx;
    QTreeWidget* m_tree = nullptr;
};

} // namespace cw
