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
    return gradeFrameOf(m_clipId);
}

int64_t ColorSession::gradeFrameOf(quint64 clipId) const
{
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    if (m_target == Target::Timeline)
        return playhead;
    const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), clipId);
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
    // A keyframed grade has other values at another frame: the palettes, thumbnails and
    // highlight follow the playhead.
    const int64_t frame = m_target == Target::Timeline || clip ? gradeFrameOf(clip) : 0;
    const bool otherFrame = frame != m_frame && grade.isAnimated();
    m_frame = frame;
    const bool changed = grade != m_grade || otherClip || otherFrame;
    m_grade = std::move(grade);
    if (otherClip || !m_grade.node(m_selected)) {
        quint64 sel = gradeops::defaultSelection(m_grade);
        // The palettes edit correctors: when the output is a mixer, start on the last
        // corrector it reaches.
        if (const GradeNode* n = m_grade.node(sel); n && n->isMixer()) {
            for (const int i : gradeops::evaluationOrder(m_grade)) {
                if (!m_grade.nodes[i].isMixer())
                    sel = m_grade.nodes[i].id;
            }
        }
        if (sel != m_selected) {
            m_selected = sel;
            emit selectedNodeChanged(m_selected);
        }
    }
    fixWindowSelection();
    // The strip selection follows the graded clip: moving to a clip outside the selection
    // (playhead, next clip) drops it, so Ctrl+V pastes to the clip on screen, not to clips
    // selected earlier.
    if (otherClip && m_clipId && !m_selectedClips.isEmpty() && !m_selectedClips.contains(m_clipId))
        setSelectedClips({});
    if (otherClip)
        emit clipChanged();
    if (changed)
        emit gradeChanged();
}

void ColorSession::fixWindowSelection()
{
    const GradeNode* n = m_grade.node(m_selected);
    quint64 want = m_selectedWindow;
    bool found = false;
    if (n) {
        for (const GradeWindow& w : n->windows)
            found = found || w.id == want;
    }
    if (!found)
        want = n && !n->windows.isEmpty() ? n->windows.first().id : 0;
    if (want != m_selectedWindow) {
        m_selectedWindow = want;
        emit selectedWindowChanged(want);
    }
}

