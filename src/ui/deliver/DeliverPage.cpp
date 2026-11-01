// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER Deliver page layout.
// TODO(export): render settings form, render queue with progress, mini timeline with in/out.
#include "ui/deliver/DeliverPage.h"

#include "ui/common/PanelFrame.h"
#include "ui/viewer/ViewerPanel.h"

#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace kk {

DeliverPage::DeliverPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_topRow = new QSplitter(Qt::Horizontal);
    m_topRow->setChildrenCollapsible(false);
    m_topRow->addWidget(createPlaceholderPanel(tr("Render Settings"),
        tr("Format, codec, resolution and file location will be chosen here.")));
    m_viewer = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_topRow->addWidget(m_viewer);
    m_topRow->addWidget(createPlaceholderPanel(
        tr("Render Queue"), tr("Queued and finished render jobs will appear here.")));
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({420, 1080, 420});

    m_main = new QSplitter(Qt::Vertical);
    m_main->setChildrenCollapsible(false);
    m_main->addWidget(m_topRow);
    m_main->addWidget(createPlaceholderPanel(
        tr("Timeline"), tr("A compact timeline for choosing the render range.")));
    m_main->setStretchFactor(0, 1);
    m_main->setSizes({760, 200});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);
}

void DeliverPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("main"), m_main->saveState());
}

void DeliverPage::restoreLayout(const QSettings& settings)
{
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
}

} // namespace kk
