// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController implementation: one undoable edit per command, action wiring.
#include "ui/timeline/TimelineController.h"

#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"

#include <QAction>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace ops = editops;

namespace {

/// Default length of stills, generators and titles.
constexpr int kDefaultStillSeconds = 5;
/// Default transition id and length (Ctrl+T).
const QString kDefaultTransition = QStringLiteral("cross_dissolve");
constexpr int kDefaultTransitionSeconds = 1;

/// Runs @p fn on the mutable timeline inside one undoable edit. @p fn returns whether it
/// changed anything; if not, the edit is cancelled so no empty undo step (or id allocation)
/// is left behind.
template<typename Fn>
bool runEdit(Project* project, const QString& label, Fn&& fn)
{
    project->beginEdit(label);
    const bool changed = fn(project->timeline()->mutableData());
    if (changed)
        project->endEdit();
    else
        project->cancelEdit();
    return changed;
}

int64_t framesForSeconds(Rational fps, int seconds)
{
    return std::max<int64_t>(1, usToFrames(int64_t(seconds) * 1000000, fps));
}

} // namespace

TimelineController::TimelineController(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{ }

Project* TimelineController::project() const
{
    return m_ctx.project;
}

Timeline* TimelineController::timeline() const
{
    return m_ctx.project->timeline();
}

// ---- Editor state ----

void TimelineController::setTool(TimelineTool tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    if (m_ctx.actions) {
        const char* id = tool == TimelineTool::Select ? "tool.select"
            : tool == TimelineTool::Trim              ? "tool.trim"
                                                      : "tool.blade";
        if (QAction* a = m_ctx.actions->action(QLatin1String(id)))
            a->setChecked(true);
    }
    emit stateChanged();
}

void TimelineController::setSnapping(bool on)
{
    if (m_snapping == on)
        return;
    m_snapping = on;
    if (m_ctx.actions)
        m_ctx.actions->action(QStringLiteral("timeline.snapping"))->setChecked(on);
    emit stateChanged();
}

void TimelineController::setLinkedSelection(bool on)
{
    if (m_linked == on)
        return;
    m_linked = on;
    if (m_ctx.actions)
        m_ctx.actions->action(QStringLiteral("timeline.linked_selection"))->setChecked(on);
    emit stateChanged();
}

void TimelineController::setTargetTrack(TrackType type, int index)
{
    const int count = int(timeline()->data().tracks(type).size());
    if (index < 0 || index >= count)
        index = -1;
    int& target = type == TrackType::Video ? m_targetVideo : m_targetAudio;
    if (target == index)
        return;
    target = index;
    emit stateChanged();
}

void TimelineController::toggleTarget(TrackRef ref)
{
    const int current = ref.type == TrackType::Video ? m_targetVideo : m_targetAudio;
    setTargetTrack(ref.type, current == ref.index ? -1 : ref.index);
}

QVector<quint64> TimelineController::withLinks(const QVector<quint64>& ids) const
{
    return m_linked ? ops::withLinkedClips(timeline()->data(), ids) : ids;
}

QVector<quint64> TimelineController::effectiveSelection() const
{
    return withLinks(m_ctx.selection->clips());
}

// ---- Clip factories ----

int64_t TimelineController::defaultStillFrames(Rational fps)
{
    return framesForSeconds(fps, kDefaultStillSeconds);
}

QVector<ops::PlacedClip> TimelineController::makeMediaClips(
    const QString& mediaId, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const
{
    const MediaItem* item = project()->mediaPool()->item(mediaId);
    if (!item)
        return {};
    const TimelineData& data = timeline()->data();
    const media::MediaInfo& info = item->info;
    inUs = std::max<int64_t>(0, inUs);
    int64_t durationUs = outUs > inUs ? outUs - inUs : info.durationUs - inUs;
    int64_t frames = durationUs > 0 ? usToFrames(durationUs, data.fps) : 0;
    if (info.isStillImage || frames < 1)
        frames = defaultStillFrames(data.fps);

    ClipData base;
    base.kind = ClipKind::Media;
    base.mediaId = mediaId;
    base.name = item->name;
    base.duration = frames;
    base.sourceInUs = info.isStillImage ? 0 : inUs;
    base.sourceDurationUs = info.isStillImage ? 0 : info.durationUs;

    QVector<ops::PlacedClip> clips;
    const bool video
        = info.hasVideo && videoTrack >= 0 && videoTrack < int(data.videoTracks.size());
    const bool audio = info.hasAudio && !info.isStillImage && audioTrack >= 0
        && audioTrack < int(data.audioTracks.size());
    if (video)
        clips.append({TrackRef {TrackType::Video, videoTrack}, base});
    if (audio)
        clips.append({TrackRef {TrackType::Audio, audioTrack}, base});
    return clips;
}

std::optional<ClipData> TimelineController::makeGeneratorClip(const QString& effectId, Rational fps)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectDescriptor* desc = registry.find(effectId);
    if (!desc)
        return std::nullopt;
    ClipData clip;
    clip.name = desc->name;
    clip.duration = defaultStillFrames(fps);
    if (desc->category == EffectCategory::Title) {
        clip.kind = ClipKind::Title;
        clip.title = registry.defaultParams(effectId);
    } else if (desc->category == EffectCategory::Generator) {
        clip.kind = ClipKind::Solid;
        const ParamSet params = registry.defaultParams(effectId);
        clip.solidColor
            = params.staticValue(QStringLiteral("color"), QColor(0x30, 0x30, 0x34)).value<QColor>();
    } else {
        return std::nullopt;
    }
    return clip;
}

// ---- Source edits ----

QVector<quint64> TimelineController::insertSource(
    const QString& mediaId, int64_t inUs, int64_t outUs, SourceEditMode mode)
{
    const QString label = mode == SourceEditMode::Insert ? tr("Insert Clip")
        : mode == SourceEditMode::Overwrite              ? tr("Overwrite Clip")
                                                         : tr("Place on Top");
    QVector<quint64> ids;
    int64_t end = 0;
    runEdit(project(), label, [&](TimelineData& data) {
        QVector<ops::PlacedClip> clips
            = makeMediaClips(mediaId, inUs, outUs, m_targetVideo, m_targetAudio);
        if (clips.isEmpty())
            return false;
        const int64_t at = timeline()->playhead();
        for (ops::PlacedClip& p : clips)
            p.clip.start = at;
        end = at + clips.front().clip.duration;
        switch (mode) {
        case SourceEditMode::Insert:
            ids = ops::insertClips(data, clips, ops::RippleScope::AllTracks);
            break;
        case SourceEditMode::Overwrite:
            ids = ops::overwriteClips(data, clips);
            break;
        case SourceEditMode::PlaceOnTop:
            ids = ops::placeOnTop(data, clips);
            break;
        }
        if (ids.size() > 1)
            ops::linkClips(data, ids); // video + audio of one source stay in sync
        return !ids.isEmpty();
    });
    if (!ids.isEmpty()) {
        m_ctx.selection->setClips(ids);
        timeline()->setPlayhead(end);
    }
    return ids;
}

void TimelineController::sourceEdit(SourceEditMode mode)
{
    if (!m_sourceProvider)
        return;
    if (const auto source = m_sourceProvider())
        insertSource(source->mediaId, source->inUs, source->outUs, mode);
}

// ---- Commands ----

void TimelineController::splitAtPlayhead()
{
    const int64_t frame = timeline()->playhead();
    const QVector<quint64> selected = effectiveSelection();
    runEdit(project(), tr("Split Clip"), [&](TimelineData& data) {
        // With a selection only the selected clips are cut, otherwise every unlocked track.
        if (!selected.isEmpty())
            return !ops::splitClips(data, selected, frame).isEmpty();
        return !ops::splitAt(data, frame).isEmpty();
    });
}

void TimelineController::deleteSelection()
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty())
        return;
    runEdit(
        project(), tr("Delete"), [&](TimelineData& data) { return ops::liftClips(data, ids) > 0; });
    m_ctx.selection->clear();
}

