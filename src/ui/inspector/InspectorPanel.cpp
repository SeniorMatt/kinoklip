// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER inspector implementation: tabs + empty-state text, follows the selection.
// TODO(panels): build parameter editors from EffectRegistry descriptors.
#include "ui/inspector/InspectorPanel.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/AppContext.h"

#include <QTabWidget>

namespace kk {

InspectorPanel::InspectorPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Inspector"), parent)
    , m_ctx(ctx)
{
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    const QString hint = tr("Select a clip on the timeline to see and animate its properties.");
    for (const QString& tab :
        {tr("Video"), tr("Audio"), tr("Effects"), tr("Title"), tr("Transition")}) {
        m_tabs->addTab(createPlaceholderContent(hint), tab);
    }
    setContentWidget(m_tabs);
    if (ctx.selection) {
        connect(ctx.selection, &SelectionModel::changed, this,
            [this] { showClip(m_ctx.selection->currentClip()); });
    }
}

void InspectorPanel::showClip(quint64 clipId)
{
    m_clipId = clipId;
    const ClipData* clip
        = clipId ? editops::clipById(m_ctx.project->timeline()->data(), clipId) : nullptr;
    setTitle(clip ? tr("Inspector — %1").arg(clip->name) : tr("Inspector"));
}

} // namespace kk
