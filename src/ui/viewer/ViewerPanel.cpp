// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER viewer implementation: draws the last frame it was given; no decoding yet.
// TODO(playback): replace with the real viewer driven by PlaybackEngine.
#include "ui/viewer/ViewerPanel.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace cw {

namespace {
constexpr int kTransportIconSize = 18;
constexpr int kTransportBarHeight = 36;
} // namespace

ViewerPanel::ViewerPanel(Mode mode, AppContext& ctx, QWidget* parent)
    : PanelFrame(mode == Mode::Source ? tr("Source") : tr("Timeline"), parent)
    , m_ctx(ctx)
    , m_mode(mode)
{
    m_timecode = new QLabel(timecode::format(0, Rational {}));
    m_timecode->setObjectName(QStringLiteral("TimecodeLabel"));
    m_timecode->setContentsMargins(0, 0, 6, 0);
    addTitleBarWidget(m_timecode);

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_surface = ViewerSurface::create(body);
    layout->addWidget(m_surface->widget(), 1);

    auto* transport = new QWidget(body);
    transport->setFixedHeight(kTransportBarHeight);
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(6, 0, 6, 0);
    transportLayout->addStretch(1);
    m_transportButtons = new QHBoxLayout;
    m_transportButtons->setSpacing(2);
    transportLayout->addLayout(m_transportButtons);
    transportLayout->addStretch(1);
    layout->addWidget(transport);
    setContentWidget(body);

    addTransportButton(
        QStringLiteral("transport.go_to_start"), [this] { emit goToStartRequested(); });
    addTransportButton(QStringLiteral("transport.prev_frame"), [this] { emit stepRequested(-1); });
    addTransportButton(QStringLiteral("transport.shuttle_stop"), [this] { emit stopRequested(); });
    addTransportButton(
        QStringLiteral("transport.play_pause"), [this] { emit playPauseRequested(); });
    addTransportButton(QStringLiteral("transport.next_frame"), [this] { emit stepRequested(1); });
    addTransportButton(QStringLiteral("transport.go_to_end"), [this] { emit goToEndRequested(); });
    addTransportButton(QStringLiteral("transport.loop"), {});

    if (m_mode == Mode::Timeline && ctx.project) {
        Timeline* timeline = ctx.project->timeline();
        auto update = [this, timeline] {
            setPosition(timeline->playhead(), timeline->data().fps);
            m_surface->setAspect(QSize(timeline->data().width, timeline->data().height));
        };
        connect(timeline, &Timeline::playheadChanged, this, update);
        connect(timeline, &Timeline::changed, this, update);
        update();
    }
    clearFrame();
}

void ViewerPanel::addTransportButton(
    const QString& actionId, const std::function<void()>& sourceHandler)
{
    QAction* action = m_ctx.actions ? m_ctx.actions->action(actionId) : nullptr;
    auto* button = new QToolButton;
    button->setAutoRaise(true);
    button->setIconSize(QSize(kTransportIconSize, kTransportIconSize));
    if (m_mode == Mode::Timeline && action) {
        button->setDefaultAction(action);
    } else if (action) {
        // Same look as the global action, but drives the source viewer.
        button->setIcon(action->icon());
        button->setToolTip(action->toolTip());
        button->setCheckable(action->isCheckable());
        if (sourceHandler)
            connect(button, &QToolButton::clicked, this, sourceHandler);
    }
    m_transportButtons->addWidget(button);
}

void ViewerPanel::showFrame(const QImage& frame)
{
    m_surface->setFrame(frame);
}

void ViewerPanel::clearFrame()
{
    m_surface->clear(m_mode == Mode::Source ? tr("No clip loaded") : tr("No picture"));
}

void ViewerPanel::setPosition(int64_t frame, Rational fps)
{
    m_timecode->setText(timecode::format(frame, fps));
}

void ViewerPanel::setSourceMedia(const QString& mediaId)
{
    m_sourceMediaId = mediaId;
    const MediaItem* item
        = m_ctx.project && !mediaId.isEmpty() ? m_ctx.project->mediaPool()->item(mediaId) : nullptr;
    setTitle(item ? tr("Source — %1").arg(item->name) : tr("Source"));
}

QSize ViewerPanel::pictureSize() const
{
    return m_surface->pictureRect().size();
}

ViewerSurface* ViewerPanel::surface() const
{
    return m_surface;
}

} // namespace cw
