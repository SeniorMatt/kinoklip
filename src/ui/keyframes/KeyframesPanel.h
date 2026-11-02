// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframesPanel — the large keyframe editor (alpha.2 feedback #10): a parameter tree, a dope
// sheet and an overlaid curve editor for the selected clip(s), on the timeline's time.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/keyframes/KeyframeItemModel.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QSet>

class QCheckBox;
class QComboBox;
class QLabel;
class QScrollBar;
class QSettings;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace kk {

struct AppContext;
class CurveView;
class DopeSheetView;
class KeyframeParamTree;
class KeyframeRuler;

/// The Keyframes panel (spec 15.14 - 15.18).
///
/// Layout: tool row (dope sheet / curves switches, interpolation presets, Normalised, Fit All,
/// Fit Selection, view range, Show All); left the KeyframeParamTree, right a ruler above the
/// dope sheet and the curve editor (either or both, split) and a time scroll bar.
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
/// floatToggleRequested() and shows the state set by setFloating().
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

    explicit KeyframesPanel(AppContext& ctx, QWidget* parent = nullptr);
    ~KeyframesPanel() override;

    /// The timeline view shows frames [@p firstFrame, @p lastFrame] (Follow Timeline mode).
    void setTimelineView(double firstFrame, double lastFrame);
    /// Shows the float / dock state on the title-bar button.
    void setFloating(bool floating);
    [[nodiscard]] bool isFloating() const { return m_floating; }

    void setViewMode(ViewMode mode);
    [[nodiscard]] ViewMode viewMode() const { return m_viewMode; }
    /// Shows the dope sheet and / or the curve editor (at least one stays on).
    void setViews(bool dopeSheet, bool curves);
    /// Normalised value axis (true, default) or one shared axis; keeps the tool button in step.
    void setNormalised(bool normalised);
    /// Lists every animatable parameter (true) or only animated ones (default).
    void setShowAllParameters(bool all);

    /// Saves splitters, views and options under keys starting with @p prefix.
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

signals:
    /// The user clicked the float / dock button.
    void floatToggleRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* buildToolRow();
    void rebuild();
    void applyView();
    void updateScrollBar();
    void syncDopeWindow();
    void zoomTime(double factor, double anchorX);
    void scrollTimePx(double dx);
    void fitFrames(double first, double last);
    void setFreeMode();
    void onPlayheadChanged();
    [[nodiscard]] double dataWidth() const;

    AppContext& m_ctx;
    LinearTimeMap m_map;
    KeyframeItems m_model;
    QVector<quint64> m_shownClips;
    QSet<QString> m_collapsed;
    bool m_showAll = false;
    bool m_floating = false;
    ViewMode m_viewMode = ViewMode::FitClip;
    double m_timelineFirst = 0.0;
    double m_timelineLast = 0.0;
    bool m_syncingScroll = false;

    QLabel* m_itemLabel = nullptr;
    QToolButton* m_floatButton = nullptr;
    QToolButton* m_dopeButton = nullptr;
    QToolButton* m_curvesButton = nullptr;
    QToolButton* m_normalisedButton = nullptr;
    QToolButton* m_showAllButton = nullptr;
    QComboBox* m_viewCombo = nullptr;
    QStackedWidget* m_stack = nullptr;
    QSplitter* m_split = nullptr; ///< tree | right column
    QSplitter* m_viewSplit = nullptr; ///< dope sheet over curves
    QWidget* m_right = nullptr;
    KeyframeParamTree* m_tree = nullptr;
    KeyframeRuler* m_ruler = nullptr;
    DopeSheetView* m_dope = nullptr;
    CurveView* m_curves = nullptr;
    QScrollBar* m_hscroll = nullptr;
};

} // namespace kk
