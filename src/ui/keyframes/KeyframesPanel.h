// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframesPanel — the large keyframe editor (alpha.2 feedback #10): a parameter tree, a dope
// sheet and an overlaid curve editor for the selected clip(s), on the timeline's time.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/keyframes/KeyframeItemModel.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QHash>
#include <QPointer>
#include <QSet>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QMenu;
class QLabel;
class QScrollBar;
class QSettings;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace kk {

struct AppContext;
class CompSession;
class CurveView;
class DopeSheetView;
class KeyframeParamTree;
class KeyframeRuler;

/// The Keyframes panel (spec 15.14 - 15.18, 15.22 - 15.23).
///
/// Layout: tool row (Keyframes / Curves switch, interpolation presets, Normalised, Fit All,
/// Fit Selection, view range, Show All); left the KeyframeParamTree, right a ruler above the
/// dope sheet or the curve editor (one at a time, spec 15.22) and a time scroll bar.
///
/// What it shows: the selected items (a clip, or a linked video + audio pair as one item,
/// spec 24.1; several items each get an item row). Time is the timeline's: the playhead is
/// shared (scrubbing the ruler, double-clicking a key or the tree's arrows move it; the panel
/// follows it), with the panel's own zoom and scroll:
/// - Fit Clip (default): the shown clips fill the width (refits on a new selection);
/// - Follow Timeline: the same frame range as the timeline view;
/// - Free: set by zooming (Ctrl / Alt + wheel), scrolling (Shift + wheel, scroll bar), middle
///   drag in the curves, Fit Selection; page-scrolls when the playhead leaves the view.
/// Editing uses the shared keyframe views and commands, so every change is one undo step and
/// the inspector, the timeline lanes and the viewers update at once.
///
/// Docking is the page's business (EditPage): the panel only asks for it with
/// floatToggleRequested() (the labelled Float / Dock button in the header, or a double-click
/// on the header) or tearOffRequested() (the header dragged out of the docked panel), and shows
/// the state set by setFloating(). While floating, dragging the header moves the window
/// (QWindow::startSystemMove(): the compositor moves it, which is the only way on Wayland).
/// Thread-safety: GUI thread only.
class KeyframesPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// How the panel's time range is chosen (see the class comment).
    enum class ViewMode {
        FitClip,
        FollowTimeline,
        Free
    };

    /// Which view on the keys is shown; selection, time zoom and scroll and the shown curves
    /// are shared by both.
    enum class View {
        Keyframes, ///< dope sheet
        Curves ///< spline editor
    };

    explicit KeyframesPanel(AppContext& ctx, QWidget* parent = nullptr);
    ~KeyframesPanel() override;

    /// Compositing page mode: shows the animated parameters of the nodes of the composition open
    /// in @p session (the selected nodes, or all with the "Sel" button off) instead of a clip
    /// selection's, as the Spline (@p view Curves) or Keyframes (@p view Keyframes) panel of that
    /// page. The view is fixed, the float / dock button and the view switch are hidden and the
    /// Spline tools (Step In, Reverse, Time Stretch, Loop, Sel / All / None, Time and Value boxes)
    /// are shown. Edits are the Edit page's (paths "node/<id>/<key>", one undo step each).
    void attachComposition(CompSession* session, View view);
    [[nodiscard]] bool isCompositionPanel() const { return m_session != nullptr; }

    /// The timeline view shows frames [@p firstFrame, @p lastFrame] (Follow Timeline mode).
    void setTimelineView(double firstFrame, double lastFrame);
    /// Shows the float / dock state on the title-bar button.
    void setFloating(bool floating);
    [[nodiscard]] bool isFloating() const { return m_floating; }

    void setViewMode(ViewMode mode);
    [[nodiscard]] ViewMode viewMode() const { return m_viewMode; }
    /// Shows that view. A running drag in the current view keeps it until it ends.
    void setView(View view);
    [[nodiscard]] View view() const { return m_view; }
    /// Normalised value axis (true, default) or one shared axis; keeps the tool button in step.
    void setNormalised(bool normalised);
    /// Lists every animatable parameter (true) or only animated ones (default).
    void setShowAllParameters(bool all);

    /// Saves the splitter, the view and options under keys starting with @p prefix.
    void saveState(QSettings& settings, const QString& prefix) const;
    /// Restores what saveState() wrote (missing keys keep the defaults).
    void restoreState(const QSettings& settings, const QString& prefix);

