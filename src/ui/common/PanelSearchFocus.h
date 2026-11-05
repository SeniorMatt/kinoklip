// SPDX-License-Identifier: GPL-3.0-or-later
// PanelSearchFocus — gives a panel's search field the focus when the panel is opened with its
// toggle's keyboard shortcut (Preferences > Keyboard, off by default; spec 49.5).
#pragma once

#include "ui/common/Page.h"

#include <QObject>
#include <QPointer>

class QAction;

namespace kk {

/// Tells whether a QAction is being triggered by its keyboard shortcut: it watches the actions'
/// QEvent::Shortcut, which Qt delivers to the action right before it emits triggered(). A click
/// on a tool button or a menu entry triggers without it.
///
/// Thread-safety: GUI thread only.
class ShortcutTriggerWatch : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    /// Starts watching @p action.
    void watch(QAction* action);
    /// True while @p action's triggered() comes from its shortcut (call it from a slot
    /// connected to triggered()).
    [[nodiscard]] bool firedByShortcut(const QAction* action) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<QObject> m_firing; ///< the action whose shortcut is being delivered
};

namespace panelsearch {

/// The PanelFrame key of the panels that have a search field ("media_pool", "effects",
/// "edit_index"), or an empty string.
[[nodiscard]] QString searchPanelKey(PanelId id);

/// Focuses the search field of the visible panel @p id inside @p page. Returns false when the
/// panel has no search field or is not shown.
bool focusSearch(QWidget* page, PanelId id);

} // namespace panelsearch

} // namespace kk
