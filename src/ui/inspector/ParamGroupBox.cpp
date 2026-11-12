// SPDX-License-Identifier: GPL-3.0-or-later
// ParamGroupBox implementation.
#include "ui/inspector/ParamGroupBox.h"

#include "ui/inspector/ParamRows.h"

#include <QAction>
#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kHeaderHeight = 24;
constexpr int kHeaderButtonSize = 20;
constexpr int kLabelColumnMinWidth = 96;
} // namespace

ParamGroupBox::ParamGroupBox(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("InspectorGroup"));
    setAttribute(Qt::WA_StyledBackground);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("InspectorGroupHeader"));
    m_header->setAttribute(Qt::WA_StyledBackground);
    m_header->setFixedHeight(kHeaderHeight);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setProperty(VimRowOps::kRowProperty, QVariant::fromValue<QObject*>(this));
    m_header->installEventFilter(this);
    m_headerLayout = new QHBoxLayout(m_header);
    m_headerLayout->setContentsMargins(2, 0, 4, 0);
    m_headerLayout->setSpacing(4);

    m_chevron = new QToolButton(m_header);
    m_chevron->setAutoRaise(true);
    m_chevron->setFixedSize(kHeaderButtonSize, kHeaderButtonSize);
    m_chevron->setIcon(inspectorGlyph(InspectorGlyph::Expanded));
    m_chevron->setFocusPolicy(Qt::NoFocus);
    m_chevron->setToolTip(tr("Collapse or expand this section"));
    connect(m_chevron, &QToolButton::clicked, this, [this] {
        setCollapsed(!m_collapsed);
        emit collapsedChanged(m_collapsed);
    });
    m_title = new QLabel(title, m_header);
    m_title->setObjectName(QStringLiteral("InspectorGroupTitle"));
    m_headerLayout->addWidget(m_chevron);
    m_headerLayout->addWidget(m_title, 1);

    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("InspectorGroupBody"));
    m_grid = new QGridLayout(m_body);
    m_grid->setContentsMargins(8, 6, 6, 8);
    m_grid->setHorizontalSpacing(6);
    m_grid->setVerticalSpacing(4);
    m_grid->setColumnMinimumWidth(paramgrid::kLabel, kLabelColumnMinWidth);
    m_grid->setColumnStretch(paramgrid::kSlider, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_header);
    layout->addWidget(m_body);
}

ParamRow* ParamGroupBox::addParam(
    const ParamRef& ref, const ParamDescriptor& desc, ParamEditHost& host)
{
    ParamRow* row = ParamRow::create(ref, desc, host, m_grid, m_nextRow++, m_body);
    if (row)
        m_rows.append(row);
    return row;
}

void ParamGroupBox::addWidgetRow(const QString& label, QWidget* widget)
{
    const int row = m_nextRow++;
    if (!label.isEmpty()) {
        auto* text = new QLabel(label, m_body);
        text->setObjectName(QStringLiteral("InspectorParamLabel"));
        m_grid->addWidget(text, row, paramgrid::kLabel, Qt::AlignRight | Qt::AlignVCenter);
    }
    widget->setParent(m_body);
    m_grid->addWidget(widget, row, paramgrid::kEditor, 1, 2);
}

QCheckBox* ParamGroupBox::addEnableToggle()
{
    auto* box = new QCheckBox(m_header);
    box->setFocusPolicy(Qt::NoFocus);
    box->setToolTip(tr("Enable or disable"));
    m_headerLayout->insertWidget(1, box);
    m_enable = box;
    return box;
}

QToolButton* ParamGroupBox::addHeaderButton(InspectorGlyph glyph, const QString& toolTip)
{
    auto* button = new QToolButton(m_header);
    button->setAutoRaise(true);
    button->setFixedSize(kHeaderButtonSize, kHeaderButtonSize);
    button->setIcon(inspectorGlyph(glyph));
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    m_headerLayout->addWidget(button);
    switch (glyph) {
    case InspectorGlyph::Remove:
        m_removeButton = button;
        break;
    case InspectorGlyph::Reset:
        m_resetButton = button;
        break;
    case InspectorGlyph::MoveUp:
        m_upButton = button;
        break;
    case InspectorGlyph::MoveDown:
        m_downButton = button;
        break;
    default:
        break;
    }
    return button;
}

void ParamGroupBox::addHeaderWidget(QWidget* widget)
{
    widget->setParent(m_header);
    m_headerLayout->addWidget(widget);
}

void ParamGroupBox::setTitle(const QString& title)
{
    m_title->setText(title);
}

QString ParamGroupBox::title() const
{
    return m_title->text();
}

void ParamGroupBox::addHeaderMenuAction(QAction* action)
{
    m_header->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_header->addAction(action);
}

QList<QAction*> ParamGroupBox::headerMenuActions() const
{
    return m_header->actions();
}

void ParamGroupBox::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    m_body->setVisible(!collapsed);
    m_chevron->setIcon(
        inspectorGlyph(collapsed ? InspectorGlyph::Collapsed : InspectorGlyph::Expanded));
}

void ParamGroupBox::refresh(const ClipData& clip, int64_t localFrame, bool playheadInside)
{
    if (m_headerRefresh)
        m_headerRefresh(clip);
    for (ParamRow* row : std::as_const(m_rows))
        row->refresh(clip, localFrame, playheadInside);
}

bool ParamGroupBox::vimFold(int mode)
{
    const bool collapse = mode == 0 ? !m_collapsed : mode < 0;
    if (collapse == m_collapsed)
        return true;
    setCollapsed(collapse);
    emit collapsedChanged(m_collapsed);
    return true;
}

bool ParamGroupBox::vimSwitch()
{
    if (!m_enable || !m_enable->isEnabled())
        return false;
    m_enable->click();
    return true;
}

bool ParamGroupBox::vimRemove()
{
    // An effect, mask or transition goes; a section that cannot be removed resets.
    QToolButton* button = m_removeButton ? m_removeButton : m_resetButton;
    if (!button || !button->isEnabled())
        return false;
    button->click();
    return true;
}

bool ParamGroupBox::vimMove(int delta)
{
    QToolButton* button = delta < 0 ? m_upButton : m_downButton;
    if (!button || !button->isEnabled())
        return false;
    button->click();
    return true;
}

bool ParamGroupBox::eventFilter(QObject* watched, QEvent* event)
{
    // A click anywhere on the header background toggles the group, like the chevron.
    if (watched == m_header && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        setCollapsed(!m_collapsed);
        emit collapsedChanged(m_collapsed);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
