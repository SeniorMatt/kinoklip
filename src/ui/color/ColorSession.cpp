// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSession — the open grade and the Color page commands.
#include "ui/color/ColorSession.h"

#include "core/EditOps.h"
#include "core/GradeMath.h"
#include "core/GradeGallery.h"
#include "core/GradeKeyframes.h"
#include "core/Project.h"
#include "core/Sizing.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/GradeLut.h"
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

/// The ids of the nodes of @p g.
QVector<quint64> nodeIds(const ClipGrade& g)
{
    QVector<quint64> ids;
    for (const GradeNode& n : g.nodes)
        ids.append(n.id);
    return ids;
}

/// The nodes added to @p g since @p before belong to the open compound @p level; when @p leads
/// the newest picture node takes over as its output from @p after (a node added at the end).
void adoptNew(ClipGrade& g, const QVector<quint64>& before, quint64 level, quint64 after, bool leads)
{
    if (level == 0 || !g.node(level))
        return;
    quint64 newest = 0;
    for (GradeNode& n : g.nodes) {
        if (before.contains(n.id))
            continue;
        n.compound = level;
        if (n.hasPictureOutput())
            newest = std::max(newest, n.id);
    }
    GradeNode* c = g.node(level);
    if (leads && newest && c->innerOutput == after)
        c->innerOutput = newest;
}
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
    m_extraSelected.erase(std::remove_if(m_extraSelected.begin(), m_extraSelected.end(),
                              [this](quint64 id) { return !m_grade.node(id) || id == m_selected; }),
        m_extraSelected.end());
    if (m_level != 0 && (!m_grade.node(m_level) || otherClip)) {
        m_level = 0;
        emit openCompoundChanged(0);
    }
    // The strip selection follows the graded clip: moving to a clip outside the selection
    // (playhead, next clip) drops it, so Ctrl+V pastes to the clip on screen, not to clips
    // selected earlier.
    if (otherClip && m_clipId && !m_selectedClips.isEmpty() && !m_selectedClips.contains(m_clipId))
        setSelectedClips({});
    if (otherClip)
        emit clipChanged();
    if (changed)
        emit gradeChanged();
    // Input / Output Sizing: the palette's fields follow edits, undo and the playhead.
    ParamSet in;
    if (const ClipData* c = editops::clipById(data, m_clipId))
        in = c->transform;
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    const bool sizingMoved = in != m_inputSizing || data.outputSizing != m_outputSizing
        || otherClip || playhead != m_playhead;
    m_inputSizing = std::move(in);
    m_outputSizing = data.outputSizing;
    m_playhead = playhead;
    if (sizingMoved)
        emit sizingChanged();
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
    const bool extras = !m_extraSelected.isEmpty();
    m_extraSelected.clear();
    if (id == m_selected) {
        if (extras)
            emit selectionChanged();
        return;
    }
    m_selected = id;
    emit selectedNodeChanged(id);
    emit selectionChanged();
    fixWindowSelection();
}

QVector<quint64> ColorSession::selectedNodes() const
{
    QVector<quint64> out;
    if (m_selected)
        out.append(m_selected);
    out += m_extraSelected;
    return out;
}

void ColorSession::toggleNodeSelection(quint64 id)
{
    if (!id || !m_grade.node(id))
        return;
    if (id == m_selected) {
        if (m_extraSelected.isEmpty())
            return; // the last selected node stays
        const quint64 next = m_extraSelected.takeFirst();
        m_selected = next;
        emit selectedNodeChanged(next);
        fixWindowSelection();
    } else if (m_extraSelected.contains(id)) {
        m_extraSelected.removeAll(id);
    } else {
        m_extraSelected.append(id);
    }
    emit selectionChanged();
}

void ColorSession::selectNodes(const QVector<quint64>& ids)
{
    QVector<quint64> valid;
    for (const quint64 id : ids) {
        if (m_grade.node(id) && !valid.contains(id))
            valid.append(id);
    }
    if (valid.isEmpty()) {
        selectNode(0);
        return;
    }
    const bool otherCurrent = valid.first() != m_selected;
    m_selected = valid.first();
    m_extraSelected = valid.mid(1);
    if (otherCurrent) {
        emit selectedNodeChanged(m_selected);
        fixWindowSelection();
    }
    emit selectionChanged();
}

