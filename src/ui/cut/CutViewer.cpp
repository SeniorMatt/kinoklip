// SPDX-License-Identifier: GPL-3.0-or-later
// CutViewer implementation: mode stack, source tape building and edit material.
#include "ui/cut/CutViewer.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "playback/SourcePlayer.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/cut/SourceTapeBar.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace kk {

namespace {

/// Registry action of each mode, in header order.
constexpr std::pair<CutViewer::Mode, const char*> kModeActions[] = {
    {CutViewer::Mode::SourceClip, "cut.viewer_source_clip"},
    {CutViewer::Mode::SourceTape, "cut.viewer_source_tape"},
    {CutViewer::Mode::Timeline, "cut.viewer_timeline"},
};

/// Prefix of the source tape's pseudo media item id (never a media pool id).
constexpr char kTapeIdPrefix[] = "kk-source-tape:";

QAction* registryAction(const AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        ? ctx.actions->action(QLatin1String(id))
        : nullptr;
}

} // namespace

CutViewer::CutViewer(AppContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("CutViewer"));
    m_clip = new ViewerPanel(ViewerPanel::Mode::Source, ctx);
    m_clip->setObjectName(QStringLiteral("CutSourceClipViewer"));
    m_clip->setMulticamAllowed(false);
    m_tape = new ViewerPanel(ViewerPanel::Mode::Source, ctx);
    m_tape->setObjectName(QStringLiteral("CutSourceTapeViewer"));
    m_tape->setMulticamAllowed(false);
    m_timeline = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
    m_timeline->setObjectName(QStringLiteral("CutTimelineViewer"));

    m_tapePage = new QWidget;
    auto* tapeLayout = new QVBoxLayout(m_tapePage);
    tapeLayout->setContentsMargins(0, 0, 0, 0);
    tapeLayout->setSpacing(0);
    tapeLayout->addWidget(m_tape, 1);
    m_tapeBar = new SourceTapeBar(m_tapePage);
    tapeLayout->addWidget(m_tapeBar);
    connect(m_tapeBar, &SourceTapeBar::positionRequested, this, [this](int64_t frame) {
        if (SourcePlayer* player = m_tape->sourcePlayer())
            player->engine()->seek(frame);
    });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_clip);
    m_stack->addWidget(m_tapePage);
    m_stack->addWidget(m_timeline);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    for (ViewerPanel* viewer : {m_clip, m_tape, m_timeline})
        addModeButtons(viewer);
    for (const auto& [mode, id] : kModeActions) {
        if (QAction* a = registryAction(ctx, id)) {
            const Mode m = mode;
            connect(a, &QAction::triggered, this, [this, m] {
                if (isVisible())
                    setMode(m);
            });
        }
    }
    // A clip opened elsewhere (match frame, media pool double-click) shows in Source Clip.
    connect(m_clip, &ViewerPanel::sourceMediaChanged, this, [this](const QString& id) {
        if (!id.isEmpty() && isVisible() && m_mode == Mode::SourceClip)
            m_clip->activate();
    });
    m_stack->setCurrentWidget(m_timeline);
}

void CutViewer::addModeButtons(ViewerPanel* viewer)
{
    for (const auto& [mode, id] : kModeActions) {
        QAction* a = registryAction(m_ctx, id);
        if (!a)
            continue;
        auto* button = new QToolButton;
        button->setObjectName(QStringLiteral("CutViewerMode_") + QLatin1String(id));
        button->setDefaultAction(a);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
        viewer->addTitleBarLeadingWidget(button);
    }
}

ViewerPanel* CutViewer::currentViewer() const
{
    switch (m_mode) {
    case Mode::SourceClip:
        return m_clip;
    case Mode::SourceTape:
        return m_tape;
    case Mode::Timeline:
        break;
    }
    return m_timeline;
}

void CutViewer::setMode(Mode mode)
{
    const bool changed = mode != m_mode;
    m_mode = mode;
    if (mode != Mode::Timeline)
        m_lastSource = mode;
    if (mode == Mode::SourceTape && m_tapeDirty)
        rebuildTape();
    switch (mode) {
    case Mode::SourceClip:
        m_stack->setCurrentWidget(m_clip);
        break;
    case Mode::SourceTape:
        m_stack->setCurrentWidget(m_tapePage);
        break;
    case Mode::Timeline:
        m_stack->setCurrentWidget(m_timeline);
        break;
    }
    for (const auto& [m, id] : kModeActions) {
        if (QAction* a = registryAction(m_ctx, id); a && m == mode)
            a->setChecked(true);
    }
    // The shown viewer takes the transport keys (Space, J/K/L, I/O with focus in it).
    if (isVisible())
        currentViewer()->activate();
    if (changed)
        emit modeChanged(mode);
}

// ---- Source tape ----

void CutViewer::setTapeItems(
    const QStringList& mediaIds, const QString& binKey, const QString& binName)
{
    if (mediaIds == m_tapeIds && binKey == m_tapeKey && binName == m_tapeName && !m_tapeDirty)
        return;
    m_tapeIds = mediaIds;
    m_tapeKey = binKey;
    m_tapeName = binName;
    m_tapeDirty = true;
    if (m_mode == Mode::SourceTape)
        rebuildTape();
}

