// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPage layout and panel toggles.
#include "app/pages/MediaPage.h"

#include "ui/common/AppContext.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/TimelineCommandSuspension.h"
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

    m_topRow = new PanelSplitter(Qt::Horizontal);
    m_topRow->setChildrenCollapsible(false);
    m_topRow->addWidget(m_browser);
    m_topRow->addWidget(m_viewer);
    m_topRow->addWidget(m_metadata);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({420, 1080, 420});

    m_main = new PanelSplitter(Qt::Vertical);
    m_main->setChildrenCollapsible(false);
    m_main->addWidget(m_topRow);
    m_main->addWidget(m_mediaPool);
    m_main->setStretchFactor(0, 1);
    m_main->setSizes({600, 360});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    // The metadata panel follows the media pool selection (one item or batch editing).
    m_metadata->attach(m_mediaPool);
    connect(
        m_mediaPool, &MediaPoolPanel::currentMediaChanged, m_viewer, &ViewerPanel::setSourceMedia);
    // Double-click (or Open in Source Viewer): the clip in the viewer, which takes the
    // transport keys (Space, J / K / L, I / O), as on the Edit page (spec 33.33).
    connect(m_mediaPool, &MediaPoolPanel::openInSourceViewer, this, [this](const QString& id) {
        m_viewer->setSourceMedia(id);
        m_viewer->activate();
        m_viewer->setFocus();
    });

    // No timeline here: the Edit page timeline's commands are off while the page is shown
    // (spec 42b.26). The media pool's own commands in the Clip menu stay; the source viewer
    // takes I / O and the other mark keys itself while it has focus.
    new TimelineCommandSuspension(this, ctx.actions,
        {QStringLiteral("clip.attributes"), QStringLiteral("mediapool.new_multicam"),
            QStringLiteral("mediapool.sync_waveform"), QStringLiteral("mediapool.sync_timecode"),
            QStringLiteral("mediapool.sync_in_points")});
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
