// SPDX-License-Identifier: GPL-3.0-or-later
// GuideDialog implementation.
#include "ui/help/GuideDialog.h"

#include "ui/common/Metrics.h"
#include "ui/help/Guide.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kIdRole = Qt::UserRole; ///< article id; empty for the start page
constexpr int kStartRole = Qt::UserRole + 1; ///< true on the start page row
} // namespace

GuideDialog::GuideDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("GuideDialog"));
    setWindowTitle(tr("Guide"));
    resize(440, 420);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search the guide"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("GuideArticles"));
    m_list->setUniformItemSizes(true);
    m_list->installEventFilter(this);

    auto* hint = new QLabel(tr("Articles open in your web browser."), this);
    hint->setObjectName(QStringLiteral("PlaceholderText"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL);
    layout->setSpacing(metrics::kSpaceM);
    layout->addWidget(m_search);
    layout->addWidget(m_list, 1);
    layout->addWidget(hint);

    populate();
    connect(m_search, &QLineEdit::textChanged, this, &GuideDialog::setFilterText);
    connect(m_list, &QListWidget::itemActivated, this, &GuideDialog::open);
    m_search->setFocus();
}

void GuideDialog::populate()
{
    auto* start = new QListWidgetItem(tr("Start page"), m_list);
    start->setData(kStartRole, true);
    start->setToolTip(tr("The guide's contents"));
    QFont bold = start->font();
    bold.setBold(true);
    start->setFont(bold);
    for (const GuideArticle& a : guide::index().articles()) {
        auto* item = new QListWidgetItem(a.title, m_list);
        item->setData(kIdRole, a.id);
        item->setToolTip(a.summary);
    }
    m_list->setCurrentRow(0);
}

void GuideDialog::setFilterText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text); // re-enters through textChanged
    QStringList matches;
    for (const GuideArticle* a : guide::index().search(text))
        matches.append(a->id);
    const bool searching = !text.trimmed().isEmpty();
    QListWidgetItem* firstMatch = nullptr;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        if (item->data(kStartRole).toBool())
            continue; // always offered
        const bool shown = matches.contains(item->data(kIdRole).toString());
        item->setHidden(!shown);
        if (shown && !firstMatch)
            firstMatch = item;
    }
    // While searching, Enter opens the best match rather than the start page.
    m_list->setCurrentItem(searching && firstMatch ? firstMatch : m_list->item(0));
}

QStringList GuideDialog::visibleArticleIds() const
{
    QStringList ids;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        if (!item->isHidden())
            ids.append(item->data(kIdRole).toString());
    }
    return ids;
}

void GuideDialog::openCurrent()
{
    QListWidgetItem* item = m_list->currentItem();
    if (!item || item->isHidden())
        item = m_list->item(0);
    open(item);
}

void GuideDialog::open(QListWidgetItem* item)
{
    if (!item)
        return;
    const QString id = item->data(kIdRole).toString();
    guide::openArticle(id.isEmpty() ? nullptr : guide::index().article(id));
    accept();
}

bool GuideDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            openCurrent();
            return true;
        }
        // Up / Down in the search field move through the list, as in a launcher.
        if (watched == m_search && (key == Qt::Key_Down || key == Qt::Key_Up)) {
            int row = m_list->currentRow();
            do {
                row += key == Qt::Key_Down ? 1 : -1;
            } while (row >= 0 && row < m_list->count() && m_list->item(row)->isHidden());
            if (row >= 0 && row < m_list->count())
                m_list->setCurrentRow(row);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace kk
