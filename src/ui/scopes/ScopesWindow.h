// SPDX-License-Identifier: GPL-3.0-or-later
// ScopesWindow — the floating Video Scopes window (View > Video Scopes, any page): two scopes
// side by side over the program picture, like Resolve's workspace scopes window.
#pragma once

#include <QWidget>

namespace kk {

class PlaybackEngine;
class ScopeFeeder;
class ScopesPanel;

/// A tool window (child of the main window, never on top of other applications) with two
/// ScopesPanels (default: Parade and Vectorscope, remembered) fed by a ScopeFeeder while it is
/// visible. Closing it unchecks its toggle action (closed() signal).
class ScopesWindow : public QWidget
{
    Q_OBJECT
public:
    ScopesWindow(PlaybackEngine* engine, QWidget* parent = nullptr);

    [[nodiscard]] ScopesPanel* panel(int index) const { return index == 0 ? m_left : m_right; }
    [[nodiscard]] ScopeFeeder* feeder() const { return m_feeder; }

Q_SIGNALS:
    void closed();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    ScopeFeeder* m_feeder = nullptr;
    ScopesPanel* m_left = nullptr;
    ScopesPanel* m_right = nullptr;
};

} // namespace kk
