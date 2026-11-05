// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/Page.h"

#include <QSplitter>

namespace kk {

void Page::captureDefaultLayout()
{
    m_defaultSplitters.clear();
    for (QSplitter* splitter : findChildren<QSplitter*>())
        m_defaultSplitters.append({splitter, splitter->saveState()});
    m_defaultPanels.clear();
    for (PanelId id : kAllPanels) {
        if (hasPanel(id))
            m_defaultPanels.append({id, isPanelVisible(id)});
    }
}

void Page::resetLayout()
{
    for (const auto& [splitter, state] : std::as_const(m_defaultSplitters)) {
        if (splitter)
            splitter->restoreState(state);
    }
    for (const auto& [id, visible] : std::as_const(m_defaultPanels))
        setPanelVisible(id, visible);
}

} // namespace kk
