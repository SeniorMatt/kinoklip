// SPDX-License-Identifier: GPL-3.0-or-later
// PageBar — the strip of page buttons (Media · Edit · Color · Deliver) at the bottom.
#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;

namespace cw {

/// Bottom bar with one button per page. Each button shows a page action (checkable, in an
/// exclusive group owned by MainWindow), so clicking, the View menu and Shift+N shortcuts all
/// stay in sync. Object name "PageBar" for the style sheet.
class PageBar : public QWidget
{
    Q_OBJECT
public:
    /// Creates an empty bar.
    explicit PageBar(QWidget* parent = nullptr);

    /// Appends a button for @p pageAction (icon + text).
    void addPageAction(QAction* pageAction);

private:
    QHBoxLayout* m_buttons = nullptr;
};

} // namespace cw