void TimelineController::rippleDeleteSelection()
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty()) {
        // Nothing selected: ripple delete the gap under the playhead on the target track.
        const TrackRef ref {TrackType::Video, std::max(0, m_targetVideo)};
        closeGapAt(ref, timeline()->playhead());
        return;
    }
    runEdit(project(), tr("Ripple Delete"),
        [&](TimelineData& data) { return ops::rippleDeleteClips(data, ids) > 0; });
    m_ctx.selection->clear();
}

void TimelineController::closeGapAt(TrackRef ref, int64_t frame)
{
    runEdit(project(), tr("Close Gap"), [&](TimelineData& data) {
        return ops::closeGap(data, ref, frame, ops::RippleScope::AllTracks) > 0;
    });
}

void TimelineController::selectAll()
{
    QVector<quint64> ids;
    const TimelineData& data = timeline()->data();
    for (const auto* list : {&data.videoTracks, &data.audioTracks}) {
        for (const TrackData& t : *list) {
            for (const ClipData& c : t.clips)
                ids.append(c.id);
        }
    }
    m_ctx.selection->setClips(ids);
}

void TimelineController::deselectAll()
{
    m_ctx.selection->clear();
}

void TimelineController::nudge(int64_t frames)
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty())
        return;
    runEdit(project(), tr("Nudge Clip"),
        [&](TimelineData& data) { return ops::moveClips(data, ids, frames, 0); });
}

