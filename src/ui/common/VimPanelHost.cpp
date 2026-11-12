// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimPanelHost.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>

namespace kk {

VimPanelHost::VimPanelHost(ActionRegistry* actions)
    : m_actions(actions)
{ }

bool VimPanelHost::trigger(const QString& actionId)
{
    if (!m_actions || !m_actions->contains(actionId))
        return false;
    QAction* a = m_actions->action(actionId);
    if (!a->isEnabled())
        return false;
    a->trigger();
    return true;
}

void VimPanelHost::openCommandLine()
{
    if (onCommandLine)
        onCommandLine();
}

void VimPanelHost::openSearch()
{
    if (onSearch)
        onSearch();
}

void VimPanelHost::runEx(const QString& line)
{
    if (onEx)
        onEx(line);
}

void VimPanelHost::showMessage(const QString& text)
{
    if (onMessage)
        onMessage(text);
}

} // namespace kk