void ColorSession::selectNode(quint64 id)
{
    if (id && !m_grade.node(id))
        return;
    if (id == m_selected)
        return;
    m_selected = id;
    emit selectedNodeChanged(id);
    fixWindowSelection();
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
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const quint64 id = gradeops::addSerialNode(g, after);
    if (!commit(g, tr("Add Serial Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addParallelNode()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const quint64 id = gradeops::addParallelNode(g, after);
    if (!commit(g, tr("Add Parallel Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addLayerNode()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const quint64 id = gradeops::addLayerNode(g, after);
    if (!commit(g, tr("Add Layer Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addSerialNodeBefore()
{
    if (!hasGrade())
        return 0;
    quint64 before = m_selected;
    ClipGrade g = withFirstNode(&before);
    const quint64 id = gradeops::addSerialNodeBefore(g, before);
    if (!commit(g, tr("Add Serial Node")))
        return 0;
    selectNode(id);
    return id;
}

bool ColorSession::connectNodes(quint64 from, quint64 to, int inputIndex)
{
    ClipGrade g = m_grade;
    if (!gradeops::connect(g, from, to, inputIndex) || g == m_grade)
        return false;
    return commit(g, tr("Connect Nodes"));
}

bool ColorSession::disconnectInput(quint64 to, int inputIndex)
{
    ClipGrade g = m_grade;
    if (!gradeops::disconnect(g, to, inputIndex))
        return false;
    return commit(g, tr("Disconnect Nodes"));
}

bool ColorSession::moveNodes(const QVector<std::pair<quint64, QPointF>>& positions)
{
    ClipGrade g = m_grade;
    for (const auto& [id, pos] : positions) {
        if (GradeNode* n = g.node(id))
            n->pos = pos;
    }
    if (g == m_grade)
        return false;
    return commit(g, tr("Move Node"));
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

ClipGrade ColorSession::withFirstNode(quint64* node) const
{
    ClipGrade g = m_grade;
    if (g.isEmpty())
        *node = gradeops::initialize(g);
    return g;
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
    return setCurves(nodeId, {{curve, points}}, label);
}

bool ColorSession::setCurves(
    quint64 nodeId, const QVector<std::pair<GradeCurve, CurvePoints>>& curves, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    for (const auto& [curve, points] : curves)
        n->curves[curve] = points;
    const bool ok = commit(g, label);
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

bool ColorSession::setQualifier(
    quint64 nodeId, const GradeQualifier& qualifier, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    n->qualifier = qualifier;
    const bool ok = commit(g, label);
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

bool ColorSession::setLut(quint64 nodeId, const QString& path)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    n->lutPath = path;
    const bool ok = commit(g, path.isEmpty() ? tr("Remove LUT") : tr("Apply LUT"));
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

namespace {

bool nodeHasKeyAt(const GradeNode& n, int64_t frame)
{
    for (const AnimatedValue& v : n.params.values) {
        if (v.hasKeyAt(frame))
            return true;
    }
    for (const GradeWindow& w : n.windows) {
        for (const AnimatedValue& v : w.params.values) {
            if (v.hasKeyAt(frame))
                return true;
        }
    }
    return false;
}

} // namespace

bool ColorSession::hasKeyframe(quint64 nodeId) const
{
    const GradeNode* n = m_grade.node(nodeId ? nodeId : m_selected);
    return n && nodeHasKeyAt(*n, gradeFrame());
}

bool ColorSession::toggleKeyframe(quint64 nodeId)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    const int64_t frame = gradeFrame();
    const bool remove = nodeHasKeyAt(*n, frame);
    auto keyAll = [&](ParamSet& params, const QVector<QString>& keys, auto&& defaultOf) {
        for (const QString& key : keys) {
            if (remove) {
                if (auto it = params.values.find(key); it != params.values.end())
                    it->removeKey(frame);
            } else {
                AnimatedValue& v = params.animated(key, defaultOf(key));
                v.setKey(frame, v.valueAt(frame));
            }
        }
    };
    keyAll(n->params, gradekeys::allKeys(),
        [](const QString& k) { return gradekeys::defaultValue(k); });
    for (GradeWindow& w : n->windows) {
        keyAll(w.params, gradewindowkeys::allKeys(),
            [](const QString& k) { return gradewindowkeys::defaultValue(k); });
    }
    const bool ok = commit(g, remove ? tr("Remove Keyframe") : tr("Add Keyframe"));
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

quint64 ColorSession::addWindow(GradeWindowShape shape)
{
    if (!hasGrade())
        return 0;
    quint64 node = 0;
    ClipGrade g = editableGrade(0, &node);
    const quint64 id = gradeops::addWindow(g, node, shape);
    if (!id || !commit(g, tr("Add Window")))
        return 0;
    selectNode(node);
    selectWindow(id);
    return id;
}

bool ColorSession::removeWindow(quint64 windowId)
{
    ClipGrade g = m_grade;
    if (!gradeops::removeWindow(g, windowId ? windowId : m_selectedWindow))
        return false;
    return commit(g, tr("Delete Window"));
}

void ColorSession::selectWindow(quint64 windowId)
{
    const GradeNode* owner = nullptr;
    if (windowId && (!m_grade.window(windowId, &owner) || !owner))
        return;
    if (owner && owner->id != m_selected)
        selectNode(owner->id);
    if (windowId == m_selectedWindow)
        return;
    m_selectedWindow = windowId;
    emit selectedWindowChanged(windowId);
}

const GradeWindow* ColorSession::window(quint64 windowId) const
{
    return m_grade.window(windowId ? windowId : m_selectedWindow);
}

template<typename Fn>
bool ColorSession::editWindow(quint64 windowId, const QString& label, Fn&& edit)
{
    ClipGrade g = m_grade;
    GradeWindow* w = g.window(windowId ? windowId : m_selectedWindow);
    if (!w)
        return false;
    edit(*w);
    if (g == m_grade)
        return false;
    return commit(g, label);
}

bool ColorSession::setWindowParams(
    quint64 windowId, const QVector<std::pair<QString, double>>& values, const QString& label)
{
    const int64_t frame = gradeFrame();
    return editWindow(windowId, label, [&](GradeWindow& w) {
        for (const auto& [key, value] : values)
            w.params.animated(key, gradewindowkeys::defaultValue(key)).setValue(frame, value);
    });
}

bool ColorSession::setWindowPoints(
    quint64 windowId, const QVector<QPointF>& points, const QString& label)
{
    return editWindow(windowId, label, [&](GradeWindow& w) { w.points = points; });
}

bool ColorSession::setWindowFlags(quint64 windowId, bool enabled, bool invert)
{
    return editWindow(windowId, tr("Window"), [&](GradeWindow& w) {
        w.enabled = enabled;
        w.invert = invert;
    });
}

QVector<quint64> ColorSession::stripClips() const
{
    struct Entry
    {
        int64_t start;
        int track;
        quint64 id;
    };
    std::vector<Entry> entries;
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (int t = 0; t < int(data.videoTracks.size()); ++t) {
        for (const ClipData& c : data.videoTracks[size_t(t)].clips) {
            if (c.kind != ClipKind::Subtitle)
                entries.push_back({c.start, t, c.id});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.start != b.start ? a.start < b.start : a.track < b.track;
    });
    QVector<quint64> ids;
    ids.reserve(qsizetype(entries.size()));
    for (const Entry& e : entries)
        ids.append(e.id);
    return ids;
}

void ColorSession::gradeClip(quint64 clipId)
{
    const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), clipId);
    if (!c)
        return;
    if (m_target != Target::Clip)
        setTarget(Target::Clip);
    m_pinned = clipId;
    Timeline* timeline = m_ctx.project->timeline();
    if (!c->contains(timeline->playhead()))
        timeline->setPlayhead(c->start); // reload() follows playheadChanged
    openClip(clipId);
}

void ColorSession::setSelectedClips(const QVector<quint64>& clipIds)
{
    if (clipIds == m_selectedClips)
        return;
    m_selectedClips = clipIds;
    emit selectedClipsChanged();
}

bool ColorSession::copyCurrentGrade()
{
    if (!hasGrade())
        return false;
    m_clipboard = m_grade;
    m_hasClipboard = true;
    return true;
}

int ColorSession::pasteGrade(const QVector<quint64>& clipIds)
{
    if (!m_hasClipboard)
        return 0;
    QVector<quint64> targets = clipIds.isEmpty() ? m_selectedClips : clipIds;
    if (targets.isEmpty()) {
        if (m_target == Target::Timeline)
            return commit(m_clipboard, tr("Paste Grade")) ? 1 : 0;
        if (!m_clipId)
            return 0;
        targets = {m_clipId};
    }
    const int n = m_ctx.project->setClipGrades(
        m_ctx.project->activeTimelineUid(), targets, m_clipboard, tr("Paste Grade"));
    reload();
    return n;
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
