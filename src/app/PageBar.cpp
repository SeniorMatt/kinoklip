// SPDX-License-Identifier: GPL-3.0-or-later
// PageBar implementation.
#include "app/PageBar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QToolButton>

namespace cw {

namespace {
constexpr int kBarHeight = 40;
constexpr int kIconSize = 18;
constexpr int kButtonMinWidth = 96;
} // namespace

PageBar::PageBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PageBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(kBarHeight);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->addStretch(1);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(24);
    layout->addLayout(m_buttons);
    layout->addStretch(1);
}

void PageBar::addPageAction(QAction* pageAction)
{
    auto* button = new QToolButton(this);
    button->setDefaultAction(pageAction);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setMinimumWidth(kButtonMinWidth);
    button->setFocusPolicy(Qt::NoFocus);
    m_buttons->addWidget(button);
}

} // namespace cw
