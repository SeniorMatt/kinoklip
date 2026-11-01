// SPDX-License-Identifier: GPL-3.0-or-later
// InterfaceBar — the top strip with panel toggles (left/right) and the project name (centre).
#pragma once

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

private:
    void addButton(QHBoxLayout* layout, QAction* action);

    QHBoxLayout* m_left = nullptr;
    QHBoxLayout* m_right = nullptr;
    QLabel* m_title = nullptr;
};

} // namespace kk