quint64 ColorSession::levelSource() const
{
    const GradeNode* c = m_grade.node(m_level);
    return c ? c->inputs.value(0, 0) : 0;
}

bool ColorSession::openCompound(quint64 id)
{
    const GradeNode* c = m_grade.node(id ? id : m_selected);
    if (!c || c->type != GradeNodeType::Compound)
        return false;
    const quint64 open = c->id;
    const quint64 inner = c->innerOutput;
    m_level = open;
    emit openCompoundChanged(m_level);
    selectNode(inner);
    emit selectionChanged();
    return true;
}

bool ColorSession::closeCompound()
{
    const GradeNode* c = m_grade.node(m_level);
    if (m_level == 0)
        return false;
    const quint64 closed = m_level;
    m_level = c ? c->compound : 0;
    emit openCompoundChanged(m_level);
    selectNode(closed);
    return true;
}

quint64 ColorSession::groupNodes()
{
    if (!hasGrade() || m_grade.isEmpty())
        return 0;
    ClipGrade g = m_grade;
    const quint64 id = gradeops::groupIntoCompound(g, selectedNodes());
    if (!id || !commit(g, tr("Create Compound Node")))
        return 0;
    selectNode(id);
    return id;
}

bool ColorSession::ungroupNode(quint64 id)
{
    ClipGrade g = m_grade;
    const quint64 node = id ? id : m_selected;
    const GradeNode* c = g.node(node);
    if (!c || c->type != GradeNodeType::Compound)
        return false;
    const quint64 inner = c->innerOutput;
    if (!gradeops::ungroupCompound(g, node) || !commit(g, tr("Decompose Compound Node")))
        return false;
    selectNode(inner);
    return true;
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
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addSerialNode(g, after);
    adoptNew(g, before, m_level, after, true);
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
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addParallelNode(g, after);
    adoptNew(g, before, m_level, after, true);
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
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addLayerNode(g, after);
    adoptNew(g, before, m_level, after, true);
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
    const QVector<quint64> ids = nodeIds(g);
    const quint64 id = gradeops::addSerialNodeBefore(g, before);
    adoptNew(g, ids, m_level, 0, false);
    if (!commit(g, tr("Add Serial Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addOutsideNode()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addOutsideNode(g, after);
    if (!id)
        return 0;
    adoptNew(g, before, m_level, after, true);
    if (!commit(g, tr("Add Outside Node")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addKeyMixer()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addKeyMixer(g, after);
    adoptNew(g, before, m_level, after, false);
    if (!commit(g, tr("Add Key Mixer")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addSplitterCombiner()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addSplitterCombiner(g, after);
    if (!id)
        return 0;
    adoptNew(g, before, m_level, after, true);
    if (!commit(g, tr("Add Splitter / Combiner")))
        return 0;
    selectNode(id);
    return id;
}

quint64 ColorSession::addAlphaOutput()
{
    if (!hasGrade())
        return 0;
    quint64 after = m_selected;
    ClipGrade g = withFirstNode(&after);
    const QVector<quint64> before = nodeIds(g);
    const quint64 id = gradeops::addAlphaOutput(g, after);
    adoptNew(g, before, m_level, after, false);
    if (!commit(g, tr("Add Alpha Output")))
        return 0;
    selectNode(id);
    return id;
}

bool ColorSession::connectKeys(quint64 from, quint64 to, int inputIndex, bool invert)
{
    ClipGrade g = m_grade;
    if (!gradeops::connectKey(g, from, to, inputIndex, invert) || g == m_grade)
        return false;
    return commit(g, tr("Connect Keys"));
}

bool ColorSession::disconnectKeyInput(quint64 to, int inputIndex)
{
    ClipGrade g = m_grade;
    if (!gradeops::disconnectKey(g, to, inputIndex))
        return false;
    return commit(g, tr("Disconnect Keys"));
}

bool ColorSession::setKeyInverted(quint64 to, int inputIndex, bool invert)
{
    ClipGrade g = m_grade;
    GradeNode* n = g.node(to);
    if (!n)
        return false;
    if (n->type == GradeNodeType::KeyMixer) {
        if (inputIndex < 0 || inputIndex >= n->inputs.size())
            return false;
        while (n->keyInverts.size() < n->inputs.size())
            n->keyInverts.append(false);
        n->keyInverts[inputIndex] = invert;
    } else if (n->hasKeyInput() && n->keyInput != 0) {
        n->keyInputInvert = invert;
    } else {
        return false;
    }
    if (g == m_grade)
        return false;
    return commit(g, tr("Invert Key"));
}

bool ColorSession::setKeyGain(quint64 mixer, int inputIndex, double gain)
{
    ClipGrade g = m_grade;
    GradeNode* n = g.node(mixer);
    if (!n || n->type != GradeNodeType::KeyMixer || inputIndex < 0
        || inputIndex >= n->inputs.size())
        return false;
    while (n->keyGains.size() < n->inputs.size())
        n->keyGains.append(1.0);
    n->keyGains[inputIndex] = std::clamp(gain, -4.0, 4.0);
    if (g == m_grade)
        return false;
    return commit(g, tr("Key Mixer Gain"));
}

bool ColorSession::setCompoundOutput(quint64 from)
{
    ClipGrade g = m_grade;
    GradeNode* c = g.node(m_level);
    const GradeNode* source = g.node(from);
    if (!c || !source || source->compound != m_level || !source->hasPictureOutput()
        || c->innerOutput == from)
        return false;
    c->innerOutput = from;
    return commit(g, tr("Connect Nodes"));
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
    for (const auto& [key, value] : values) {
        AnimatedValue& v = n->params.animated(key, gradekeys::defaultValue(key));
        if (m_autoKey)
            v.setKey(frame, value);
        else
            v.setValue(frame, value);
    }
    const bool ok = commit(g, label);
    if (ok && m_selected != node)
        selectNode(node);
    return ok;
}

bool ColorSession::generateLut(quint64 clipId, int size, const QString& path, QString* error)
{
    const ClipData* clip = editops::clipById(m_ctx.project->timeline()->data(), clipId);
    if (!clip) {
        if (error)
            *error = tr("The clip is not on the timeline.");
        return false;
    }
    const ClipGrade grade = m_ctx.project->clipGrade(m_ctx.project->activeTimelineUid(), clipId);
    if (grade.isEmpty()) {
        if (error)
            *error = tr("The clip has no grade.");
        return false;
    }
    const int64_t frame = clipId == m_clipId ? gradeFrame() : 0;
    QString name = clip->name;
    if (name.isEmpty())
        name = tr("Kinoklip grade");
    return render::writeGradeCube(grade, frame, size, name, path, error);
}

bool ColorSession::autoColor(quint64 nodeId, const QImage& picture)
{
    if (picture.isNull())
        return false;
    const QImage image = picture.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> rgb;
    rgb.reserve(size_t(image.width()) * size_t(image.height()) * 3);
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            rgb.push_back(float(qRed(line[x])) / 255.0f);
            rgb.push_back(float(qGreen(line[x])) / 255.0f);
            rgb.push_back(float(qBlue(line[x])) / 255.0f);
        }
    }
    const grademath::AutoColorResult r = grademath::autoColor(rgb.data(), rgb.size() / 3);
    if (!r.valid)
        return false;
    namespace gk = gradekeys;
    QVector<std::pair<QString, double>> values;
    for (int ch = 0; ch < 3; ++ch) {
        values.append({gk::wheelKey(gk::Wheel::Lift, ch), double(r.lift[ch])});
        values.append({gk::wheelKey(gk::Wheel::Gamma, ch), double(r.gamma[ch])});
        values.append({gk::wheelKey(gk::Wheel::Gain, ch), double(r.gain[ch])});
    }
    return setParams(nodeId, values, tr("Auto Color"));
}

bool ColorSession::applyChartMatch(
    quint64 nodeId, const colormatch::ChartMatch& match, const QString& label)
{
    if (!hasGrade() || !match.valid)
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    colormatch::applyChartToNode(*n, match);
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

bool ColorSession::setWarp(quint64 nodeId, const GradeWarp& warp, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    n->warp = warp;
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
        for (const auto& [key, value] : values) {
            AnimatedValue& v = w.params.animated(key, gradewindowkeys::defaultValue(key));
            if (m_autoKey)
                v.setKey(frame, value);
            else
                v.setValue(frame, value);
        }
    });
}

bool ColorSession::setWindowShape(
    quint64 windowId, const GradeWindowShapePoints& points, const QString& label)
{
    const int64_t frame = gradeFrame();
    return editWindow(windowId, label, [&](GradeWindow& w) {
        if (w.shapeKeys.isEmpty())
            w.points = points;
        else if (points.size() == w.points.size())
            w.shapeKeys.insert(frame, points);
    });
}

bool ColorSession::addWindowPoint(quint64 windowId, int index, double t, const QString& label)
{
    return editWindow(windowId, label, [&](GradeWindow& w) {
        if (w.points.size() >= kMaxWindowPoints)
            return;
        insertWindowPoint(w.points, index, t);
        for (auto it = w.shapeKeys.begin(); it != w.shapeKeys.end(); ++it)
            insertWindowPoint(it.value(), index, t);
    });
}

bool ColorSession::removeWindowPoint(quint64 windowId, int index, const QString& label)
{
    return editWindow(windowId, label, [&](GradeWindow& w) {
        if (w.points.size() <= 3 || index < 0 || index >= w.points.size())
            return;
        w.points.remove(index);
        for (auto it = w.shapeKeys.begin(); it != w.shapeKeys.end(); ++it)
            it.value().remove(index);
    });
}

bool ColorSession::keyWindowShape(quint64 windowId)
{
    const int64_t frame = gradeFrame();
    return editWindow(windowId, tr("Keyframe Window Shape"), [&](GradeWindow& w) {
        if (w.shape == GradeWindowShape::Polygon || w.shape == GradeWindowShape::Curve)
            w.shapeKeys.insert(frame, w.shapeAt(frame));
    });
}

bool ColorSession::removeWindowShapeKey(quint64 windowId)
{
    const int64_t frame = gradeFrame();
    return editWindow(windowId, tr("Remove Window Shape Keyframe"),
        [&](GradeWindow& w) { w.shapeKeys.remove(frame); });
}

bool ColorSession::clearWindowShapeKeys(quint64 windowId)
{
    const int64_t frame = gradeFrame();
    return editWindow(windowId, tr("Clear Window Shape Keyframes"), [&](GradeWindow& w) {
        if (w.shapeKeys.isEmpty())
            return;
        w.points = w.shapeAt(frame);
        w.shapeKeys.clear();
    });
}

bool ColorSession::setWindowFlags(quint64 windowId, bool enabled, bool invert)
{
    return editWindow(windowId, tr("Window"), [&](GradeWindow& w) {
        w.enabled = enabled;
        w.invert = invert;
    });
}

GradeTrack ColorSession::track() const
{
    const GradeNode* n = m_grade.node(m_selected);
    return n ? n->track : GradeTrack {};
}

bool ColorSession::setTrack(quint64 nodeId, const GradeTrack& track, const QString& label)
{
    if (!hasGrade())
        return false;
    quint64 node = 0;
    ClipGrade g = editableGrade(nodeId, &node);
    GradeNode* n = g.node(node);
    if (!n || n->isMixer())
        return false;
    n->track = track;
    if (g == m_grade)
        return false;
    return commit(g, label);
}

bool ColorSession::clearTrack()
{
    GradeTrack t = track();
    if (!t.hasData())
        return false;
    t.samples.clear();
    t.firstFrame = 0;
    return setTrack(0, t, tr("Clear Track"));
}

bool ColorSession::trackRange(int64_t* firstFrame, int64_t* lastFrame, int64_t* timelineOffset) const
{
    if (!hasGrade())
        return false;
    const TimelineData& data = m_ctx.project->timeline()->data();
    int64_t first = 0, last = 0, offset = 0;
    if (m_target == Target::Timeline) {
        last = std::max<int64_t>(0, data.duration() - 1);
    } else {
        const ClipData* c = editops::clipById(data, m_clipId);
        if (!c)
            return false;
        last = std::max<int64_t>(0, c->duration - 1);
        offset = c->start;
    }
    if (firstFrame)
        *firstFrame = first;
    if (lastFrame)
        *lastFrame = last;
    if (timelineOffset)
        *timelineOffset = offset;
    return true;
}

double ColorSession::pictureAspect() const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    return data.height > 0 ? double(data.width) / data.height : 16.0 / 9.0;
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
    return m_ctx.project->gallery()->addStill(currentPicture(), m_grade);
}

bool ColorSession::saveMemory(int index)
{
    if (!hasGrade())
        return false;
    return m_ctx.project->gallery()->setMemory(index, m_grade, currentPicture());
}

int ColorSession::applyMemory(int index, const QVector<quint64>& clipIds)
{
    const GalleryMemory& m = m_ctx.project->gallery()->memory(index);
    if (!m.used)
        return 0;
    return applyGrade(m.grade,
        clipIds, tr("Apply Memory %1").arg(GradeGallery::memoryLetter(index)));
}

QImage ColorSession::currentPicture()
{
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
    return picture;
}

int ColorSession::applyStill(quint64 stillId, const QVector<quint64>& clipIds)
{
    const GalleryStill* still = m_ctx.project->gallery()->still(stillId);
    return still ? applyGrade(still->grade, clipIds, tr("Apply Grade")) : 0;
}

int ColorSession::appendStill(quint64 stillId, const QVector<quint64>& clipIds)
{
    const GalleryStill* still = m_ctx.project->gallery()->still(stillId);
    return still ? applyGrade(still->grade, clipIds, tr("Append Node Graph"), true) : 0;
}

int ColorSession::applyGrade(
    const ClipGrade& grade, const QVector<quint64>& clipIds, const QString& label, bool append)
{
    if (grade.isEmpty() && append)
        return 0;
    Project* p = m_ctx.project;
    const quint64 uid = p->activeTimelineUid();
    if (clipIds.isEmpty() && m_target == Target::Timeline) {
        ClipGrade g = grade;
        if (append) {
            g = p->timelineGrade(uid);
            gradeops::appendGraph(g, grade);
        }
        return commit(g, label) ? 1 : 0;
    }
    const QVector<quint64> targets = clipIds.isEmpty() ? QVector<quint64> {m_clipId} : clipIds;
    int n = 0;
    if (!append) {
        n = p->setClipGrades(uid, targets, grade, label);
    } else {
        // Each clip keeps its own nodes, so the grades differ: one step for all of them.
        beginGesture(label);
        for (const quint64 id : targets) {
            ClipGrade g = p->clipGrade(uid, id);
            if (gradeops::appendGraph(g, grade) && p->setClipGrades(uid, {id}, g, label) == 1)
                ++n;
        }
        endGesture();
    }
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

// ---- Keyframes palette ----

namespace {

int doAdd(GradeNode& n, int64_t f) { return gradekeyframes::addKeys(n, f); }
int doAdd(ParamSet& p, int64_t f) { return sizing::addKeys(p, f); }
int doDelete(GradeNode& n, int64_t f) { return gradekeyframes::deleteKeys(n, f); }
int doDelete(ParamSet& p, int64_t f) { return keyrows::deleteKeys(sizing::rowOf(p), f); }
int doMove(GradeNode& n, int64_t a, int64_t b) { return gradekeyframes::moveKeys(n, a, b); }
int doMove(ParamSet& p, int64_t a, int64_t b) { return keyrows::moveKeys(sizing::rowOf(p), a, b); }
int doMode(GradeNode& n, int64_t f, keyrows::Mode m) { return gradekeyframes::setMode(n, f, m); }
int doMode(ParamSet& p, int64_t f, keyrows::Mode m) { return keyrows::setMode(sizing::rowOf(p), f, m); }
int doInterp(GradeNode& n, int64_t f, Interp i) { return gradekeyframes::setInterp(n, f, i); }
int doInterp(ParamSet& p, int64_t f, Interp i) { return keyrows::setInterp(sizing::rowOf(p), f, i); }

ColorSession::SizingScope scopeOf(const ColorSession::KeyRow& row)
{
    return row.kind == ColorSession::KeyRow::Kind::Input ? ColorSession::SizingScope::Input
                                                         : ColorSession::SizingScope::Output;
}

} // namespace

QVector<ColorSession::KeyRow> ColorSession::keyRows() const
{
    QVector<KeyRow> rows;
    if (!hasGrade())
        return rows;
    for (const GradeNode& n : m_grade.nodes) {
        if (!n.isMixer())
            rows.append({KeyRow::Kind::Node, n.id});
    }
    if (hasSizing(SizingScope::Input))
        rows.append({KeyRow::Kind::Input, 0});
    rows.append({KeyRow::Kind::Output, 0});
    return rows;
}

bool ColorSession::stripRange(int64_t* firstFrame, int64_t* lastFrame) const
{
    return trackRange(firstFrame, lastFrame, nullptr);
}

void ColorSession::seekStrip(int64_t frame)
{
    int64_t first = 0, last = 0, offset = 0;
    if (!trackRange(&first, &last, &offset))
        return;
    m_ctx.project->timeline()->setPlayhead(offset + std::clamp(frame, first, last));
}

int64_t ColorSession::rowOffset(const KeyRow& row) const
{
    // Output Sizing is keyed in timeline frames; the strip of a clip counts from its start.
    int64_t first = 0, last = 0, offset = 0;
    if (row.kind == KeyRow::Kind::Output && trackRange(&first, &last, &offset))
        return offset;
    return 0;
}

QVector<int64_t> ColorSession::rowFrames(const KeyRow& row) const
{
    QVector<int64_t> frames;
    const int64_t off = rowOffset(row);
    if (row.kind == KeyRow::Kind::Node) {
        if (const GradeNode* n = m_grade.node(row.nodeId))
            frames = gradekeyframes::frames(*n);
    } else {
        const ParamSet p = sizingOf(scopeOf(row));
        frames = keyrows::frames(sizing::rowOf(p));
    }
    for (int64_t& f : frames)
        f -= off;
    // Output Sizing keys outside the open clip are not on its strip.
    int64_t first = 0, last = 0;
    if (row.kind == KeyRow::Kind::Output && trackRange(&first, &last, nullptr)) {
        frames.erase(std::remove_if(frames.begin(), frames.end(),
                         [&](int64_t f) { return f < first || f > last; }),
            frames.end());
    }
    return frames;
}

bool ColorSession::rowHasKey(const KeyRow& row, int64_t frame) const
{
    return rowFrames(row).contains(frame);
}

keyrows::Mode ColorSession::rowKeyMode(const KeyRow& row, int64_t frame) const
{
    const int64_t f = frame + rowOffset(row);
    if (row.kind == KeyRow::Kind::Node) {
        const GradeNode* n = m_grade.node(row.nodeId);
        return n ? gradekeyframes::modeAt(*n, f) : keyrows::Mode::Dynamic;
    }
    const ParamSet p = sizingOf(scopeOf(row));
    return keyrows::modeAt(sizing::rowOf(p), f);
}

Interp ColorSession::rowKeyInterp(const KeyRow& row, int64_t frame) const
{
    const int64_t f = frame + rowOffset(row);
    if (row.kind == KeyRow::Kind::Node) {
        const GradeNode* n = m_grade.node(row.nodeId);
        return n ? gradekeyframes::interpAt(*n, f) : Interp::Custom;
    }
    const ParamSet p = sizingOf(scopeOf(row));
    return keyrows::interpAt(sizing::rowOf(p), f);
}

template<typename Fn>
bool ColorSession::editRow(const KeyRow& row, const QString& label, Fn&& edit)
{
    if (!hasGrade())
        return false;
    if (row.kind == KeyRow::Kind::Node) {
        ClipGrade g = m_grade;
        GradeNode* n = g.node(row.nodeId);
        if (!n || n->isMixer() || edit(*n) <= 0)
            return false;
        return commit(g, label);
    }
    const SizingScope scope = scopeOf(row);
    if (!hasSizing(scope))
        return false;
    ParamSet p = sizingOf(scope);
    if (edit(p) <= 0)
        return false;
    return commitSizing(scope, p, label);
}

bool ColorSession::addRowKey(const KeyRow& row, int64_t frame)
{
    const int64_t f = frame + rowOffset(row);
    return editRow(row, tr("Add Keyframe"), [f](auto& t) { return doAdd(t, f); });
}

bool ColorSession::deleteRowKey(const KeyRow& row, int64_t frame)
{
    const int64_t f = frame + rowOffset(row);
    return editRow(row, tr("Delete Keyframe"), [f](auto& t) { return doDelete(t, f); });
}

bool ColorSession::moveRowKey(const KeyRow& row, int64_t from, int64_t to)
{
    const int64_t off = rowOffset(row);
    const int64_t a = from + off, b = to + off;
    return editRow(row, tr("Move Keyframe"), [a, b](auto& t) { return doMove(t, a, b); });
}

bool ColorSession::setRowKeyMode(const KeyRow& row, int64_t frame, keyrows::Mode mode)
{
    const int64_t f = frame + rowOffset(row);
    return editRow(row,
        mode == keyrows::Mode::Static ? tr("Static Keyframe") : tr("Dynamic Keyframe"),
        [f, mode](auto& t) { return doMode(t, f, mode); });
}

bool ColorSession::setRowKeyInterp(const KeyRow& row, int64_t frame, Interp interp)
{
    const int64_t f = frame + rowOffset(row);
    return editRow(
        row, tr("Keyframe Dissolve"), [f, interp](auto& t) { return doInterp(t, f, interp); });
}

void ColorSession::setAutoKeyframe(bool on)
{
    if (on == m_autoKey)
        return;
    m_autoKey = on;
    emit autoKeyframeChanged(on);
}

// ---- Sizing palette ----

bool ColorSession::hasSizing(SizingScope scope) const
{
    if (scope == SizingScope::Input)
        return m_target == Target::Clip && m_clipId != 0;
    return true;
}

ParamSet ColorSession::sizingOf(SizingScope scope) const
{
    return scope == SizingScope::Input ? m_inputSizing : m_outputSizing;
}

int64_t ColorSession::sizingFrame(SizingScope scope) const
{
    if (scope == SizingScope::Input)
        return m_target == Target::Clip ? gradeFrameOf(m_clipId) : 0;
    return m_ctx.project->timeline()->playhead();
}

bool ColorSession::commitSizing(SizingScope scope, const ParamSet& params, const QString& label)
{
    Project* p = m_ctx.project;
    const quint64 uid = p->activeTimelineUid();
    const bool ok = scope == SizingScope::Input ? p->setClipTransform(uid, m_clipId, params, label)
                                                : p->setOutputSizing(uid, params, label);
    reload(); // inside a gesture Timeline::changed() comes at its end
    return ok;
}

bool ColorSession::setSizing(
    SizingScope scope, const QVector<std::pair<QString, double>>& values, const QString& label)
{
    if (!hasSizing(scope))
        return false;
    ParamSet p = sizingOf(scope);
    const int64_t frame = sizingFrame(scope);
    for (const auto& [key, value] : values)
        sizing::setAt(p, key, frame, value, m_autoKey);
    return commitSizing(scope, p, label);
}

bool ColorSession::setSizingZoom(SizingScope scope, double zoom, const QString& label)
{
    if (!hasSizing(scope))
        return false;
    ParamSet p = sizingOf(scope);
    sizing::setZoomAt(p, sizingFrame(scope), zoom, m_autoKey);
    return commitSizing(scope, p, label);
}

bool ColorSession::setSizingFlip(
    SizingScope scope, const QString& key, bool on, const QString& label)
{
    if (!hasSizing(scope) || (key != keys::FlipH && key != keys::FlipV))
        return false;
    ParamSet p = sizingOf(scope);
    if (on)
        p.setStatic(key, true);
    else
        p.statics.remove(key);
    return commitSizing(scope, p, label);
}

bool ColorSession::resetSizing(SizingScope scope)
{
    if (!hasSizing(scope))
        return false;
    ParamSet p = sizingOf(scope);
    for (const QString& key : sizing::sizingKeys())
        p.values.remove(key);
    p.statics.remove(keys::FlipH);
    p.statics.remove(keys::FlipV);
    return commitSizing(scope, p, tr("Reset Sizing"));
}

} // namespace kk
