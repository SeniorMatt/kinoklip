// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/PanelInfo.h"

#include <QWidget>

namespace kk {

namespace {
constexpr char kNameProperty[] = "kkPanelName";
constexpr char kDescriptionProperty[] = "kkPanelDescription";
} // namespace

void setPanelInfo(QWidget* widget, const QString& name, const QString& description)
{
    widget->setProperty(kNameProperty, name);
    widget->setProperty(kDescriptionProperty, description);
}

QString panelInfoName(const QWidget* widget)
{
    return widget->property(kNameProperty).toString();
}

QString panelInfoDescription(const QWidget* widget)
{
    return widget->property(kDescriptionProperty).toString();
}

} // namespace kk
