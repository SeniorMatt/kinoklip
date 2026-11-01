// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel — PLACEHOLDER timeline: tool bar + read-only track/clip drawing.
// The timeline agent replaces the implementation (TimelineWidget, ruler, headers, tools).
#pragma once

#include "ui/common/PanelFrame.h"

#include <cstdint>

class QToolBar;

namespace kk {

struct AppContext;
class TimelinePreview;

/// The Edit page's timeline area: a tool bar ([Select][Trim][Blade] | [Snap][Link] | zoom)
/// bound to the registry actions, and the track view below it.
///
/// Placeholder behaviour: draws tracks and clips from the Timeline, moves the playhead on
/// clicks in the ruler, no editing.
class TimelinePanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel for the project in @p ctx.
    TimelinePanel(AppContext& ctx, QWidget* parent = nullptr);

    /// The tool bar, so other modules can append buttons.
    [[nodiscard]] QToolBar* toolBar() const { return m_toolBar; }

private:
    AppContext& m_ctx;
    QToolBar* m_toolBar = nullptr;
    TimelinePreview* m_view = nullptr;
};

} // namespace kk
