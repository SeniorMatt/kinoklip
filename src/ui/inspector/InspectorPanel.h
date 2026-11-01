// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorPanel — PLACEHOLDER clip inspector (Video / Audio / Effects / Title / Transition tabs).
// The panels agent replaces the implementation with generic editors built from EffectRegistry
// descriptors; keep the public API.
#pragma once

#include "ui/common/PanelFrame.h"

class QTabWidget;

namespace cw {

struct AppContext;

/// Shows and edits the parameters of the current clip (SelectionModel::currentClip()).
/// Placeholder: shows the tabs with an explanatory text and follows the selection.
class InspectorPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the inspector; it follows ctx.selection automatically.
    InspectorPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Clip currently shown (0 = none).
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Shows the clip with id @p clipId (0 = empty state).
    void showClip(quint64 clipId);

private:
    AppContext& m_ctx;
    QTabWidget* m_tabs = nullptr;
    quint64 m_clipId = 0;
};

} // namespace cw
