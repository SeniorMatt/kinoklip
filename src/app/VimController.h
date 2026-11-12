// SPDX-License-Identifier: GPL-3.0-or-later
// VimController — the window's side of the vim key layer (spec sections 62 and 83): builds the
// layer (ui/common/VimInput.h) and its timeline host, shows the permanent mode line in the page
// bar row and the accent frame of the focused panel, and owns the `:` command line (which
// replaces the mode line while open, with completion and history), runs the ex commands
// (app/VimEx.h: :w :q :e :bn :ls :page :set :undo ... and every command of the command
// registry on the open project, `:marker add --frame 120`), and opens the pickers
// (ui/common/VimPicker.h) of <leader>f.
#pragma once

#include "app/commands/Command.h"
#include "ui/common/VimCommandLine.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>

class QFrame;
class QLabel;
class QLineEdit;
class QWidget;

namespace kk {

struct AppContext;
namespace vimex {
struct Parsed;
}
class PageBar;
class PanelFrame;
class TimelinePanel;
class VimInput;
class VimListHost;
class VimFormHost;
class VimKeymap;
class VimModeLine;
class VimPicker;
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
    [[nodiscard]] VimCommandLine* commandLine() const { return m_line; }
    [[nodiscard]] VimPicker* picker() const { return m_picker; }
    /// The matches listed under the `:` line while completing (hidden when empty).
    [[nodiscard]] QFrame* wildMenu() const { return m_wild; }
    /// The timeline host (cursor track, registers, search).
    [[nodiscard]] VimTimelineHost* host() const { return m_host; }

    /// Runs one command-line text (without the colon) and returns the message to show;
    /// @p ok (if given) tells whether it worked (a failure is shown as an error). Public for
    /// tests.
    QString runCommand(const QString& text, bool* ok = nullptr);

    /// The list host (the "list" context: any list, tree or grid with the focus).
    [[nodiscard]] VimListHost* listHost() const { return m_listHost; }
    [[nodiscard]] VimFormHost* formHost() const { return m_formHost; }
    /// Gives the focus to the panel with key @p name (a panel key such as "media_pool", or one
    /// of :panel's names: media effects inspector timeline viewer keyframes mixer nodes
    /// metadata index) on the shown page, showing it first when it is hidden. False when this
    /// page has no such panel.
    bool focusPanelKey(const QString& name);
    /// Opens a picker: "media", "commands", "effects", "timelines", "markers", "bins", "help",
    /// "keymaps" or "history". False for another name.
    bool openPicker(const QString& kind);
    /// The lines of the `:` history, oldest first.
    [[nodiscard]] QStringList history() const;

    /// Opens the `:` line (also what the layer does for the colon key).
    void showCommandLine(bool search = false);

private:
    void closeCommandLine();
    void showResult(const QString& message, bool ok);
    void saveHistory() const;
    void placeWildMenu();
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool handleCommand(const QString& name, int count, bool hasCount, const QString& arg);
    // panels, windows, pages, lists (VimWindows.cpp)
    bool windowCommand(const QString& name, int count, bool hasCount, const QString& arg);
    bool newBin();
    [[nodiscard]] PanelFrame* currentFrame() const;
    [[nodiscard]] PanelFrame* findPanel(const QString& key, bool show);
    void navigate(int direction);
    void zoomToggle();
    void restoreZoom();
    void afterPageChange();
    void searchInList();
    bool upOneBin();
    // ex commands (VimExCommands.cpp)
    QString runBuiltin(const vimex::Parsed& parsed, bool* ok);
    QString gotoTime(const QString& text, bool* ok);
    // pickers (VimPickers.cpp)
    QVector<quint64> cursorClips() const;
    QStringList timelineNames() const;
    QStringList timecodes() const;
    void updateModeLine();
    void updateFocusFrame();
    void updateWhichKey();

    AppContext& m_ctx;
    QWidget* m_window;
    commands::LiveTarget m_target;
    VimInput* m_input = nullptr;
    VimTimelineHost* m_host = nullptr;
    PageBar* m_pageBar = nullptr;
    VimModeLine* m_modeLine = nullptr;
    VimWhichKey* m_whichKey = nullptr;
    QPointer<PanelFrame> m_framed; ///< the panel drawn with the focus frame
    QPointer<PanelFrame> m_previousFrame; ///< the panel that had it before (<C-w>p)
    QVector<QPointer<PanelFrame>> m_zoomHidden; ///< the panels <C-w>o hid
    VimListHost* m_listHost = nullptr;
    VimFormHost* m_formHost = nullptr;
    bool m_searchInList = false; ///< the `/` line searches the rows of a list
    QPointer<QWidget> m_returnFocus; ///< where the focus goes when the `:` line closes
    VimCommandLine* m_line = nullptr;
    VimPicker* m_picker = nullptr;
    QFrame* m_wild = nullptr;
    QLabel* m_wildText = nullptr;
    QStringList m_exHistory;
    QStringList m_searchHistory;
    bool m_searchLine = false; ///< the line is open for /search
};

} // namespace kk
