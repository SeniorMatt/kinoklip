// SPDX-License-Identifier: GPL-3.0-or-later
// VimController — the window's side of the vim key layer (spec sections 62 and 83): builds the
// layer (ui/common/VimInput.h) and its timeline host, shows the permanent mode line in the page
// bar row and the accent frame of the focused panel, and owns the `:` command line (which
// replaces the mode line while open), running a few window commands (w, q, wq) and every
// command of the command registry on the open project (`:marker add --frame 120`).
#pragma once

#include "app/commands/Command.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

class QLineEdit;
class QWidget;

namespace kk {

struct AppContext;
class PageBar;
class PanelFrame;
class TimelinePanel;
class VimInput;
class VimKeymap;
class VimModeLine;
class VimWhichKey;
class VimTimelineHost;

class VimController : public QObject
{
    Q_OBJECT
public:
    /// @p pageBar hosts the mode line (left end) and the `:` line (laid over the bar);
    /// @p target is the open project for registry commands.
    VimController(AppContext& ctx, TimelinePanel* timeline, QWidget* window, PageBar* pageBar,
        commands::LiveTarget target, QObject* parent = nullptr);

    ~VimController() override;

    [[nodiscard]] VimInput* input() const { return m_input; }
    [[nodiscard]] VimModeLine* modeLine() const { return m_modeLine; }
    [[nodiscard]] VimKeymap* keymap() const;
    [[nodiscard]] VimWhichKey* whichKey() const { return m_whichKey; }
    [[nodiscard]] QLineEdit* commandLine() const { return m_line; }

    /// Runs one command-line text (without the colon) and returns the message to show.
    /// Public for tests.
    QString runCommand(const QString& text);

    /// Opens the `:` line (also what the layer does for the colon key).
    void showCommandLine();

private:
    void closeCommandLine();
    void updateModeLine();
    void updateFocusFrame();
    void updateWhichKey();
    bool eventFilter(QObject* watched, QEvent* event) override;

    AppContext& m_ctx;
    QWidget* m_window;
    commands::LiveTarget m_target;
    VimInput* m_input = nullptr;
    VimTimelineHost* m_host = nullptr;
    PageBar* m_pageBar = nullptr;
    VimModeLine* m_modeLine = nullptr;
    VimWhichKey* m_whichKey = nullptr;
    QPointer<PanelFrame> m_framed; ///< the panel drawn with the focus frame
    QPointer<QWidget> m_returnFocus; ///< where the focus goes when the `:` line closes
    QLineEdit* m_line = nullptr;
    QStringList m_history;
    int m_historyPos = 0;
};

} // namespace kk
