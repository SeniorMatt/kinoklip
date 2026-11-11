// SPDX-License-Identifier: GPL-3.0-or-later
// PanelFrame implementation and placeholder helpers.
#include "ui/common/PanelFrame.h"

#include "ui/common/Metrics.h"

#include <QAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSettings>
#include <QSplitter>
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

bool PanelFrame::focusSearchField()
{
    if (!m_searchField || !m_searchField->isEnabled())
        return false;
    m_searchField->setFocus(Qt::ShortcutFocusReason);
    m_searchField->selectAll();
    return true;
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

namespace {

/// The Vim focus frame: a border drawn over the panel, ignoring the mouse.
class FocusRing final : public QWidget
{
public:
    explicit FocusRing(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("VimFocusFrame"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QPen pen(palette().color(QPalette::Highlight), 2);
        pen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(pen);
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
};

} // namespace

void PanelFrame::setVimFocusFrame(bool on)
{
    if (on && !m_focusRing)
        m_focusRing = new FocusRing(this);
    if (!m_focusRing)
        return;
    if (on) {
        m_focusRing->setGeometry(rect());
        m_focusRing->show();
        m_focusRing->raise();
    } else {
        m_focusRing->hide();
    }
}

void PanelFrame::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    if (m_focusRing && m_focusRing->isVisible()) {
        m_focusRing->setGeometry(rect());
        m_focusRing->raise();
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

void PanelFrame::saveSplitterSizes(QSettings& settings, const QString& prefix) const
{
    // findChildren() lists them in creation order, the same in every run.
    const QList<QSplitter*> splitters = findChildren<QSplitter*>();
    for (qsizetype i = 0; i < splitters.size(); ++i)
        settings.setValue(prefix + QStringLiteral("splitter%1").arg(i), splitters[i]->saveState());
}

void PanelFrame::restoreSplitterSizes(const QSettings& settings, const QString& prefix)
{
    const QList<QSplitter*> splitters = findChildren<QSplitter*>();
    for (qsizetype i = 0; i < splitters.size(); ++i) {
        const QString key = prefix + QStringLiteral("splitter%1").arg(i);
        if (settings.contains(key))
            splitters[i]->restoreState(settings.value(key).toByteArray());
    }
}

} // namespace kk
