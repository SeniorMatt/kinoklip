// SPDX-License-Identifier: GPL-3.0-or-later
// PanelFrame implementation and placeholder helpers.
#include "ui/common/PanelFrame.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace cw {

namespace {
constexpr int kTitleBarHeight = 26; // px; compact like most NLE panel headers
constexpr int kTitleIconSize = 16;
} // namespace

PanelFrame::PanelFrame(const QString& title, QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("PanelFrame"));
    setAttribute(Qt::WA_StyledBackground);

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("PanelTitleBar"));
    m_titleBar->setAttribute(Qt::WA_StyledBackground);
    m_titleBar->setFixedHeight(kTitleBarHeight);

    m_titleLabel = new QLabel(title, m_titleBar);
    m_titleLabel->setObjectName(QStringLiteral("PanelTitle"));

    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(10, 0, 4, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch(1);
    m_toolsLayout = new QHBoxLayout;
    m_toolsLayout->setSpacing(2);
    titleLayout->addLayout(m_toolsLayout);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_titleBar);
}

QString PanelFrame::title() const
{
    return m_titleLabel->text();
}

void PanelFrame::setTitle(const QString& title)
{
    m_titleLabel->setText(title);
}

void PanelFrame::setContentWidget(QWidget* widget)
{
    if (m_content == widget)
        return;
    delete m_content;
    m_content = widget;
    if (m_content) {
        m_content->setParent(this);
        static_cast<QVBoxLayout*>(layout())->addWidget(m_content, 1);
    }
}

QToolButton* PanelFrame::addTitleBarAction(QAction* action)
{
    auto* button = new QToolButton(m_titleBar);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setIconSize(QSize(kTitleIconSize, kTitleIconSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_toolsLayout->addWidget(button);
    return button;
}

void PanelFrame::addTitleBarWidget(QWidget* widget)
{
    widget->setParent(m_titleBar);
    m_toolsLayout->addWidget(widget);
}

void PanelFrame::setTitleBarVisible(bool visible)
{
    m_titleBar->setVisible(visible);
}

QWidget* createPlaceholderContent(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("PlaceholderText"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMargin(16);
    return label;
}

PanelFrame* createPlaceholderPanel(const QString& title, const QString& text, QWidget* parent)
{
    auto* panel = new PanelFrame(title, parent);
    panel->setContentWidget(createPlaceholderContent(text));
    return panel;
}

} // namespace cw
