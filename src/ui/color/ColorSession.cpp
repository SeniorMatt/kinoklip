// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSession — the open grade and the Color page commands.
#include "ui/color/ColorSession.h"

#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderTypes.h"
#include "ui/common/AppContext.h"

#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

namespace kk {

namespace {
/// Longest wait for the still picture of Grab Still (a render of the current frame).
constexpr auto kStillTimeout = std::chrono::seconds(5);
/// Width of the picture rendered for a still (the gallery keeps kGalleryImageWidth).
constexpr int kStillRenderWidth = 640;
} // namespace

ColorSession::ColorSession(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &ColorSession::reload);
    connect(timeline, &Timeline::playheadChanged, this, &ColorSession::reload);
    connect(ctx.project, &Project::projectReset, this, [this] { openClip(0); });
    if (ctx.playback) {
        connect(ctx.playback, &PlaybackEngine::bypassGradesChanged, this,
            &ColorSession::bypassAllChanged);
    }
    reload();
}

quint64 ColorSession::clipAtPlayhead() const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    for (auto t = data.videoTracks.rbegin(); t != data.videoTracks.rend(); ++t) {
        if (!t->enabled)
            continue;
        if (const ClipData* c = t->clipAt(playhead); c && c->kind != ClipKind::Subtitle)
            return c->id;
    }
    return 0;
}

void ColorSession::setTarget(Target target)
{
    if (target == m_target)
        return;
    m_target = target;
    m_selected = 0;
    reload();
    emit clipChanged();
}

void ColorSession::openClip(quint64 clipId)
{
    m_pinned = clipId;
    reload();
}

int64_t ColorSession::gradeFrame() const
{
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    if (m_target == Target::Timeline)
        return playhead;
    const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), m_clipId);
    return c ? c->localFrame(playhead) : 0;
}

void ColorSession::reload()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    quint64 clip = 0;
    if (m_target == Target::Clip) {
        // A pinned clip stays while the playhead is on it (or nothing else is there).
        const ClipData* pinned = m_pinned ? editops::clipById(data, m_pinned) : nullptr;
        const int64_t playhead = m_ctx.project->timeline()->playhead();
        if (pinned && pinned->contains(playhead))
            clip = m_pinned;
        else {
            m_pinned = 0;
            clip = clipAtPlayhead();
        }
    }
    const bool otherClip = clip != m_clipId;
    m_clipId = clip;
    ClipGrade grade;
    if (m_target == Target::Timeline)
        grade = data.grade;
    else if (const ClipData* c = editops::clipById(data, m_clipId))
        grade = c->grade;
    const bool changed = grade != m_grade || otherClip;
    m_grade = std::move(grade);
    if (otherClip || !m_grade.node(m_selected)) {
        const quint64 sel = gradeops::defaultSelection(m_grade);
        if (sel != m_selected) {
            m_selected = sel;
            emit selectedNodeChanged(m_selected);
        }
    }
    if (otherClip)
        emit clipChanged();
    if (changed)
        emit gradeChanged();
}

void ColorSession::selectNode(quint64 id)
{
    if (id && !m_grade.node(id))
        return;
    if (id == m_selected)
        return;
    m_selected = id;
    emit selectedNodeChanged(id);
}

bool ColorSession::commit(const ClipGrade& grade, const QString& label)
{
    if (!hasGrade())
        return false;
    Project* p = m_ctx.project;
    const quint64 uid = p->activeTimelineUid();
    const bool ok = m_target == Target::Timeline ? p->setTimelineGrade(uid, grade, label)
                                                 : p->setClipGrade(uid, m_clipId, grade, label);
    reload(); // inside a gesture Timeline::changed() comes at its end
    return ok;
}

