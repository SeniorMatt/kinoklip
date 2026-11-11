// SPDX-License-Identifier: GPL-3.0-or-later
// PageBar implementation.
#include "app/PageBar.h"

#include "ui/common/Metrics.h"
#include "ui/common/SpringLoad.h"

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
    m_root = new QHBoxLayout(this);
    m_root->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceM, 0);
    m_root->addStretch(1);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(metrics::kSpaceM);
    m_root->addLayout(m_buttons);
    m_root->addStretch(1);
}

void PageBar::setLeadingWidget(QWidget* widget)
{
    m_root->insertWidget(0, widget);
}

void PageBar::setOverlayWidget(QWidget* widget)
{
    m_overlay = widget;
    if (m_overlay) {
        m_overlay->setParent(this);
        m_overlay->setGeometry(rect().adjusted(metrics::kSpaceM, 3, -metrics::kSpaceM, -3));
    }
}

void PageBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_overlay)
        m_overlay->setGeometry(rect().adjusted(metrics::kSpaceM, 3, -metrics::kSpaceM, -3));
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
    new SpringLoad(button); // hovering a drag over the page switches to it (spec 71)
    m_buttons->addWidget(button);
}

} // namespace kk
