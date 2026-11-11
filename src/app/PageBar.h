// SPDX-License-Identifier: GPL-3.0-or-later
// PageBar — the strip of page buttons (Media · Edit · Color · Deliver) at the bottom.
#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;

namespace kk {

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

    /// Puts @p widget at the left end of the bar (the Vim mode line, spec section 83).
    void setLeadingWidget(QWidget* widget);
    /// A widget laid over the whole bar when shown (the Vim `:` line replaces the mode line).
    /// Not owned by the layout; follows the bar's size.
    void setOverlayWidget(QWidget* widget);
    [[nodiscard]] QWidget* overlayWidget() const { return m_overlay; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QHBoxLayout* m_buttons = nullptr;
    QHBoxLayout* m_root = nullptr;
    QWidget* m_overlay = nullptr;
};

} // namespace kk
