// SPDX-License-Identifier: GPL-3.0-or-later
// TextFit — text in a non-resizable part grows its widget or is elided with a tooltip.
#include "ui/common/TextFit.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QHelpEvent>
#include <QLabel>
#include <QListView>
#include <QScrollBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>

namespace kk::textfit {

namespace {

/// Events after which a widget's text metrics may differ.
bool changesMetrics(QEvent::Type type)
{
    return type == QEvent::FontChange || type == QEvent::StyleChange || type == QEvent::Polish;
}

// ---- Section list width ----

class ListWidthFitter : public QObject
{
public:
    ListWidthFitter(QListView* list, int minimumWidth)
        : QObject(list)
        , m_list(list)
        , m_minimum(minimumWidth)
    {
        list->installEventFilter(this);
        QAbstractItemModel* model = list->model();
        // Queued: a list being destroyed resets its model, and the fit must not run then
        // (this filter, a child of the list, is gone by the time the queue is processed).
        const auto refit = [this] { fit(); };
        constexpr auto queued = Qt::QueuedConnection;
        connect(model, &QAbstractItemModel::rowsInserted, this, refit, queued);
        connect(model, &QAbstractItemModel::rowsRemoved, this, refit, queued);
        connect(model, &QAbstractItemModel::dataChanged, this, refit, queued);
        connect(model, &QAbstractItemModel::modelReset, this, refit, queued);
        connect(model, &QAbstractItemModel::layoutChanged, this, refit, queued);
    }

    void fit() { m_list->setFixedWidth(std::max(m_minimum, listWidthForItems(m_list))); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_list && changesMetrics(event->type()))
            fit();
        return false;
    }

private:
    QListView* m_list;
    int m_minimum;
};

// ---- Elided list items ----

/// Items no wider than the viewport (the list view then gives each one the viewport's width
/// and the style elides the text); an elided text shows in full as the tooltip.
class ElidingDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setWidth(0); // the view stretches items to its viewport width
        return size;
    }

    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
        const QModelIndex& index) override
    {
        if (event->type() == QEvent::ToolTip && !index.data(Qt::ToolTipRole).isValid()) {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            const QStyle* style = view->style();
            const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, view);
            const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, view) + 1;
            if (opt.fontMetrics.horizontalAdvance(opt.text) > textRect.width() - 2 * margin) {
                QToolTip::showText(event->globalPos(), opt.text, view);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
};

// ---- Elided label and button text ----

/// Remembers the full text and re-elides it whenever the widget's width or font changes.
class ElideFilter : public QObject
{
public:
    explicit ElideFilter(QWidget* widget)
        : QObject(widget)
        , m_widget(widget)
    {
        widget->installEventFilter(this);
    }

    void set(const QString& text, const QString& toolTip)
    {
        if (text == m_text && toolTip == m_toolTip)
            return; // the mixer refreshes its strips on every frame while playing
        m_text = text;
        m_toolTip = toolTip;
        apply();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_widget
            && (event->type() == QEvent::Resize || changesMetrics(event->type())))
            apply();
        return false;
    }

private:
    /// Room for text: the label's contents width, or what a button leaves beside its frame,
    /// padding and menu arrow.
    int textWidth() const
    {
        if (auto* button = qobject_cast<QToolButton*>(m_widget)) {
            const int chrome = button->sizeHint().width()
                - button->fontMetrics().horizontalAdvance(button->text());
            return button->width() - chrome;
        }
        auto* label = static_cast<QLabel*>(m_widget);
        return label->contentsRect().width() - 2 * label->margin() - label->indent();
    }

    void apply()
    {
        if (m_applying)
            return;
        m_applying = true;
        const QFontMetrics metrics = m_widget->fontMetrics();
        const int width = std::max(0, textWidth());
        QStringList lines = m_text.split(QLatin1Char('\n'));
        bool elided = false;
        for (QString& line : lines) {
            const QString shown = metrics.elidedText(line, Qt::ElideRight, width);
            elided = elided || shown != line;
            line = shown;
        }
        const QString shown = lines.join(QLatin1Char('\n'));
        if (auto* button = qobject_cast<QToolButton*>(m_widget))
            button->setText(shown);
        else
            static_cast<QLabel*>(m_widget)->setText(shown);
        QString tip = m_toolTip;
        if (elided)
            tip = m_toolTip.isEmpty() ? m_text : m_text + QLatin1Char('\n') + m_toolTip;
        m_widget->setToolTip(tip);
        m_applying = false;
    }

    QWidget* m_widget;
    QString m_text;
    QString m_toolTip;
    bool m_applying = false;
};

/// Names the filter so it can be found again (it has no meta-object of its own to look for).
const QString kElideFilterName = QStringLiteral("kkElideFilter");

ElideFilter* elideFilterOf(QWidget* widget)
{
    if (QObject* filter = widget->findChild<QObject*>(kElideFilterName, Qt::FindDirectChildrenOnly))
        return static_cast<ElideFilter*>(filter);
    auto* filter = new ElideFilter(widget);
    filter->setObjectName(kElideFilterName);
    return filter;
}

} // namespace

int listWidthForItems(const QListView* list)
{
    int width = list->sizeHintForColumn(0) + 2 * list->spacing() + 2 * list->frameWidth();
    const QMargins contents = list->contentsMargins(); // (viewport margins are not used here)
    width += contents.left() + contents.right();
    if (list->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff)
        width += list->verticalScrollBar()->sizeHint().width();
    return width;
}

void fitListWidthToItems(QListView* list, int minimumWidth)
{
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    (new ListWidthFitter(list, minimumWidth))->fit();
}

void elideListItems(QListView* list)
{
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setTextElideMode(Qt::ElideRight);
    list->setItemDelegate(new ElidingDelegate(list));
}

void setElidedText(QLabel* label, const QString& text, const QString& toolTip)
{
    elideFilterOf(label)->set(text, toolTip);
}

void setElidedText(QToolButton* button, const QString& text, const QString& toolTip)
{
    elideFilterOf(button)->set(text, toolTip);
}

} // namespace kk::textfit