void TimelineController::setPlayheadClamped(int64_t frame)
{
    timeline()->setPlayhead(std::max<int64_t>(0, frame));
}

void TimelineController::goToPreviousEdit()
{
    if (const auto f = ops::previousEditPoint(timeline()->data(), timeline()->playhead()))
        setPlayheadClamped(*f);
}

void TimelineController::goToNextEdit()
{
    if (const auto f = ops::nextEditPoint(timeline()->data(), timeline()->playhead()))
        setPlayheadClamped(*f);
}

void TimelineController::goToStart()
{
    setPlayheadClamped(0);
}

void TimelineController::goToEnd()
{
    setPlayheadClamped(timeline()->data().duration());
}

void TimelineController::setMarks(int64_t in, int64_t out, const QString& label)
{
    runEdit(project(), label, [&](TimelineData& data) {
        if (data.markIn == in && data.markOut == out)
            return false;
        data.markIn = in;
        data.markOut = out;
        return true;
    });
}

void TimelineController::markIn()
{
    const TimelineData& d = timeline()->data();
    const int64_t in = timeline()->playhead();
    setMarks(in, d.markOut > in ? d.markOut : -1, tr("Mark In"));
}

void TimelineController::markOut()
{
    // The out mark is exclusive: marking out on a frame includes that frame.
    const TimelineData& d = timeline()->data();
    const int64_t out = timeline()->playhead() + 1;
    setMarks(d.markIn >= 0 && d.markIn < out ? d.markIn : -1, out, tr("Mark Out"));
}

void TimelineController::markClip()
{
    const TimelineData& d = timeline()->data();
    const ClipData* clip = ops::clipById(d, m_ctx.selection->currentClip());
    if (!clip) {
        // Clip under the playhead on the target video track, else on any track (V1 first).
        const int64_t f = timeline()->playhead();
        if (const TrackData* t = d.track(TrackRef {TrackType::Video, m_targetVideo}))
            clip = t->clipAt(f);
        for (const auto* list : {&d.videoTracks, &d.audioTracks}) {
            for (size_t i = 0; !clip && i < list->size(); ++i)
                clip = (*list)[i].clipAt(f);
        }
    }
    if (clip)
        setMarks(clip->start, clip->end(), tr("Mark Clip"));
}

void TimelineController::clearIn()
{
    setMarks(-1, timeline()->data().markOut, tr("Clear In"));
}

void TimelineController::clearOut()
{
    setMarks(timeline()->data().markIn, -1, tr("Clear Out"));
}

void TimelineController::clearInOut()
{
    setMarks(-1, -1, tr("Clear In and Out"));
}

void TimelineController::goToIn()
{
    if (timeline()->data().markIn >= 0)
        setPlayheadClamped(timeline()->data().markIn);
}

void TimelineController::goToOut()
{
    // Show the last marked frame (the mark itself is exclusive).
    if (timeline()->data().markOut > 0)
        setPlayheadClamped(timeline()->data().markOut - 1);
}

void TimelineController::addMarker()
{
    const int64_t frame = timeline()->playhead();
    runEdit(project(), tr("Add Marker"), [&](TimelineData& data) {
        auto it = std::lower_bound(data.markers.begin(), data.markers.end(), frame,
            [](const Marker& m, int64_t f) { return m.frame < f; });
        if (it != data.markers.end() && it->frame == frame)
            return false;
        Marker m;
        m.frame = frame;
        m.name = tr("Marker %1").arg(data.markers.size() + 1);
        data.markers.insert(it, m);
        return true;
    });
}

void TimelineController::toggleClipsEnabled()
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty())
        return;
    runEdit(project(), tr("Enable/Disable Clip"), [&](TimelineData& data) {
        // Mixed selection: disable all; all disabled: enable all.
        bool anyEnabled = false;
        for (quint64 id : ids) {
            if (const ClipData* c = ops::clipById(data, id))
                anyEnabled |= c->enabled;
        }
        bool changed = false;
        for (quint64 id : ids) {
            const auto loc = ops::findClip(data, id);
            if (!loc || data.track(loc->track)->locked)
                continue;
            data.track(loc->track)->clips[size_t(loc->clipIndex)].enabled = !anyEnabled;
            changed = true;
        }
        return changed;
    });
}

