// SPDX-License-Identifier: GPL-3.0-or-later
// CutPage — fast single-timeline editing: media pool, one large viewer (source clip / source
// tape / timeline), smart edit buttons and the dual timeline.
#pragma once

#include "ui/common/Page.h"

class QSplitter;

namespace kk {

struct AppContext;
class CutEditor;
class CutOverview;
class CutToolBar;
class CutViewer;
class FastReview;
class MediaPoolPanel;
class TimelineController;
class TimelineWidget;

/// Cut page layout (Resolve's Cut page):
/// ```
/// | Media Pool | Viewer (Source Clip / Source Tape / Timeline)   |
/// | Smart edits | split, ripple delete, dissolve | fast review   |
/// | Upper timeline: the whole timeline                          |
/// | Lower timeline: zoomed around the playhead, editable        |
/// ```
/// It edits the same Project, active timeline, selection and undo stack as the Edit page, with
/// the Edit page's TimelineController (shared tool, snapping, targets, clipboard), so switching
/// pages keeps the playhead and the selection. The lower timeline is a TimelineWidget with the
/// Cut page view options (centred playhead, ripple edge trims, compact audio); the upper one a
/// CutOverview. Both follow the playhead during playback.
///
/// The cut.* actions are enabled while the page is shown; Edit page source edit keys (F9,
/// Shift+F10, F12, Shift+F12) run the Cut versions here (CutEditor::handleSourceEdit()).
class CutPage : public Page
{
    Q_OBJECT
public:
    /// Builds the page; @p controller is the Edit page's timeline controller (not owned).
    CutPage(AppContext& ctx, TimelineController* controller, QWidget* parent = nullptr);

    [[nodiscard]] QString pageId() const override { return QStringLiteral("cut"); }
    [[nodiscard]] bool hasPanel(PanelId id) const override { return id == PanelId::MediaPool; }
    void setPanelVisible(PanelId id, bool visible) override;
    [[nodiscard]] bool isPanelVisible(PanelId id) const override;
    void saveLayout(QSettings& settings) const override;
    void restoreLayout(const QSettings& settings) override;

    [[nodiscard]] MediaPoolPanel* mediaPool() const { return m_mediaPool; }
    [[nodiscard]] CutViewer* viewer() const { return m_viewer; }
    [[nodiscard]] CutToolBar* toolBar() const { return m_toolBar; }
    [[nodiscard]] CutOverview* overview() const { return m_overview; }
    /// The lower, editable timeline.
    [[nodiscard]] TimelineWidget* timeline() const { return m_lower; }
    [[nodiscard]] CutEditor* editor() const { return m_editor; }
    [[nodiscard]] FastReview* fastReview() const { return m_review; }

    /// Seconds the lower timeline shows across its width by default.
    static constexpr int kLowerTimelineSeconds = 20;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void connectMediaPool();
    void connectActions();
    void updateTapeItems();
    void syncOverviewRange();
    /// Gives the lower timeline its default zoom once it has a real width.
    void applyDefaultZoom();
    void setCutActionsEnabled(bool enabled);

    AppContext& m_ctx;
    TimelineController* m_controller = nullptr;
    MediaPoolPanel* m_mediaPool = nullptr;
    CutViewer* m_viewer = nullptr;
    CutToolBar* m_toolBar = nullptr;
    CutOverview* m_overview = nullptr;
    TimelineWidget* m_lower = nullptr;
    CutEditor* m_editor = nullptr;
    FastReview* m_review = nullptr;
    QSplitter* m_main = nullptr; ///< vertical: top row | timelines
    QSplitter* m_topRow = nullptr; ///< media pool | viewer
    QSplitter* m_timelines = nullptr; ///< overview | lower timeline
    bool m_zoomApplied = false;
};

} // namespace kk
