// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerPanel, multicam part: the source viewer's Multicam mode. The angle grid is a private
// timeline (multicam::gridTimeline()) played by the source player, so every angle decodes,
// plays and scrubs like any nested timeline; MulticamOverlay adds names, the active angle
// frames and the clicks.
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderTypes.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/viewer/MulticamOverlay.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QComboBox>

namespace kk {

namespace {

QAction* registryAction(const AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        ? ctx.actions->action(QLatin1String(id))
        : nullptr;
}

/// Angle (0-based) a set of multicam parts shows at @p frame: the first part covering it.
int angleAt(const TimelineData& data, const QVector<quint64>& ids, int64_t frame)
{
    for (const quint64 id : ids) {
        const ClipData* c = editops::clipById(data, id);
        if (c && c->contains(frame))
            return c->multicamAngle;
    }
    return -1;
}

} // namespace

void ViewerPanel::setupMulticam()
{
    if (m_mode != Mode::Source || !m_ctx.project)
        return;
    m_multicamOverlay = new MulticamOverlay(m_surface);
    connect(
        m_multicamOverlay, &MulticamOverlay::angleClicked, this, &ViewerPanel::chooseMulticamAngle);
    connect(m_multicamOverlay, &MulticamOverlay::pageRequested, this,
        [this](int delta) { setMulticamPage(m_multicamPage + delta); });

    // Header: Source / Multicam (Resolve's source viewer mode menu) and the grid size.
    m_modeBox = new QComboBox;
    m_modeBox->setObjectName(QStringLiteral("ViewerSourceMode"));
    m_modeBox->setFocusPolicy(Qt::NoFocus);
    m_modeBox->addItems({tr("Source"), tr("Multicam")});
    m_modeBox->setToolTip(tr("Source viewer mode"));
    connect(m_modeBox, &QComboBox::currentIndexChanged, this,
        [this](int index) { setMulticamMode(index == 1); });
    addTitleBarLeadingWidget(m_modeBox);
    m_gridBox = new QComboBox;
    m_gridBox->setObjectName(QStringLiteral("ViewerMulticamGrid"));
    m_gridBox->setFocusPolicy(Qt::NoFocus);
    m_gridBox->addItem(tr("Auto"), 0);
    m_gridBox->addItem(QStringLiteral("2x2"), 2);
    m_gridBox->addItem(QStringLiteral("3x3"), 3);
    m_gridBox->addItem(QStringLiteral("4x4"), 4);
    m_gridBox->setToolTip(tr("Angles per page"));
    m_gridBox->setVisible(false);
    connect(m_gridBox, &QComboBox::currentIndexChanged, this,
        [this](int index) { setMulticamColumns(m_gridBox->itemData(index).toInt()); });
    addTitleBarLeadingWidget(m_gridBox);

    if (QAction* view = registryAction(m_ctx, "view.multicam_viewer"))
        connect(view, &QAction::toggled, this, &ViewerPanel::setMulticamMode);
    // Number keys and page actions: only the visible source viewer in multicam mode acts.
    for (int n = 1; n <= 9; ++n) {
        const QByteArray id = "multicam.angle_" + QByteArray::number(n);
        if (QAction* a = registryAction(m_ctx, id.constData())) {
            connect(a, &QAction::triggered, this, [this, n] {
                if (m_multicam && isVisible())
                    chooseMulticamAngle(n - 1 + m_multicamPage * m_mc.columns * m_mc.columns);
            });
        }
    }
    for (const auto& [id, delta] :
        {std::pair {"multicam.prev_page", -1}, {"multicam.next_page", 1}}) {
        if (QAction* a = registryAction(m_ctx, id)) {
            const int d = delta;
            connect(a, &QAction::triggered, this, [this, d] {
                if (m_multicam && isVisible())
                    setMulticamPage(m_multicamPage + d);
            });
        }
    }

    // Follow the program timeline: edits (cuts change the active angle), the playhead (also
    // while playing), tab switches, edited multicam clips.
    Project* project = m_ctx.project;
    Timeline* program = project->timeline();
    connect(program, &Timeline::changed, this, [this] {
        if (m_multicam)
            updateMulticam();
    });
    connect(program, &Timeline::playheadChanged, this, [this] {
        // Moving the timeline playhead from elsewhere hands the grid back to the timeline.
        if (activeViewer() != this)
            m_multicamPrefersSource = false;
        if (m_multicam)
            updateMulticam();
    });
    connect(project, &Project::timelinesChanged, this, [this] {
        if (m_multicam)
            updateMulticam();
    });
    connect(project, &Project::projectReset, this, [this] {
        m_mc = {};
        if (m_multicam)
            updateMulticam();
    });
}

void ViewerPanel::setMulticamMode(bool on)
{
    if (m_mode != Mode::Source || !m_multicamOverlay || on == m_multicam) {
        if (m_modeBox && m_modeBox->currentIndex() != (m_multicam ? 1 : 0))
            m_modeBox->setCurrentIndex(m_multicam ? 1 : 0);
        return;
    }
    m_multicam = on;
    m_modeBox->setCurrentIndex(on ? 1 : 0);
    m_gridBox->setVisible(on);
    if (QAction* view = registryAction(m_ctx, "view.multicam_viewer");
        view && view->isChecked() != on)
        view->setChecked(on);
    if (on) {
        updateMulticam();
    } else {
        m_multicamOverlay->hide();
        leaveMulticamContent();
    }
    updateTitle();
}

void ViewerPanel::setMulticamColumns(int columns)
{
    m_multicamColumns = columns == 0 ? 0 : std::clamp(columns, 2, 4);
    if (m_gridBox && m_gridBox->currentData().toInt() != m_multicamColumns)
        m_gridBox->setCurrentIndex(std::max(0, m_gridBox->findData(m_multicamColumns)));
    if (m_multicam)
        updateMulticam();
}

void ViewerPanel::setMulticamPage(int page)
{
    m_multicamPage = std::max(0, page);
    if (m_multicam)
        updateMulticam(); // clamps the page to the angle count
}

void ViewerPanel::chooseMulticamAngle(int angle)
{
    if (!m_multicam || m_mc.uid == 0 || angle < 0)
        return;
    if (m_mc.followClip != 0) {
        emit multicamAngleChosen(angle);
        return;
    }
    // The loaded multicam clip on its own: the choice sets what this viewer plays.
    const bool video = !registryAction(m_ctx, "multicam.target_audio")
        || !registryAction(m_ctx, "multicam.target_audio")->isChecked();
    const bool audio = !registryAction(m_ctx, "multicam.target_video")
        || !registryAction(m_ctx, "multicam.target_video")->isChecked();
    if (video)
        m_localVideoAngle = angle;
    if (audio)
        m_localAudioAngle = angle;
    updateMulticam();
}

void ViewerPanel::leaveMulticamContent()
{
    if (m_mc.uid == 0)
        return;
    m_mc = {};
    // Back to the plain source clip (a multicam item shows nothing in Source mode yet).
    const QString id = m_sourceMediaId;
    m_sourceMediaId.clear();
    if (m_player)
        m_player->setMedia(nullptr);
    const MediaItem* item = m_ctx.project->mediaPool()->item(id);
    if (item && !item->isTimeline())
        setSourceMedia(id);
    else
        m_sourceMediaId = id;
    if (!m_player || !m_player->hasMedia())
        clearFrame();
}

void ViewerPanel::updateMulticam()
{
    if (!m_multicam || !m_multicamOverlay)
        return;
    Project* project = m_ctx.project;
    const auto lookup = project->timelineLookup();
    const TimelineData& program = project->timeline()->data();
    const int64_t playhead = project->timeline()->playhead();

    // 1. What to show: the multicam clip under the timeline playhead, else the loaded one.
    MulticamShown next;
    int videoAngle = -1;
    int64_t followFrame = -1;
    const MediaItem* loaded = project->mediaPool()->item(m_sourceMediaId);
    const bool loadedIsMulticam = loaded && loaded->kind == MediaItemKind::MulticamClip;
    const quint64 under = (m_multicamPrefersSource && loadedIsMulticam)
        ? 0
        : multicam::multicamClipAt(program, playhead, lookup);
    if (under != 0) {
        const ClipData& clip = *editops::clipById(program, under);
        const multicam::MulticamParts parts = multicam::parts(program, under);
        next.uid = clip.nestedUid;
        next.followClip = under;
        next.itemId = project->timelineItemId(clip.nestedUid);
        videoAngle = angleAt(program, parts.video, playhead);
        next.audioAngle = angleAt(program, parts.audio, playhead);
        if (const TimelineData* mc = lookup(clip.nestedUid))
            followFrame = nestedFrameAt(clip, playhead, program.fps, mc->fps);
    } else if (loadedIsMulticam) {
        next.uid = loaded->timelineUid;
        next.itemId = loaded->id;
        videoAngle = m_localVideoAngle;
        next.audioAngle = m_localAudioAngle;
    }
    next.data = next.uid ? project->timelineData(next.uid) : nullptr;
    const MediaItem* item = project->mediaPool()->item(next.itemId);
    if (!next.data || !item) {
        if (m_mc.uid != 0 && m_player)
            m_player->setMedia(nullptr);
        m_mc = {};
        m_surface->clear(QString());
        m_multicamOverlay->setGrid({}, 2, 0);
        m_multicamOverlay->setMessage(
            tr("No multicam clip at the timeline playhead.\nOpen a multicam clip from the media "
               "pool or move the playhead over one."));
        m_multicamOverlay->refresh();
        m_multicamOverlay->show();
        return;
    }
    const int angles = multicam::angleCount(*next.data);
    next.columns = m_multicamColumns ? m_multicamColumns : multicam::columnsFor(angles);
    const int perPage = next.columns * next.columns;
    const int pages = std::max(1, (angles + perPage - 1) / perPage);
    m_multicamPage = std::clamp(m_multicamPage, 0, pages - 1);
    next.page = m_multicamPage;

    // 2. (Re)build the grid when anything it is made of changed. The player keeps position
    //    and transport for the same item.
    if (next.uid != m_mc.uid || next.itemId != m_mc.itemId || next.data != m_mc.data
        || next.columns != m_mc.columns || next.page != m_mc.page
        || next.audioAngle != m_mc.audioAngle) {
        ensurePlayer();
        TimelineData grid = multicam::gridTimeline(
            *next.data, next.columns, next.page, next.audioAngle, next.itemId);
        m_player->setContent(*item, std::move(grid),
            makeMediaPathMap(project->mediaPool()->state()),
            [project](const TimelineData& d) { return project->nestedTimelinesFor(d); });
        m_surface->setOverlayText(QString());
    }
    m_mc = next;

    // 3. Chrome and position.
    QStringList names;
    for (int a = 0; a < angles; ++a)
        names.append(multicam::angleName(*next.data, a));
    m_multicamOverlay->setMessage(QString());
    m_multicamOverlay->setGrid(names, next.columns, next.page);
    m_multicamOverlay->setActive(videoAngle, next.audioAngle);
    m_multicamOverlay->refresh();
    m_multicamOverlay->show();
    m_multicamOverlay->raise();
    if (followFrame >= 0 && m_player) {
        PlaybackEngine* e = m_player->engine();
        // Following the program: the grid shows the frame under the timeline playhead (paused
        // engine, coalesced still renders, so it keeps up with playback at the write-back rate).
        if (e->rate() == 0.0 && e->position() != followFrame)
            e->seek(followFrame);
    }
    updateTitle();
}

} // namespace kk
