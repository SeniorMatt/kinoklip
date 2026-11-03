// SPDX-License-Identifier: GPL-3.0-or-later
// NodeLibraryPanel and SelectNodePopup implementation.
#include "ui/comp/NodeLibrary.h"

#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

constexpr int kTypeRole = Qt::UserRole + 1;
const QSize kPopupSize(280, 340);

/// The library tree: items drag as their node type.
class NodeTree final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QLatin1String(mime::CompNodeType)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty() || items.first()->data(0, kTypeRole).toString().isEmpty())
            return nullptr;
        auto* data = new QMimeData;
        data->setData(QLatin1String(mime::CompNodeType),
            items.first()->data(0, kTypeRole).toString().toUtf8());
        return data;
    }
};

} // namespace

// ---- NodeLibraryPanel ----

NodeLibraryPanel::NodeLibraryPanel(CompSession* session, QWidget* parent)
    : PanelFrame(tr("Nodes"), parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("NodeLibraryPanel"));
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, 0);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search nodes"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_tree = new NodeTree;
    m_tree->setObjectName(QStringLiteral("NodeLibraryTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_tree, 1);
    setContentWidget(body);

    for (const QString& category : compadapter::categories()) {
        auto* folder = new QTreeWidgetItem(m_tree, {category});
        folder->setFlags(Qt::ItemIsEnabled);
        for (const compadapter::NodeTypeInfo& info : compadapter::nodeTypes()) {
            if (info.category != category || info.singleton)
                continue;
            auto* item = new QTreeWidgetItem(folder, {info.name});
            item->setData(0, kTypeRole, info.type);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        folder->setExpanded(category != compadapter::categories().last());
    }
    connect(m_search, &QLineEdit::textChanged, this, &NodeLibraryPanel::setFilter);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString type = item->data(0, kTypeRole).toString();
        if (m_session && !type.isEmpty())
            m_session->addNode(type);
    });
}

void NodeLibraryPanel::setFilter(const QString& text)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* folder = m_tree->topLevelItem(i);
        int shown = 0;
        for (int j = 0; j < folder->childCount(); ++j) {
            QTreeWidgetItem* item = folder->child(j);
            const bool match = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
            item->setHidden(!match);
            shown += match ? 1 : 0;
        }
        folder->setHidden(shown == 0);
        if (!text.isEmpty())
            folder->setExpanded(true);
    }
}

// ---- SelectNodePopup ----

SelectNodePopup::SelectNodePopup(CompSession* session, QWidget* parent)
    : QFrame(parent, Qt::Popup)
    , m_session(session)
{
    setObjectName(QStringLiteral("SelectNodePopup"));
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("SelectNodeSearch"));
    m_search->setPlaceholderText(tr("Select Node"));
    layout->addWidget(m_search);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("SelectNodeList"));
    layout->addWidget(m_list, 1);
    resize(kPopupSize);
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, &SelectNodePopup::fill);
    connect(m_search, &QLineEdit::returnPressed, this, [this] { accept(); });
    connect(m_list, &QListWidget::itemActivated, this, [this] { accept(); });
    connect(m_list, &QListWidget::itemClicked, this, [this] { accept(); });
    fill({});
}

QStringList SelectNodePopup::matches(const QString& text)
{
    QStringList starts, contains;
    for (const compadapter::NodeTypeInfo& info : compadapter::nodeTypes()) {
        if (info.singleton)
            continue;
        if (text.isEmpty() || info.name.startsWith(text, Qt::CaseInsensitive))
            starts.append(info.type);
        else if (info.name.contains(text, Qt::CaseInsensitive)
            || info.type.contains(text, Qt::CaseInsensitive))
            contains.append(info.type);
    }
    return starts + contains;
}

void SelectNodePopup::fill(const QString& text)
{
    m_list->clear();
    for (const QString& type : matches(text)) {
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(type);
        auto* item = new QListWidgetItem(
            info->category.isEmpty() ? info->name : tr("%1   (%2)").arg(info->name, info->category),
            m_list);
        item->setData(kTypeRole, type);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

QPoint SelectNodePopup::placement(const QPoint& globalPos, const QSize& size, const QRect& bounds)
{
    // Centred under the cursor like before, then pushed back inside like a menu (the left /
    // top edge wins when the popup is larger than the bounds).
    QPoint at = globalPos - QPoint(size.width() / 2, metrics::kSpaceXl);
    if (bounds.isEmpty())
        return at;
    at.setX(std::max(bounds.left(), std::min(at.x(), bounds.right() + 1 - size.width())));
    at.setY(std::max(bounds.top(), std::min(at.y(), bounds.bottom() + 1 - size.height())));
    return at;
}

QRect SelectNodePopup::popupBounds(const QPoint& globalPos) const
{
    // The window (where the user works) within the screen under the point, as menus stay on
    // the screen.
    QRect bounds;
    if (QScreen* screen = QGuiApplication::screenAt(globalPos))
        bounds = screen->availableGeometry();
    else if (QScreen* fallback = this->screen())
        bounds = fallback->availableGeometry();
    if (const QWidget* w = parentWidget() ? parentWidget()->window() : nullptr) {
        const QRect window(w->mapToGlobal(QPoint(0, 0)), w->size());
        const QRect both = bounds.isEmpty() ? window : bounds.intersected(window);
        if (!both.isEmpty())
            bounds = both;
    }
    return bounds;
}

void SelectNodePopup::popup(const QPoint& globalPos)
{
    m_search->clear();
    fill({});
    const QRect bounds = popupBounds(globalPos);
    move(placement(globalPos, size(), bounds));
    show();
    // move() places the frame; its size is known once shown (window manager decorations).
    if (frameGeometry().size() != size())
        move(placement(globalPos, frameGeometry().size(), bounds));
    m_search->setFocus();
}

quint64 SelectNodePopup::accept()
{
    QListWidgetItem* item = m_list->currentItem();
    const QString type = item ? item->data(kTypeRole).toString() : QString();
    hide();
    if (!m_session || type.isEmpty())
        return 0;
    return m_session->addNode(type);
}

bool SelectNodePopup::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Up / Down move in the list while typing.
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int row = std::clamp(m_list->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1),
                0, std::max(0, m_list->count() - 1));
            m_list->setCurrentRow(row);
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            hide();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace kk
