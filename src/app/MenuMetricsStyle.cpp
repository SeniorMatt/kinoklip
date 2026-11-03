// SPDX-License-Identifier: GPL-3.0-or-later
// MenuMetricsStyle implementation.
#include "app/MenuMetricsStyle.h"

#include <QStyleOptionMenuItem>

namespace kk {

namespace {
/// Room for the submenu arrow plus the gap to the text (Breeze: 20 px indicator + 4 px).
constexpr int kSubmenuArrowSpace = 24;
} // namespace

MenuMetricsStyle::MenuMetricsStyle(QStyle* base)
    : QProxyStyle(base)
{
    // Keep the factory name so code asking for the style's name still sees "Breeze".
    if (base)
        setObjectName(base->objectName());
}

QSize MenuMetricsStyle::sizeFromContents(
    ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const
{
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_MenuItem) {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item && item->menuItemType == QStyleOptionMenuItem::SubMenu)
            result.rwidth() += kSubmenuArrowSpace;
    }
    return result;
}

int MenuMetricsStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
    QStyleHintReturn* returnData) const
{
    if (hint == SH_Menu_FadeOutOnHide || hint == SH_Menu_FlashTriggeredItem)
        return 0; // menus close at once, like Resolve's
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace kk