void CutViewer::rebuildTape()
{
    m_tapeDirty = false;
    Project* project = m_ctx.project;
    const TimelineData& program = project->timeline()->data();
    QVector<const MediaItem*> items;
    for (const QString& id : std::as_const(m_tapeIds))
        items.append(project->mediaPool()->item(id));
    m_sourceTape = SourceTape::build(
        items, program.fps, TimelineController::defaultStillFrames(program.fps));

    // One pseudo item per bin: the player remembers the tape position and marks per bin.
    MediaItem tapeItem;
    tapeItem.id = QLatin1String(kTapeIdPrefix) + m_tapeKey;
    tapeItem.name
        = m_tapeName.isEmpty() ? tr("Source Tape") : tr("Source Tape: %1").arg(m_tapeName);
    tapeItem.info.valid = true;
    tapeItem.info.hasVideo = true;
    tapeItem.info.hasAudio = true;
    tapeItem.info.frameRate = m_sourceTape.fps();
    tapeItem.info.durationUs = framesToUs(m_sourceTape.duration(), m_sourceTape.fps());
    const bool hadPlayer = m_tape->sourcePlayer() != nullptr;
    m_tape->setSourceContent(
        tapeItem, m_sourceTape.toTimeline(tapeItem.name, program.width, program.height));
    m_tapeBar->setTape(m_sourceTape);
    if (!hadPlayer && m_tape->sourcePlayer()) {
        SourcePlayer* player = m_tape->sourcePlayer();
        connect(player->engine(), &PlaybackEngine::positionChanged, m_tapeBar,
            &SourceTapeBar::setPosition);
        connect(player, &SourcePlayer::marksChanged, this, &CutViewer::syncTapeBar);
        connect(player, &SourcePlayer::mediaChanged, this, &CutViewer::syncTapeBar);
    }
    syncTapeBar();
}

void CutViewer::syncTapeBar()
{
    const SourcePlayer* player = m_tape->sourcePlayer();
    if (!player || !player->hasMedia()) {
        m_tapeBar->setMarks(-1, -1);
        m_tapeBar->setPosition(0);
        return;
    }
    m_tapeBar->setMarks(player->markIn(), player->markOut());
    m_tapeBar->setPosition(player->engine()->position());
}

bool CutViewer::showTapeClip(const QString& mediaId)
{
    if (m_tapeDirty)
        rebuildTape();
    const int index = m_sourceTape.indexOf(mediaId);
    SourcePlayer* player = m_tape->sourcePlayer();
    if (index < 0 || !player)
        return false;
    player->engine()->seek(m_sourceTape.entries()[size_t(index)].start);
    return true;
}

QString CutViewer::tapeClipAtPosition() const
{
    const SourcePlayer* player = m_tape->sourcePlayer();
    if (!player || !player->hasMedia())
        return {};
    const int index = m_sourceTape.entryAt(player->engine()->position());
    return index >= 0 ? m_sourceTape.entries()[size_t(index)].mediaId : QString();
}

// ---- Edit material ----

QVector<SourceSelection> CutViewer::sourceMaterial() const
{
    return sourceMaterial(m_mode);
}

QVector<SourceSelection> CutViewer::sourceMaterial(Mode mode) const
{
    QVector<SourceSelection> out;
    if (mode == Mode::SourceClip) {
        if (const std::optional<SourceRange> r = m_clip->sourceSelection())
            out.append(SourceSelection {r->mediaId, r->inUs, r->outUs});
        return out;
    }
    if (mode != Mode::SourceTape)
        return out;
    const SourcePlayer* player = m_tape->sourcePlayer();
    if (!player || !player->hasMedia() || m_sourceTape.isEmpty())
        return out;
    const auto& entries = m_sourceTape.entries();
    const int64_t markIn = player->markIn();
    const int64_t markOut = player->markOut();
    int64_t in = 0;
    int64_t endFrame = 0;
    if (markIn >= 0 && markOut > markIn) {
        in = markIn;
        endFrame = markOut;
    } else if (markIn >= 0) {
        const int e = m_sourceTape.entryAt(markIn);
        if (e < 0)
            return out;
        in = markIn;
        endFrame = entries[size_t(e)].end();
    } else if (markOut > 0) {
        const int e = m_sourceTape.entryAt(markOut - 1);
        if (e < 0)
            return out;
        in = entries[size_t(e)].start;
        endFrame = markOut;
    } else {
        const int e = m_sourceTape.entryAt(player->engine()->position());
        if (e < 0)
            return out;
        in = entries[size_t(e)].start;
        endFrame = entries[size_t(e)].end();
    }
    for (const SourceTapeSegment& s : m_sourceTape.segments(in, endFrame))
        out.append(SourceSelection {s.mediaId, s.inUs, s.outUs});
    return out;
}

} // namespace kk