void TimelineController::linkSelection()
{
    const QVector<quint64> ids = m_ctx.selection->clips();
    runEdit(project(), tr("Link Clips"),
        [&](TimelineData& data) { return ops::linkClips(data, ids) != 0; });
}

void TimelineController::unlinkSelection()
{
    const QVector<quint64> ids = ops::withLinkedClips(timeline()->data(), m_ctx.selection->clips());
    runEdit(project(), tr("Unlink Clips"), [&](TimelineData& data) {
        const TimelineData before = data;
        ops::unlinkClips(data, ids);
        return !(data == before);
    });
}

void TimelineController::copy()
{
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty())
        m_clipboard = ops::copyClips(timeline()->data(), ids);
}

void TimelineController::cut()
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty())
        return;
    m_clipboard = ops::copyClips(timeline()->data(), ids);
    runEdit(
        project(), tr("Cut"), [&](TimelineData& data) { return ops::liftClips(data, ids) > 0; });
    m_ctx.selection->clear();
}

void TimelineController::paste()
{
    if (m_clipboard.isEmpty())
        return;
    const int64_t at = timeline()->playhead();
    QVector<quint64> ids;
    runEdit(project(), tr("Paste"), [&](TimelineData& data) {
        ids = ops::pasteClips(data, m_clipboard, at);
        return !ids.isEmpty();
    });
    if (!ids.isEmpty()) {
        m_ctx.selection->setClips(ids);
        timeline()->setPlayhead(at + m_clipboard.duration);
    }
}

void TimelineController::addDefaultTransition()
{
    const TimelineData& d = timeline()->data();
    const int64_t playhead = timeline()->playhead();

    // Candidate edges: of the selected clips if any, otherwise of every clip on unlocked video
    // tracks. The edge closest to the playhead wins.
    struct Edge
    {
        quint64 clipId = 0;
        bool head = true;
        int64_t distance = std::numeric_limits<int64_t>::max();
    };
    Edge best;
    auto consider = [&](const ClipData& c) {
        const int64_t dh = std::abs(c.start - playhead);
        const int64_t dt = std::abs(c.end() - playhead);
        if (dh < best.distance)
            best = {c.id, true, dh};
        if (dt < best.distance)
            best = {c.id, false, dt};
    };
    const QVector<quint64> selected = m_ctx.selection->clips();
    if (!selected.isEmpty()) {
        for (quint64 id : selected) {
            if (const ClipData* c = ops::clipById(d, id))
                consider(*c);
        }
    } else {
        for (const TrackData& t : d.videoTracks) {
            if (t.locked)
                continue;
            for (const ClipData& c : t.clips)
                consider(c);
        }
    }
    if (best.clipId == 0)
        return;

    runEdit(project(), tr("Add Transition"), [&](TimelineData& data) {
        const auto loc = ops::findClip(data, best.clipId);
        if (!loc || data.track(loc->track)->locked)
            return false;
        TrackData& track = *data.track(loc->track);
        size_t index = size_t(loc->clipIndex);
        bool head = best.head;
        // A tail that touches the next clip is the same cut as that clip's head: store the
        // transition on the right-hand clip (transitionIn, centred on the cut).
        if (!head && index + 1 < track.clips.size()
            && track.clips[index + 1].start == track.clips[index].end()) {
            ++index;
            head = true;
        }
        ClipData& clip = track.clips[index];
        const int64_t frames
            = std::min(framesForSeconds(data.fps, kDefaultTransitionSeconds), clip.duration);
        TransitionData t = EffectRegistry::instance().createTransition(kDefaultTransition, frames);
        (head ? clip.transitionIn : clip.transitionOut) = t;
        return true;
    });
}

// ---- Tracks ----

void TimelineController::addTrack(TrackType type, int index)
{
    runEdit(project(), type == TrackType::Video ? tr("Add Video Track") : tr("Add Audio Track"),
        [&](TimelineData& data) {
            ops::addTrack(data, type, index);
            return true;
        });
}

void TimelineController::removeTrack(TrackRef ref)
{
    const bool removed = runEdit(project(), tr("Delete Track"),
        [&](TimelineData& data) { return ops::removeTrack(data, ref); });
    if (removed) {
        int& target = ref.type == TrackType::Video ? m_targetVideo : m_targetAudio;
        const int count = int(timeline()->data().tracks(ref.type).size());
        if (target >= count)
            target = count - 1;
        emit stateChanged();
    }
}

