// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel — the Edit page's timeline area: tool bar + TimelineWidget + controller.
#pragma once

#include "ui/common/PanelFrame.h"

class QToolBar;

namespace kk {

struct AppContext;
class TimelineController;
class TimelineWidget;

/// The Edit page's timeline: a tool bar ([Select][Trim][Blade] | [Snap][Link] |
/// [Insert][Overwrite][Place on Top] | zoom) bound to the registry actions, and the
/// interactive TimelineWidget below it. Owns the TimelineController, which connects the
/// timeline's actions (see src/ui/timeline/readme.md for the list and the public API other
/// modules use: TimelineController::insertSource(), setSourceProvider(), the widget's
/// thumbnail/waveform providers and setFollowPlayhead()).
class TimelinePanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the panel for the project in @p ctx and connects the timeline actions.
    TimelinePanel(AppContext& ctx, QWidget* parent = nullptr);

    /// The tool bar, so other modules can append buttons.
    [[nodiscard]] QToolBar* toolBar() const { return m_toolBar; }
    /// Commands, tool state and track targeting.
    [[nodiscard]] TimelineController* controller() const { return m_controller; }
    /// The track view.
    [[nodiscard]] TimelineWidget* timelineWidget() const { return m_view; }

private:
    AppContext& m_ctx;
    QToolBar* m_toolBar = nullptr;
    TimelineController* m_controller = nullptr;
    TimelineWidget* m_view = nullptr;
};

} // namespace kk
