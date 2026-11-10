// SPDX-License-Identifier: GPL-3.0-or-later
// MixerPanel strip management.
#include "ui/audio/MixerPanel.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>

namespace kk {

MixerPanel::MixerPanel(
    AppContext& ctx, MixerController* controller, MeterModel* meters, QWidget* parent)
    : PanelFrame(tr("Mixer"), parent)
    , m_ctx(ctx)
    , m_controller(controller)
    , m_meters(meters)
{
    setPanelInfo(this, tr("Mixer"),
        tr("A strip for each audio track and each bus, with its level and meter."));
    setPanelKey(QStringLiteral("mixer")); // in-app help: panel:mixer
    setObjectName(QStringLiteral("MixerPanel"));
    if (ctx.actions && ctx.actions->contains(QStringLiteral("audio.add_bus")))
        addTitleBarAction(ctx.actions->action(QStringLiteral("audio.add_bus")));
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_row = new QWidget;
    m_rowLayout = new QHBoxLayout(m_row);
    m_rowLayout->setContentsMargins(0, 0, 0, 0);
    m_rowLayout->setSpacing(1);
    m_rowLayout->addStretch(1);
    m_scroll->setWidget(m_row);
    setContentWidget(m_scroll);

    connect(m_controller, &MixerController::mixChanged, this, &MixerPanel::sync);
    connect(
        m_ctx.project->timeline(), &Timeline::playheadChanged, this, &MixerPanel::refreshValues);
    sync();
}

MixerStrip* MixerPanel::strip(const MixTarget& target) const
{
    for (MixerStrip* s : m_strips) {
        if (s->target() == target)
            return s;
    }
    return nullptr;
}

void MixerPanel::sync()
{
    const TimelineData& d = m_controller->data();
    QVector<MixTarget> targets;
    for (const TrackData& t : d.audioTracks)
        targets.append(MixTarget::track(t.id));
    for (const BusData& b : d.mixer.buses)
        targets.append(MixTarget::bus(b.id));
    if (targets != m_targets) {
        // Rebuild (rare: tracks / buses added, removed or reordered).
        for (MixerStrip* s : std::as_const(m_strips))
            s->deleteLater();
        m_strips.clear();
        while (QLayoutItem* item = m_rowLayout->takeAt(0)) {
            if (QWidget* w = item->widget(); w && !qobject_cast<MixerStrip*>(w))
                w->deleteLater(); // separators
            delete item;
        }
        bool busesStarted = false;
        for (const MixTarget& t : targets) {
            if (t.isBus() && !busesStarted) {
                busesStarted = true;
                auto* line = new QFrame;
                line->setFrameShape(QFrame::VLine);
                line->setFrameShadow(QFrame::Sunken);
                m_rowLayout->addWidget(line);
            }
            auto* strip = new MixerStrip(m_controller, m_meters, t);
            m_rowLayout->addWidget(strip);
            m_strips.append(strip);
        }
        m_rowLayout->addStretch(1);
        m_targets = targets;
    }
    refreshValues();
}

void MixerPanel::refreshValues()
{
    const int64_t frame = m_ctx.project->timeline()->playhead();
    for (MixerStrip* s : std::as_const(m_strips))
        s->refresh(frame);
}

} // namespace kk
