// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineTabs — the row above the timeline listing the open timelines: tabs (default) or,
// with tabs turned off in Preferences > Editing, a drop-down of the project's timelines.
#pragma once

#include <QWidget>

class QComboBox;
class QTabBar;

namespace kk {

struct AppContext;

/// Shows Project::tabs() as a QTabBar (switch by clicking, drag to reorder, close button,
/// context menu) or — tabs off — a QComboBox with every timeline (compound clips only while
/// they are open). Pure view of the Project: every user action goes through Project
/// (setActiveTimeline / moveTab / closeTab) and the row follows its signals.
///
/// Thread-safety: GUI thread only.
class TimelineTabs : public QWidget
{
    Q_OBJECT
public:
    /// Creates the row for the project in @p ctx.
    explicit TimelineTabs(AppContext& ctx, QWidget* parent = nullptr);

    /// Tabs (true) or the drop-down (false).
    void setTabsMode(bool tabs);
    [[nodiscard]] bool tabsMode() const { return m_tabsMode; }
    /// The tab bar (tests, screenshots).
    [[nodiscard]] QTabBar* tabBar() const { return m_bar; }
    /// The drop-down used when tabs are off.
    [[nodiscard]] QComboBox* comboBox() const { return m_combo; }
    /// Timeline uid of tab @p index (0 if out of range).
    [[nodiscard]] quint64 uidAt(int index) const;

signals:
    /// The user asked to rename timeline @p uid (the panel asks for the name).
    void renameRequested(quint64 uid);
    /// The user asked for the settings of timeline @p uid.
    void settingsRequested(quint64 uid);

private:
    /// Brings the bar / drop-down in line with the project (cheap when only names changed,
    /// never recreates tabs while the user drags one).
    void sync();
    void showTabMenu(int index, const QPoint& globalPos);

    AppContext& m_ctx;
    QTabBar* m_bar = nullptr;
    QComboBox* m_combo = nullptr;
    bool m_tabsMode = true;
    bool m_syncing = false;
};

} // namespace kk
