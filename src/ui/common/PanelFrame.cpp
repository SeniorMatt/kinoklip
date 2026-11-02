// SPDX-License-Identifier: GPL-3.0-or-later
// PanelFrame implementation and placeholder helpers.
#include "ui/common/PanelFrame.h"

#include "ui/common/Metrics.h"

#include <QAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kTitleBarHeight = metrics::kPanelTitleHeight;
constexpr int kTitleIconSize = metrics::kToolIconSize;
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

    // Three columns: leading widgets (+ title when left aligned) | centred title | tools.
    // Columns 0 and 2 stretch equally, so a centred title stays in the middle of the bar
    // whatever the widths of the side widgets (same trick as the InterfaceBar).
    m_titleGrid = new QGridLayout(m_titleBar);
    m_titleGrid->setContentsMargins(metrics::kSpaceM, 0, metrics::kSpaceS, 0);
    m_titleGrid->setHorizontalSpacing(metrics::kSpaceM);
    m_leadingLayout = new QHBoxLayout;
    m_leadingLayout->setSpacing(metrics::kSpaceS);
    m_leadingLayout->addWidget(m_titleLabel);
    m_leadingLayout->addStretch(1);
    m_toolsLayout = new QHBoxLayout;
    m_toolsLayout->setSpacing(metrics::kSpaceS);
    m_toolsLayout->addStretch(1);
    m_titleGrid->addLayout(m_leadingLayout, 0, 0);
    m_titleGrid->addLayout(m_toolsLayout, 0, 2);
    m_titleGrid->setColumnStretch(0, 1);
    m_titleGrid->setColumnStretch(2, 1);

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
    button->setFocusPolicy(Qt::NoFocus);
    m_toolsLayout->addWidget(button);
    return button;
}

void PanelFrame::addTitleBarWidget(QWidget* widget)
{
    widget->setParent(m_titleBar);
    m_toolsLayout->addWidget(widget);
}

void PanelFrame::addTitleBarLeadingWidget(QWidget* widget)
{
    widget->setParent(m_titleBar);
    // Before the title label (which sits in this layout while left aligned) or, when the
    // title is centred, before the trailing stretch.
    const int titleIndex = m_leadingLayout->indexOf(m_titleLabel);
    const int stretchIndex = m_leadingLayout->count() - 1;
    m_leadingLayout->insertWidget(titleIndex >= 0 ? titleIndex : stretchIndex, widget);
}

void PanelFrame::setTitleCentered(bool centered)
{
    const bool isCentered = m_leadingLayout->indexOf(m_titleLabel) < 0;
    if (centered == isCentered)
        return;
    if (centered) {
        m_leadingLayout->removeWidget(m_titleLabel);
        m_titleGrid->addWidget(m_titleLabel, 0, 1, Qt::AlignCenter);
    } else {
        m_titleGrid->removeWidget(m_titleLabel);
        m_leadingLayout->insertWidget(m_leadingLayout->count() - 1, m_titleLabel);
    }
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
    label->setMargin(metrics::kSpaceXl);
    return label;
}

PanelFrame* createPlaceholderPanel(const QString& title, const QString& text, QWidget* parent)
{
    auto* panel = new PanelFrame(title, parent);
    panel->setContentWidget(createPlaceholderContent(text));
    return panel;
}

} // namespace kk
