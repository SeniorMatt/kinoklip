// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER media pool implementation: QListWidget rebuilt on every pool change.
// TODO(panels): bins tree, thumbnails, sorting, drag to timeline.
#include "ui/mediapool/MediaPoolPanel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QListWidget>
#include <QMimeData>
#include <QStackedWidget>

namespace cw {

namespace {
constexpr int kItemIconSize = 48;

QString iconNameFor(const MediaItem& item)
{
    if (item.info.isStillImage)
        return QStringLiteral("image");
    if (item.info.hasVideo)
        return QStringLiteral("film");
    if (item.info.hasAudio)
        return QStringLiteral("audio_file");
    return QStringLiteral("film");
}
} // namespace

MediaPoolPanel::MediaPoolPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Media Pool"), parent)
    , m_ctx(ctx)
{
    setAcceptDrops(true);
    if (ctx.actions)
        addTitleBarAction(ctx.actions->action(QStringLiteral("file.import_media")));

    m_list = new QListWidget;
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kItemIconSize, kItemIconSize));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSpacing(8);
    m_list->setWordWrap(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAcceptDrops(false); // drops are handled by the panel

    m_stack = new QStackedWidget;
    m_stack->addWidget(
        createPlaceholderContent(tr("No media yet.\nImport files with Ctrl+I or drop them here.")));
    m_stack->addWidget(m_list);
    setContentWidget(m_stack);

    connect(m_list, &QListWidget::currentItemChanged, this,
        [this] { emit currentMediaChanged(currentMediaId()); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit mediaActivated(item->data(Qt::UserRole).toString());
    });
    connect(ctx.project->mediaPool(), &MediaPool::changed, this, &MediaPoolPanel::rebuild);
    rebuild();
}

void MediaPoolPanel::rebuild()
{
    const QString current = currentMediaId();
    m_list->clear();
    for (const MediaItem& item : m_ctx.project->mediaPool()->items()) {
        auto* row = new QListWidgetItem(icon(iconNameFor(item)), item.name, m_list);
        row->setData(Qt::UserRole, item.id);
        row->setToolTip(item.path);
        if (item.id == current)
            m_list->setCurrentItem(row);
    }
    m_stack->setCurrentIndex(m_list->count() > 0 ? 1 : 0);
}

QStringList MediaPoolPanel::selectedMediaIds() const
{
    QStringList ids;
    for (const QListWidgetItem* item : m_list->selectedItems())
        ids.append(item->data(Qt::UserRole).toString());
    return ids;
}

QString MediaPoolPanel::currentMediaId() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void MediaPoolPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MediaPoolPanel::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    event->acceptProposedAction();
    importMediaWithFeedback(m_ctx, paths, this);
}

} // namespace cw
