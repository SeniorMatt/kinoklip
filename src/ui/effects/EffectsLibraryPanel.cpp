// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER effects library: a category tree filled from the EffectRegistry.
// TODO(panels): drag & drop to the timeline, search field, favourites.
#include "ui/effects/EffectsLibraryPanel.h"

#include "core/EffectRegistry.h"

#include <QHeaderView>
#include <QTreeWidget>

namespace cw {

EffectsLibraryPanel::EffectsLibraryPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Effects"), parent)
    , m_ctx(ctx)
{
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setFrameShape(QFrame::NoFrame);

    const EffectRegistry& registry = EffectRegistry::instance();
    for (EffectCategory category : {EffectCategory::VideoEffect, EffectCategory::Transition,
             EffectCategory::Title, EffectCategory::Generator, EffectCategory::AudioEffect}) {
        const auto descriptors = registry.byCategory(category);
        if (descriptors.isEmpty())
            continue;
        auto* group = new QTreeWidgetItem(m_tree, {EffectRegistry::categoryName(category)});
        group->setFlags(Qt::ItemIsEnabled);
        for (const EffectDescriptor* d : descriptors) {
            auto* item = new QTreeWidgetItem(group, {d->name});
            item->setData(0, Qt::UserRole, d->id);
            item->setToolTip(0, d->description);
        }
        group->setExpanded(true);
    }
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!id.isEmpty())
            emit effectActivated(id);
    });
    setContentWidget(m_tree);
}

} // namespace cw
