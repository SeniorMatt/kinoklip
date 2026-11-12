// SPDX-License-Identifier: GPL-3.0-or-later
// VimModeLine — the permanent Vim mode line (spec section 83): a coloured chip with the mode
// (NORMAL, VISUAL, INSERT, COMMAND), the keys typed so far, the focused panel, and the last
// message. It sits at the left of the page bar on every page and is hidden while Vim mode is
// off.
#pragma once

#include <QWidget>

class QLabel;
class QTimer;

namespace kk {

class VimModeLine : public QWidget
{
    Q_OBJECT
public:
    explicit VimModeLine(QWidget* parent = nullptr);

    /// The mode chip: @p text is the visible name, @p mode its colour key ("NORMAL", "VISUAL",
    /// "INSERT", "COMMAND").
    void setMode(const QString& text, const QString& mode);
    void setPending(const QString& keys);
    void setPanel(const QString& name);
    /// Shows @p text after the panel for @p ms milliseconds (0 = until replaced).
    void showMessage(const QString& text, int ms = 3000);
    /// Like showMessage() in the error colour (an ex command that failed).
    void showError(const QString& text, int ms = 5000);
    [[nodiscard]] bool messageIsError() const { return m_error; }
    void clearMessage();

    [[nodiscard]] QString modeText() const;
    [[nodiscard]] QString pendingText() const;
    [[nodiscard]] QString panelText() const;
    [[nodiscard]] QString messageText() const;

private:
    QLabel* m_chip = nullptr;
    QLabel* m_pending = nullptr;
    QLabel* m_panel = nullptr;
    QLabel* m_message = nullptr;
    QTimer* m_messageTimer = nullptr;
    bool m_error = false;
};

} // namespace kk
