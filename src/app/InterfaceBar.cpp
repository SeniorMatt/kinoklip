// SPDX-License-Identifier: GPL-3.0-or-later
// InterfaceBar implementation: a 3-column grid keeps the title centred whatever the button widths.
#include "app/InterfaceBar.h"

#include "ui/common/Metrics.h"
#include "ui/common/SpringLoad.h"

#include <QAbstractButton>
#include <QAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace kk {

namespace {
constexpr int kBarHeight = metrics::kInterfaceBarHeight;
constexpr int kIconSize = metrics::kToolIconSize;
} // namespace

InterfaceBar::InterfaceBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("InterfaceBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(kBarHeight);

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceM, 0);
    m_left = new QHBoxLayout;
    m_left->setSpacing(metrics::kSpaceXs);
    m_right = new QHBoxLayout;
    m_right->setSpacing(metrics::kSpaceXs);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("ProjectTitle"));
    m_title->setAlignment(Qt::AlignCenter);

    m_remote = new QLabel(tr("Assistant connected"), this);
    m_remote->setObjectName(QStringLiteral("RemoteControlIndicator"));
    m_remote->setToolTip(tr("A local tool (an AI assistant through MCP) is connected and can "
                            "change this project. Each change is one step in Edit > Undo. Switch "
                            "it off in Preferences > Local Control."));
    m_remote->hide();
    m_right->addWidget(m_remote);

    grid->addLayout(m_left, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(m_title, 0, 1, Qt::AlignCenter);
    grid->addLayout(m_right, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);
}

void InterfaceBar::addButton(QHBoxLayout* layout, QAction* action)
{
    if (!action)
        return;
    auto* button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setAutoRaise(true); // flat until hovered/checked: the Breeze toolbar look
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);
    m_buttons.insert(action, button);
}

void InterfaceBar::setActionShown(QAction* action, bool shown)
{
    if (QWidget* button = m_buttons.value(action))
        button->setVisible(shown);
}

void InterfaceBar::enableSpringLoad(QAction* action)
{
    if (auto* button = qobject_cast<QAbstractButton*>(m_buttons.value(action)))
        new SpringLoad(button);
}

void InterfaceBar::addLeftAction(QAction* action)
{
    addButton(m_left, action);
}

void InterfaceBar::addRightAction(QAction* action)
{
    addButton(m_right, action);
}

void InterfaceBar::setRemoteControlClients(int clients)
{
    m_remote->setVisible(clients > 0);
}

void InterfaceBar::setTitle(const QString& title)
{
    m_title->setText(title);
}

} // namespace kk
