// SPDX-License-Identifier: GPL-3.0-or-later
// ListKeyGuard — keeps the window-wide timeline editing and navigation keys away from a focused
// list or tree panel (the Effects library, the media pool, Edit Index, bins, the node library):
// there Delete, Backspace, Ctrl+A, the arrow keys, Home / End and Page Up / Down act on the list,
// as in Resolve, instead of ripple-deleting, selecting or stepping the timeline (spec 10.11
// in docs/behavior-spec.md). A focused combo box or spin box likewise keeps Up / Down and
// Page Up / Down (spec 10.12). Transport keys (Space, J / K / L) still reach the window.
#pragma once

#include <QObject>

class QKeyEvent;
class QWidget;

namespace kk {

class ListKeyGuard : public QObject
{
    Q_OBJECT
public:
    /// Installs itself as an application event filter (removed again when destroyed).
    explicit ListKeyGuard(QObject* parent = nullptr);
    ~ListKeyGuard() override;

    /// True if @p key, pressed with @p focus focused, belongs to the focused list or tree view
    /// rather than to a window shortcut. A view opts out with the dynamic property
    /// "kkTimelineKeys" set to true.
    [[nodiscard]] static bool keyBelongsToList(const QWidget* focus, const QKeyEvent& key);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace kk
