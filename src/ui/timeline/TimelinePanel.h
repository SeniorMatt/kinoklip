// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel — the Edit page's timeline area: tool bar + TimelineWidget + controller.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/timeline/TimelinePainter.h"

class QMenu;
class QSlider;
class QSplitter;
class QToolBar;

namespace kk {

struct AppContext;
class StackedTimelinePane;
class TimelineController;
class TimelineTabs;
class TimelineWidget;

/// The Edit page's timeline: a tool bar in Resolve's order ([Select][Trim][Blade] |
/// [Insert][Overwrite][Place on Top][Replace] | [Snap][Link] | [Split][Marker] ... right:
/// [Fit][-] zoom slider [+]) bound to the registry actions, and the
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
    /// The row of open timelines (tabs, or the drop-down when tabs are off).
    [[nodiscard]] TimelineTabs* tabs() const { return m_tabs; }
    /// Re-reads the preferences this panel follows (Preferences > Editing > Timelines).
    void applyPreferences();
    /// Timeline View Options (spec 12.11): video clip pictures and audio waveforms of the
    /// view, stored in the settings (prefs::kTimelineThumbnails / kTimelineWaveforms).
    void setClipThumbnails(ClipThumbnails mode);
    void setShowWaveforms(bool on);

    // ---- Stacked timelines (View > Stacked Timelines, spec 31.8 - 31.10) ----
    /// Shows a second timeline above the editor (or hides it). The second one is read-only
    /// until clicked: then it becomes the active timeline (it drives the viewer) and the
    /// editor moves into its place, the other timeline taking the other pane.
    void setStackedTimelines(bool on);
    [[nodiscard]] bool stackedTimelines() const { return m_stacked; }
    /// The inactive pane (null until stacked timelines were first shown).
    [[nodiscard]] StackedTimelinePane* stackedPane() const { return m_pane; }
    /// Makes the inactive pane's timeline the active one (what a click on it does). Returns
    /// whether the timelines swapped.
    bool activateStackedPane();
    /// 0 = the editor is the top pane, 1 = the bottom pane (stacked mode).
    [[nodiscard]] int activePaneIndex() const { return m_activeSlot; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Ctrl+D: asks for a new duration of the selected clips (timecode).
    void askClipDuration();
    /// R: the Change Clip Speed dialog for the selected clips (else the clips under the
    /// playhead).
    void askClipSpeed();
    void syncZoomSlider();

    AppContext& m_ctx;
    QToolBar* m_toolBar = nullptr;
    /// The Timeline View Options button and its menu (filled on every show).
    QWidget* makeViewOptionsButton();
    void fillViewOptionsMenu(QMenu& menu);
    ClipThumbnails m_viewThumbnails = ClipThumbnails::Filmstrip;
    bool m_viewWaveforms = true;
    QSlider* m_zoomSlider = nullptr;
    bool m_syncingSlider = false;
    TimelineController* m_controller = nullptr;
    TimelineWidget* m_view = nullptr;
    TimelineTabs* m_tabs = nullptr;
    // Stacked timelines
    /// Places the editor group and the pane into the two slots (m_activeSlot = editor).
    void placeStackedPanes();
    /// Starts the copy drag of clips of the active timeline dragged out of the editor.
    void dragClipsOut(const QVector<quint64>& clips, int64_t grabFrames);
    QSplitter* m_stack = nullptr;
    QWidget* m_slots[2] = {nullptr, nullptr};
    QWidget* m_editorGroup = nullptr; ///< tabs + timeline widget
    StackedTimelinePane* m_pane = nullptr;
    bool m_stacked = false;
    bool m_swapping = false;
    int m_activeSlot = 1;
    quint64 m_previousActive = 0;
};

} // namespace kk
