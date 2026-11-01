// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER media browser implementation: QFileSystemModel tree, double-click imports.
// TODO(panels): places sidebar, thumbnails, preview in the viewer on selection.
#include "ui/mediapool/MediaBrowserPanel.h"

#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"

#include <QDir>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QTreeView>

namespace kk {

MediaBrowserPanel::MediaBrowserPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Media Storage"), parent)
    , m_ctx(ctx)
{
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_model->setReadOnly(true);
    // Show only importable media (hide, rather than grey out, everything else).
    m_model->setNameFilters(mediaFileNamePatterns());
    m_model->setNameFilterDisables(false);

    m_view = new QTreeView;
    m_view->setModel(m_model);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setHeaderHidden(true);
    for (int column = 1; column < m_model->columnCount(); ++column)
        m_view->hideColumn(column);
    setContentWidget(m_view);
    setRootPath(QDir::homePath());

    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!m_model->isDir(index))
            importMediaWithFeedback(m_ctx, {m_model->filePath(index)}, this);
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& index) {
            if (index.isValid() && !m_model->isDir(index))
                emit fileSelected(m_model->filePath(index));
        });
}

void MediaBrowserPanel::setRootPath(const QString& path)
{
    m_view->setRootIndex(m_model->setRootPath(path));
}

} // namespace kk
