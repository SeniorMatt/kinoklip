// SPDX-License-Identifier: GPL-3.0-or-later
// CutToolBar — the Cut page's tool bar above the dual timeline: the six smart edit buttons,
// split / ripple delete / dissolve, fast review, a message line and the zoom buttons.
#pragma once

#include <QWidget>

class QLabel;
class QTimer;
class QToolBar;
class QToolButton;

namespace kk {

struct AppContext;

/// A row of buttons bound to registry actions (nothing is hard-wired: the groups are action
/// ids). The smart edits show their names next to the icon, like Resolve's labelled Cut page
/// buttons; the other commands are icon buttons with tooltips naming the shortcut. A message
/// line shows why an edit did nothing (showMessage()).
///
/// Thread-safety: GUI thread only.
class CutToolBar : public QWidget
{
    Q_OBJECT
public:
    /// Builds the bar from the actions of @p ctx.
    explicit CutToolBar(AppContext& ctx, QWidget* parent = nullptr);

    /// Shows @p text for a few seconds (empty = clear now).
    void showMessage(const QString& text);
    /// The button showing action @p actionId, or nullptr (tests).
    [[nodiscard]] QToolButton* button(const QString& actionId) const;

signals:
    /// The zoom buttons (the page zooms its lower timeline).
    void zoomInRequested();
    void zoomOutRequested();

private:
    QToolButton* addActionButton(const char* actionId, bool withText);

    AppContext& m_ctx;
    QToolBar* m_bar = nullptr;
    QLabel* m_message = nullptr;
    QTimer* m_messageTimer = nullptr;
};

} // namespace kk
