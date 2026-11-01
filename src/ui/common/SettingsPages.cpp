// SPDX-License-Identifier: GPL-3.0-or-later
// SettingsPageRegistry implementation.
#include "ui/common/SettingsPages.h"

#include <algorithm>

namespace kk {

void SettingsPageRegistry::add(SettingsPageInfo page)
{
    for (SettingsPageInfo& existing : m_pages) {
        if (existing.id == page.id) {
            existing = std::move(page);
            return;
        }
    }
    m_pages.append(std::move(page));
}

QVector<SettingsPageInfo> SettingsPageRegistry::pages() const
{
    QVector<SettingsPageInfo> sorted = m_pages;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const SettingsPageInfo& a, const SettingsPageInfo& b) { return a.order < b.order; });
    return sorted;
}

bool SettingsPageRegistry::contains(const QString& id) const
{
    return std::any_of(m_pages.cbegin(), m_pages.cend(),
        [&id](const SettingsPageInfo& page) { return page.id == id; });
}

} // namespace kk