quint64 ColorSession::addSerialNode()
{
    if (!hasGrade())
        return 0;
    ClipGrade g = m_grade;
    const quint64 id = gradeops::addSerialNode(g, m_selected);
    if (!commit(g, tr("Add Serial Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addParallelNode()
{
    if (!hasGrade())
        return 0;
    ClipGrade g = m_grade;
    const quint64 id = gradeops::addParallelNode(g, m_selected);
    if (!commit(g, tr("Add Parallel Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addLayerNode()
{
    if (!hasGrade())
        return 0;
    ClipGrade g = m_grade;
    const quint64 id = gradeops::addLayerNode(g, m_selected);
    if (!commit(g, tr("Add Layer Node")))
        return 0;
    selectNode(id);
    return id;
}

bool ColorSession::removeNode(quint64 id)
{
    ClipGrade g = m_grade;
    if (!gradeops::removeNode(g, id ? id : m_selected))
        return false;
    return commit(g, tr("Delete Node"));
}

bool ColorSession::toggleNodeBypass(quint64 id)
{
    ClipGrade g = m_grade;
    const quint64 node = id ? id : m_selected;
    const GradeNode* n = g.node(node);
    if (!n || !gradeops::setNodeEnabled(g, node, !n->enabled))
        return false;
    return commit(g, g.node(node)->enabled ? tr("Enable Node") : tr("Bypass Node"));
}

bool ColorSession::resetNode(quint64 id)
{
    ClipGrade g = m_grade;
    if (!gradeops::resetNode(g, id ? id : m_selected))
        return false;
    return commit(g, tr("Reset Node"));
}

bool ColorSession::resetGrade()
{
    if (m_grade.isEmpty())
        return false;
    return commit(ClipGrade {}, tr("Reset Grade"));
}

ClipGrade ColorSession::editableGrade(quint64 wanted, quint64* node) const
{
    ClipGrade g = m_grade;
    *node = wanted ? wanted : m_selected;
    if (g.isEmpty())
        *node = gradeops::initialize(g);
    else if (!g.node(*node))
        *node = gradeops::defaultSelection(g);
    return g;
}

bool ColorSession::setParam(quint64 nodeId, const QString& key, double value, const QString& label)
{
    return setParams(nodeId, {{key, value}}, label);
}

bool ColorSession::setParams(
    quint64 nodeId, const QVector<std::pair<QString, double>>& values, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    const int64_t frame = gradeFrame();
    for (const auto& [key, value] : values)
        n->params.animated(key, gradekeys::defaultValue(key)).setValue(frame, value);
    const bool ok = commit(g, label);
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

bool ColorSession::setCurve(
    quint64 nodeId, GradeCurve curve, const CurvePoints& points, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    n->curves[curve] = points;
    const bool ok = commit(g, label);
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

void ColorSession::beginGesture(const QString& label)
{
    ++m_gesture;
    m_ctx.project->beginEdit(label);
}

void ColorSession::endGesture()
{
    if (m_gesture <= 0)
        return;
    --m_gesture;
    m_ctx.project->endEdit();
}

bool ColorSession::bypassAll() const
{
    return m_ctx.playback && m_ctx.playback->bypassGrades();
}

void ColorSession::setBypassAll(bool bypass)
{
    if (m_ctx.playback)
        m_ctx.playback->setBypassGrades(bypass); // emits bypassGradesChanged
}

quint64 ColorSession::grabStill()
{
    if (!hasGrade())
        return 0;
    QImage picture;
    if (m_ctx.playback && m_ctx.playback->compositor()) {
        const TimelineData& data = m_ctx.project->timeline()->data();
        const QSize size(kStillRenderWidth,
            std::max(1, int(std::lround(double(kStillRenderWidth) * data.height / data.width))));
        auto future = m_ctx.playback->renderImage(m_ctx.project->timeline()->playhead(), size);
        if (future.wait_for(kStillTimeout) == std::future_status::ready) {
            const RenderedFramePtr frame = future.get();
            if (frame && frame->ok)
                picture = frame->image;
        }
    }
    return m_ctx.project->gallery()->addStill(picture, m_grade);
}

int ColorSession::applyStill(quint64 stillId, const QVector<quint64>& clipIds)
{
    const GalleryStill* still = m_ctx.project->gallery()->still(stillId);
    if (!still)
        return 0;
    if (clipIds.isEmpty() && m_target == Target::Timeline)
        return commit(still->grade, tr("Apply Grade")) ? 1 : 0;
    const QVector<quint64> targets = clipIds.isEmpty() ? QVector<quint64> {m_clipId} : clipIds;
    const int n = m_ctx.project->setClipGrades(
        m_ctx.project->activeTimelineUid(), targets, still->grade, tr("Apply Grade"));
    reload();
    return n;
}

int ColorSession::copyGrade(quint64 fromClipId, const QVector<quint64>& toClipIds)
{
    const quint64 uid = m_ctx.project->activeTimelineUid();
    if (!editops::clipById(m_ctx.project->timeline()->data(), fromClipId))
        return 0;
    const ClipGrade grade = m_ctx.project->clipGrade(uid, fromClipId);
    const QVector<quint64> targets = toClipIds.isEmpty() ? QVector<quint64> {m_clipId} : toClipIds;
    const int n = m_ctx.project->setClipGrades(uid, targets, grade, tr("Copy Grade"));
    reload();
    return n;
}

} // namespace kk
