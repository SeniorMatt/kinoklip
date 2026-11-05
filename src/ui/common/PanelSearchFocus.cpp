// SPDX-License-Identifier: GPL-3.0-or-later
// PanelSearchFocus implementation.
#include "ui/common/PanelSearchFocus.h"

#include "ui/common/PanelFrame.h"

#include <QAction>
#include <QEvent>
#include <QTimer>

namespace kk {

void ShortcutTriggerWatch::watch(QAction* action)
{
    if (action)
        action->installEventFilter(this);
}

bool ShortcutTriggerWatch::firedByShortcut(const QAction* action) const
{
    return action && m_firing == action;
}

bool ShortcutTriggerWatch::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Shortcut) {
        // QAction::event() handles it synchronously after this filter: triggered() runs while
        // m_firing is set; the next event loop pass forgets it again.
        m_firing = watched;
        QTimer::singleShot(0, this, [this, watched] {
            if (m_firing == watched)
                m_firing = nullptr;
        });
    }
    return QObject::eventFilter(watched, event);
}

namespace panelsearch {

QString searchPanelKey(PanelId id)
{
    switch (id) {
    case PanelId::MediaPool:
        return QStringLiteral("media_pool");
    case PanelId::Effects:
        return QStringLiteral("effects");
    case PanelId::EditIndex:
        return QStringLiteral("edit_index");
    default:
        return {};
    }
}

bool focusSearch(QWidget* page, PanelId id)
{
    const QString key = searchPanelKey(id);
    if (!page || key.isEmpty())
        return false;
    for (PanelFrame* panel : page->findChildren<PanelFrame*>()) {
        if (panel->panelKey() == key && panel->isVisible() && panel->focusSearchField())
            return true;
    }
    return false;
}

} // namespace panelsearch

} // namespace kk