void TimelineController::renameTrack(TrackRef ref, const QString& name)
{
    runEdit(project(), tr("Rename Track"), [&](TimelineData& data) {
        TrackData* t = data.track(ref);
        if (!t || t->name == name)
            return false;
        t->name = name;
        return true;
    });
}

void TimelineController::toggleTrackFlag(TrackRef ref, int flag)
{
    runEdit(project(), tr("Change Track"), [&](TimelineData& data) {
        TrackData* t = data.track(ref);
        if (!t)
            return false;
        switch (flag) {
        case FlagEnabled:
            t->enabled = !t->enabled;
            break;
        case FlagLocked:
            t->locked = !t->locked;
            break;
        case FlagMuted:
            t->muted = !t->muted;
            break;
        case FlagSolo:
            t->solo = !t->solo;
            break;
        default:
            return false;
        }
        return true;
    });
}

// ---- Actions ----

void TimelineController::connectActions()
{
    ActionRegistry* reg = m_ctx.actions;
    if (!reg)
        return;
    auto on = [this, reg](const char* id, auto slot) {
        if (QAction* a = reg->action(QLatin1String(id)))
            connect(a, &QAction::triggered, this, slot);
    };
    auto toggle = [this, reg](const char* id, auto fn) {
        if (QAction* a = reg->action(QLatin1String(id)))
            connect(a, &QAction::toggled, this, fn);
    };
    toggle("tool.select", [this](bool on) {
        if (on)
            setTool(TimelineTool::Select);
    });
    toggle("tool.trim", [this](bool on) {
        if (on)
            setTool(TimelineTool::Trim);
    });
    toggle("tool.blade", [this](bool on) {
        if (on)
            setTool(TimelineTool::Blade);
    });
    toggle("timeline.snapping", [this](bool on) { setSnapping(on); });
    toggle("timeline.linked_selection", [this](bool on) { setLinkedSelection(on); });
    // Adopt the actions' current state (defaults: snapping and linked selection on).
    m_snapping = reg->action(QStringLiteral("timeline.snapping"))->isChecked();
    m_linked = reg->action(QStringLiteral("timeline.linked_selection"))->isChecked();

    on("trim.nudge_left", [this] { nudge(-1); });
    on("trim.nudge_right", [this] { nudge(1); });
    on("edit.split", &TimelineController::splitAtPlayhead);
    on("edit.delete", &TimelineController::deleteSelection);
    on("edit.ripple_delete", &TimelineController::rippleDeleteSelection);
    on("edit.cut", &TimelineController::cut);
    on("edit.copy", &TimelineController::copy);
    on("edit.paste", &TimelineController::paste);
    on("edit.select_all", &TimelineController::selectAll);
    on("edit.deselect_all", &TimelineController::deselectAll);
    on("edit.insert", [this] { sourceEdit(SourceEditMode::Insert); });
    on("edit.overwrite", [this] { sourceEdit(SourceEditMode::Overwrite); });
    on("edit.place_on_top", [this] { sourceEdit(SourceEditMode::PlaceOnTop); });
    on("timeline.add_transition", &TimelineController::addDefaultTransition);
    on("timeline.add_video_track", [this] { addTrack(TrackType::Video); });
    on("timeline.add_audio_track", [this] { addTrack(TrackType::Audio); });
    on("clip.toggle_enabled", &TimelineController::toggleClipsEnabled);
    on("clip.link", &TimelineController::linkSelection);
    on("clip.unlink", &TimelineController::unlinkSelection);
    on("mark.in", &TimelineController::markIn);
    on("mark.out", &TimelineController::markOut);
    on("mark.clip", &TimelineController::markClip);
    on("mark.clear_in", &TimelineController::clearIn);
    on("mark.clear_out", &TimelineController::clearOut);
    on("mark.clear_both", &TimelineController::clearInOut);
    on("mark.go_to_in", &TimelineController::goToIn);
    on("mark.go_to_out", &TimelineController::goToOut);
    on("mark.add_marker", &TimelineController::addMarker);
    // Pure playhead navigation over timeline edit points; the playback module must not connect
    // these a second time (see src/ui/timeline/readme.md).
    on("transport.prev_edit", &TimelineController::goToPreviousEdit);
    on("transport.next_edit", &TimelineController::goToNextEdit);
    on("transport.go_to_start", &TimelineController::goToStart);
    on("transport.go_to_end", &TimelineController::goToEnd);
}

} // namespace kk
