// SPDX-License-Identifier: GPL-3.0-or-later
// PageBar implementation.
#include "app/PageBar.h"

#include "ui/common/Metrics.h"

#include <QAction>
#include <QHBoxLayout>
#include <QToolButton>

namespace kk {

namespace {
constexpr int kBarHeight = metrics::kPageBarHeight;
constexpr int kIconSize = metrics::kTransportIconSize;
/// Equal button widths keep the seven pages evenly spaced like Resolve's page bar.
constexpr int kButtonMinWidth = 112;
} // namespace

PageBar::PageBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PageBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(kBarHeight);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceM, 0);
    layout->addStretch(1);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(metrics::kSpaceM);
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
    button->setAutoRaise(true); // flat until hovered/checked: the Breeze toolbar look
    button->setFocusPolicy(Qt::NoFocus);
    m_buttons->addWidget(button);
}

} // namespace kk
