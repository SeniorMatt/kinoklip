// SPDX-License-Identifier: GPL-3.0-or-later
// InterfaceBar implementation: a 3-column grid keeps the title centred whatever the button widths.
#include "app/InterfaceBar.h"

#include <QAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace cw {

namespace {
constexpr int kBarHeight = 34;
constexpr int kIconSize = 16;
} // namespace

InterfaceBar::InterfaceBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("InterfaceBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(kBarHeight);

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(6, 0, 6, 0);
    m_left = new QHBoxLayout;
    m_left->setSpacing(2);
    m_right = new QHBoxLayout;
    m_right->setSpacing(2);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("ProjectTitle"));
    m_title->setAlignment(Qt::AlignCenter);

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
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);
}

void InterfaceBar::addLeftAction(QAction* action)
{
    addButton(m_left, action);
}

void InterfaceBar::addRightAction(QAction* action)
{
    addButton(m_right, action);
}

void InterfaceBar::setTitle(const QString& title)
{
    m_title->setText(title);
}

} // namespace cw
