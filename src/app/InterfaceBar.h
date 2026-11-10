// SPDX-License-Identifier: GPL-3.0-or-later
// InterfaceBar — the top strip with panel toggles (left/right) and the project name (centre).
#pragma once

#include <QHash>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;

namespace kk {

/// "[Media Pool][Effects][Edit Index]   Project name   [Mixer][Metadata][Inspector]".
/// Buttons show checkable panel-toggle actions. Object name "InterfaceBar" for styling.
class InterfaceBar : public QWidget
{
    Q_OBJECT
public:
    /// Creates an empty bar.
    explicit InterfaceBar(QWidget* parent = nullptr);

    /// Appends a toggle button for @p action to the left group.
    void addLeftAction(QAction* action);
    /// Appends a toggle button for @p action to the right group.
    void addRightAction(QAction* action);
    /// Sets the centred title (project name).
    void setTitle(const QString& title);
    /// Shows or hides the button of @p action (like Resolve, each page only shows the
    /// toggles of the panels it has; the action itself stays in the View menu).
    void setActionShown(QAction* action, bool shown);

    /// Shows "Assistant connected" in the bar while @p clients > 0: a local tool (an AI assistant
    /// through MCP) is connected to this window and may change the project. Hidden at 0.
    void setRemoteControlClients(int clients);

private:
    void addButton(QHBoxLayout* layout, QAction* action);

    QHBoxLayout* m_left = nullptr;
    QHBoxLayout* m_right = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_remote = nullptr;
    QHash<QAction*, QWidget*> m_buttons;
};

} // namespace kk
