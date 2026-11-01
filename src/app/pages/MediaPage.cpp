// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPage layout and panel toggles.
#include "app/pages/MediaPage.h"

#include "ui/mediapool/MediaBrowserPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MetadataPanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace kk {

MediaPage::MediaPage(AppContext& ctx, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
{
    m_browser = new MediaBrowserPanel(ctx);
    m_viewer = new ViewerPanel(ViewerPanel::Mode::Source, ctx);
    m_metadata = new MetadataPanel(ctx);
    m_mediaPool = new MediaPoolPanel(ctx);

    m_topRow = new QSplitter(Qt::Horizontal);
    m_topRow->setChildrenCollapsible(false);
    m_topRow->setHandleWidth(3);
    m_topRow->addWidget(m_browser);
    m_topRow->addWidget(m_viewer);
    m_topRow->addWidget(m_metadata);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({420, 1080, 420});

    m_main = new QSplitter(Qt::Vertical);
    m_main->setChildrenCollapsible(false);
    m_main->setHandleWidth(3);
    m_main->addWidget(m_topRow);
    m_main->addWidget(m_mediaPool);
    m_main->setStretchFactor(0, 1);
    m_main->setSizes({600, 360});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    connect(
        m_mediaPool, &MediaPoolPanel::currentMediaChanged, m_metadata, &MetadataPanel::showMedia);
    connect(
        m_mediaPool, &MediaPoolPanel::currentMediaChanged, m_viewer, &ViewerPanel::setSourceMedia);
}

QWidget* MediaPage::panel(PanelId id) const
{
    switch (id) {
    case PanelId::MediaPool:
        return m_mediaPool;
    case PanelId::Metadata:
        return m_metadata;
    default:
        return nullptr;
    }
}

bool MediaPage::hasPanel(PanelId id) const
{
    return panel(id) != nullptr;
}

void MediaPage::setPanelVisible(PanelId id, bool visible)
{
    QWidget* w = panel(id);
    if (!w || w->isHidden() == !visible)
        return;
    w->setVisible(visible);
    emit panelVisibilityChanged(id, visible);
}

bool MediaPage::isPanelVisible(PanelId id) const
{
    const QWidget* w = panel(id);
    return w && !w->isHidden();
}

void MediaPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("main"), m_main->saveState());
    for (PanelId id : {PanelId::MediaPool, PanelId::Metadata})
        settings.setValue(QStringLiteral("visible/") + panelIdToString(id), isPanelVisible(id));
}

void MediaPage::restoreLayout(const QSettings& settings)
{
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
    for (PanelId id : {PanelId::MediaPool, PanelId::Metadata}) {
        const QString key = QStringLiteral("visible/") + panelIdToString(id);
        if (settings.contains(key))
            setPanelVisible(id, settings.value(key).toBool());
    }
}

} // namespace kk