    // ---- Access for tests and the screenshot tool ----
    [[nodiscard]] const KeyframeItems& model() const { return m_model; }
    [[nodiscard]] KeyframeParamTree* tree() const { return m_tree; }
    [[nodiscard]] DopeSheetView* dopeSheet() const { return m_dope; }
    [[nodiscard]] CurveView* curveView() const { return m_curves; }
    [[nodiscard]] KeyframeRuler* ruler() const { return m_ruler; }
    [[nodiscard]] const KeyframeTimeMap& timeMap() const { return m_map; }
    [[nodiscard]] QToolButton* floatButton() const { return m_floatButton; }
    [[nodiscard]] QToolButton* keyframesViewButton() const { return m_keyframesViewButton; }
    [[nodiscard]] QToolButton* curvesViewButton() const { return m_curvesViewButton; }
    [[nodiscard]] QButtonGroup* viewGroup() const { return m_viewGroup; }
    [[nodiscard]] QToolButton* normalisedButton() const { return m_normalisedButton; }
    [[nodiscard]] QToolButton* fitSelectionButton() const { return m_fitSelectionButton; }
    /// The Loop button (default action: Set Loop; menu: every extrapolation mode).
    [[nodiscard]] QToolButton* loopButton() const { return m_loopButton; }
    [[nodiscard]] QToolButton* reverseButton() const { return m_reverseButton; }
    [[nodiscard]] QToolButton* stretchButton() const { return m_stretchButton; }
    [[nodiscard]] QToolButton* selectedNodesButton() const { return m_selOnlyButton; }
    [[nodiscard]] QDoubleSpinBox* timeBox() const { return m_timeBox; }
    [[nodiscard]] QDoubleSpinBox* valueBox() const { return m_valueBox; }
    /// The loop / reverse / stretch entries of the panel's context menu and Loop button.
    void fillSplineMenu(QMenu& menu);
    /// Time Stretch with @p factor around the playhead (the button asks for it).
    bool stretchSelectedKeys(double factor);
    /// Fit All: every shown curve over the whole clip.
    void fitAll();

signals:
    /// The user clicked the float / dock button or double-clicked the header.
    void floatToggleRequested();
    /// The user dragged the docked panel's header out of the panel (tear-off): the page should
    /// float the panel and let the window follow the pointer (startSystemMove()) while the
    /// button is still down.
    void tearOffRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    bool headerEvent(QEvent* event);
    QWidget* buildToolRow();
    void rebuild();
    void applyView();
    void applyRegionColors();
    void updateScrollBar();
    void syncDopeWindow();
    void zoomTime(double factor, double anchorX);
    void scrollTimePx(double dx);
    void fitFrames(double first, double last);
    void setFreeMode();
    void onPlayheadChanged();
    void updateKeyBoxes();
    void applyKeyBox(bool time);
    [[nodiscard]] QVector<KeyframeRef> shownCurveRefs() const;
    void setupSplineTools();
    [[nodiscard]] double dataWidth() const;

    AppContext& m_ctx;
    LinearTimeMap m_map;
    KeyframeItems m_model;
    QVector<quint64> m_shownClips;
    QSet<QString> m_collapsed;
    bool m_showAll = false;
    bool m_floating = false;
    ViewMode m_viewMode = ViewMode::FitClip;
    View m_view = View::Keyframes;
    double m_timelineFirst = 0.0;
    double m_timelineLast = 0.0;
    bool m_syncingScroll = false;
    QPoint m_headerPressGlobal; ///< pointer at the header press (global), for drag gestures
    bool m_headerArmed = false; ///< left button pressed on the header, no gesture yet

    QLabel* m_itemLabel = nullptr;
    QToolButton* m_floatButton = nullptr;
    QButtonGroup* m_viewGroup = nullptr;
    QToolButton* m_keyframesViewButton = nullptr;
    QToolButton* m_curvesViewButton = nullptr;
    QHash<QToolButton*, QAction*> m_toolActions; ///< tool row action of each button
    QToolButton* m_normalisedButton = nullptr;
    QToolButton* m_fitSelectionButton = nullptr;
    QToolButton* m_showAllButton = nullptr;
    QToolButton* m_loopButton = nullptr;
    QToolButton* m_reverseButton = nullptr;
    QToolButton* m_stretchButton = nullptr;
    QToolButton* m_stepInButton = nullptr;
    QToolButton* m_selOnlyButton = nullptr;
    QToolButton* m_allCurvesButton = nullptr;
    QToolButton* m_noCurvesButton = nullptr;
    QDoubleSpinBox* m_timeBox = nullptr;
    QDoubleSpinBox* m_valueBox = nullptr;
    QPointer<CompSession> m_session; ///< Compositing page mode (null on the Edit page)
    bool m_selOnly = true; ///< composition mode: only the selected nodes
    bool m_updatingBoxes = false;
    QComboBox* m_viewCombo = nullptr;
    QStackedWidget* m_stack = nullptr;
    QSplitter* m_split = nullptr; ///< tree | right column
    QStackedWidget* m_viewStack = nullptr; ///< dope sheet or curves
    QLabel* m_treeHeader = nullptr;
    QWidget* m_right = nullptr;
    KeyframeParamTree* m_tree = nullptr;
    KeyframeRuler* m_ruler = nullptr;
    DopeSheetView* m_dope = nullptr;
    CurveView* m_curves = nullptr;
    QScrollBar* m_hscroll = nullptr;
};

} // namespace kk
