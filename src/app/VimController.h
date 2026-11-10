// SPDX-License-Identifier: GPL-3.0-or-later
// VimController — the window's side of the vim key layer (spec section 62): builds the layer
// (ui/common/VimInput.h) and its timeline host, shows the mode indicator in the timeline's tool
// bar, and owns the `:` command line, which runs a few window commands (w, q, wq) and every
// command of the command registry on the open project (`:marker add --frame 120`).
#pragma once

#include "app/commands/Command.h"

#include <QObject>
#include <QStringList>

class QAction;
class QLabel;
class QLineEdit;
class QWidget;

namespace kk {

struct AppContext;
class TimelinePanel;
class VimInput;
class VimTimelineHost;

class VimController : public QObject
{
    Q_OBJECT
public:
    /// @p window hosts the command line (a child at its bottom edge); @p target is the open
    /// project for registry commands.
    VimController(AppContext& ctx, TimelinePanel* timeline, QWidget* window,
        commands::LiveTarget target, QObject* parent = nullptr);

    ~VimController() override;

    [[nodiscard]] VimInput* input() const { return m_input; }
    [[nodiscard]] QLabel* indicator() const { return m_indicator; }
    [[nodiscard]] QLineEdit* commandLine() const { return m_line; }

    /// Runs one command-line text (without the colon) and returns the message to show.
    /// Public for tests.
    QString runCommand(const QString& text);

    /// Opens the `:` line (also what the layer does for the colon key).
    void showCommandLine();

private:
    void closeCommandLine();
    void updateIndicator();
    bool eventFilter(QObject* watched, QEvent* event) override;

    AppContext& m_ctx;
    QWidget* m_window;
    commands::LiveTarget m_target;
    VimInput* m_input = nullptr;
    VimTimelineHost* m_host = nullptr;
    QLabel* m_indicator = nullptr;
    QAction* m_indicatorAction = nullptr;
    QLineEdit* m_line = nullptr;
    QStringList m_history;
    int m_historyPos = 0;
};

} // namespace kk
