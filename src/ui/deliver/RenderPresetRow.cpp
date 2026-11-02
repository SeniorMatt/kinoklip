// SPDX-License-Identifier: GPL-3.0-or-later
// RenderPresetRow implementation.
#include "ui/deliver/RenderPresetRow.h"

#include "ui/common/Metrics.h"

#include <QButtonGroup>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextLayout>
#include <QToolButton>

#include <algorithm>

namespace kk {

namespace {
constexpr int kTileWidth = 96; ///< px; fits "YouTube 1080p" / "Vimeo 1080p" on one line
constexpr int kTileIconSize = 28;
constexpr int kTextLines = 2;
constexpr char kIdProperty[] = "kkPresetId";

/// @p text wrapped into at most kTextLines lines of @p width px; the last line is elided.
QString twoLineText(const QString& text, const QFont& font, int width)
{
    const QFontMetrics metrics(font);
    QTextLayout layout(text, font);
    layout.beginLayout();
    QStringList lines;
    int start = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (lines.size() == kTextLines - 1) {
            lines.append(metrics.elidedText(text.mid(start), Qt::ElideRight, width));
            break;
        }
        lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
        start = line.textStart() + line.textLength();
    }
    layout.endLayout();
    return lines.join(QLatin1Char('\n'));
}

QToolButton* arrowButton(Qt::ArrowType arrow, const QString& tip)
{
    auto* button = new QToolButton;
    button->setArrowType(arrow);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    return button;
}
} // namespace

RenderPresetRow::RenderPresetRow(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("RenderPresetStrip"));
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    m_strip = new QWidget;
    m_stripLayout = new QHBoxLayout(m_strip);
    m_stripLayout->setContentsMargins(0, 0, 0, 0);
    m_stripLayout->setSpacing(metrics::kSpaceXs);
    m_stripLayout->addStretch(1);

    m_scroll = new QScrollArea;
    m_scroll->setWidget(m_strip);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_strip->setAutoFillBackground(false);

    m_left = arrowButton(Qt::LeftArrow, tr("Previous presets"));
    m_right = arrowButton(Qt::RightArrow, tr("More presets"));
    connect(m_left, &QToolButton::clicked, this, [this] { scrollBy(-1); });
    connect(m_right, &QToolButton::clicked, this, [this] { scrollBy(1); });
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this,
        &RenderPresetRow::updateArrows);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::rangeChanged, this,
        &RenderPresetRow::updateArrows);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_left);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_right);

    connect(m_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* button) {
        emit presetClicked(button->property(kIdProperty).toString());
    });
    updateArrows();
}

void RenderPresetRow::clear()
{
    for (QAbstractButton* button : m_group->buttons()) {
        m_group->removeButton(button);
        delete button;
    }
}

void RenderPresetRow::addPreset(
    const QString& id, const QString& name, const QIcon& icon, const QString& toolTip)
{
    auto* tile = new QToolButton(m_strip);
    tile->setObjectName(QStringLiteral("PresetTile_") + id);
    tile->setProperty(kIdProperty, id);
    tile->setCheckable(true);
    tile->setAutoRaise(true);
    tile->setFocusPolicy(Qt::NoFocus);
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setIcon(icon);
    tile->setIconSize(QSize(kTileIconSize, kTileIconSize));
    const int textWidth = kTileWidth - 2 * metrics::kSpaceS;
    tile->setText(twoLineText(name, tile->font(), textWidth));
    tile->setToolTip(toolTip.isEmpty() ? name : name + QLatin1Char('\n') + toolTip);
    tile->setFixedWidth(kTileWidth);
    // Every tile reserves two text lines, so the row height never depends on the names.
    const int height
        = kTileIconSize + kTextLines * tile->fontMetrics().lineSpacing() + 3 * metrics::kSpaceS;
    tile->setFixedHeight(height);
    m_scroll->setFixedHeight(height);
    m_group->addButton(tile);
    m_stripLayout->insertWidget(m_stripLayout->count() - 1, tile);
}

void RenderPresetRow::setCurrent(const QString& id)
{
    QToolButton* match = tile(id);
    if (!match) {
        // An exclusive group cannot uncheck its last button: lift exclusivity briefly.
        m_group->setExclusive(false);
        for (QAbstractButton* button : m_group->buttons())
            button->setChecked(false);
        m_group->setExclusive(true);
        return;
    }
    match->setChecked(true);
    m_scroll->ensureWidgetVisible(match, 0, 0);
}

QString RenderPresetRow::current() const
{
    QAbstractButton* checked = m_group->checkedButton();
    return checked ? checked->property(kIdProperty).toString() : QString();
}

int RenderPresetRow::count() const
{
    return int(m_group->buttons().size());
}

QToolButton* RenderPresetRow::tile(const QString& id) const
{
    if (id.isEmpty())
        return nullptr;
    for (QAbstractButton* button : m_group->buttons()) {
        if (button->property(kIdProperty).toString() == id)
            return static_cast<QToolButton*>(button);
    }
    return nullptr;
}

void RenderPresetRow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateArrows();
}

void RenderPresetRow::scrollBy(int direction)
{
    QScrollBar* bar = m_scroll->horizontalScrollBar();
    // One page minus one tile, so the last visible tile stays as orientation.
    const int step = std::max(kTileWidth, m_scroll->viewport()->width() - kTileWidth);
    bar->setValue(bar->value() + direction * step);
}

void RenderPresetRow::updateArrows()
{
    const QScrollBar* bar = m_scroll->horizontalScrollBar();
    const bool scrollable = bar->maximum() > 0;
    m_left->setVisible(scrollable);
    m_right->setVisible(scrollable);
    m_left->setEnabled(bar->value() > bar->minimum());
    m_right->setEnabled(bar->value() < bar->maximum());
}

} // namespace kk
