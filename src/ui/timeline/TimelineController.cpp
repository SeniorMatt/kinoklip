// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController implementation: one undoable edit per command, action wiring.
#include "ui/timeline/TimelineController.h"

#include "core/CompoundOps.h"
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QAction>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace ops = editops;

namespace {

using timelineedit::runEdit;

/// Clips linked to @p clipId whose same edge (head or tail) sits on the same frame; they
/// trim along with it (spec 8.12). Includes @p clipId itself first.
QVector<quint64> sameEdgeLinks(const TimelineData& data, quint64 clipId, bool head, bool linked)
{
    QVector<quint64> ids {clipId};
    const ClipData* clip = ops::clipById(data, clipId);
    if (!clip || !linked)
        return ids;
    const int64_t edge = head ? clip->start : clip->end();
    for (quint64 id : ops::linkedClips(data, clipId)) {
        const ClipData* other = ops::clipById(data, id);
        if (id != clipId && other && (head ? other->start : other->end()) == edge)
            ids.append(id);
    }
    return ids;
}

} // namespace

TimelineController::TimelineController(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
{
    if (m_ctx.selection)
        connect(m_ctx.selection, &SelectionModel::changed, this,
            &TimelineController::onClipSelectionChanged);
}

void TimelineController::onClipSelectionChanged()
{
    // Clips, an edit point and a gap are mutually exclusive selections (spec 7.6, 10.5).
    if (m_ctx.selection->clips().isEmpty())
        return;
    if (m_selectedEdit.isEmpty() && !m_selectedGap)
        return;
    m_selectedEdit = {};
    m_selectedGap.reset();
    emit stateChanged();
}

void TimelineController::setSelectedEdit(const EditPointSelection& edit)
{
    if (!edit.isEmpty()) {
        m_selectedGap.reset();
        m_ctx.selection->clear();
    }
    if (m_selectedEdit == edit)
        return;
    m_selectedEdit = edit;
    emit stateChanged();
}

void TimelineController::setSelectedGap(const std::optional<GapSelection>& gap)
{
    if (gap) {
        m_selectedEdit = {};
        m_ctx.selection->clear();
    }
    if (m_selectedGap == gap)
        return;
    m_selectedGap = gap;
    emit stateChanged();
}

bool TimelineController::selectGapAt(TrackRef ref, int64_t frame)
{
    const auto gap = ops::gapAt(timeline()->data(), ref, frame);
    if (!gap) {
        setSelectedGap(std::nullopt);
        return false;
    }
    setSelectedGap(GapSelection {ref, gap->first, gap->second});
    return true;
}

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
    if (type == TrackType::Subtitle)
        return; // subtitle tracks are not source destinations
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
    if (ref.type == TrackType::Subtitle)
        return;
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
    return EditBehaviour::frames(EditBehaviour::fromSettings().stillSeconds, fps);
}

QVector<ops::PlacedClip> TimelineController::makeMediaClips(
    const QString& mediaId, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const
{
    const MediaItem* item = project()->mediaPool()->item(mediaId);
    if (!item)
        return {};
    const TimelineData& data = timeline()->data();
    if (item->isTimeline())
        return makeNestedClips(*item, inUs, outUs, videoTrack, audioTrack);
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

QVector<ops::PlacedClip> TimelineController::makeNestedClips(
    const MediaItem& item, int64_t inUs, int64_t outUs, int videoTrack, int audioTrack) const
{
    // A timeline / compound clip from the media pool (spec 26.x): one Nested clip per track
    // type the timeline has content of, linked. A timeline can never contain itself.
    const auto nested = project()->timelineData(item.timelineUid);
    if (!nested || !project()->canNest(item.timelineUid, project()->activeTimelineUid()))
        return {};
    const TimelineData& data = timeline()->data();
    ClipData base = compound::makeNestedClip(*nested, item.id);
    base.name = item.name;
    const int64_t lengthUs = compound::nestedDurationUs(*nested);
    inUs = std::clamp<int64_t>(inUs, 0, lengthUs);
    const int64_t endUs = outUs > inUs ? std::min(outUs, lengthUs) : lengthUs;
    base.sourceInUs = inUs;
    base.duration = std::max<int64_t>(1, usToFrames(endUs - inUs, data.fps));
    QVector<ops::PlacedClip> clips;
    if (item.info.hasVideo && videoTrack >= 0 && videoTrack < int(data.videoTracks.size()))
        clips.append({TrackRef {TrackType::Video, videoTrack}, base});
    if (item.info.hasAudio && audioTrack >= 0 && audioTrack < int(data.audioTracks.size()))
        clips.append({TrackRef {TrackType::Audio, audioTrack}, base});
    return clips;
}

std::optional<ClipData> TimelineController::makeGeneratorClip(const QString& effectId, Rational fps)
{
    return effectactions::makeGeneratorClip(effectId, fps);
}

// ---- Source edits ----

QVector<quint64> TimelineController::insertSource(
    const QString& mediaId, int64_t inUs, int64_t outUs, SourceEditMode mode)
{
    QString label;
    switch (mode) {
    case SourceEditMode::Insert:
        label = tr("Insert Clip");
        break;
    case SourceEditMode::Overwrite:
        label = tr("Overwrite Clip");
        break;
    case SourceEditMode::PlaceOnTop:
        label = tr("Place on Top");
        break;
    case SourceEditMode::Replace:
        label = tr("Replace Clip");
        break;
    case SourceEditMode::FitToFill:
        label = tr("Fit to Fill");
        break;
    case SourceEditMode::AppendAtEnd:
        label = tr("Append at End");
        break;
    case SourceEditMode::RippleOverwrite:
        label = tr("Ripple Overwrite");
        break;
    }
    const EditBehaviour behaviour = EditBehaviour::fromSettings();
    const TimelineData& before = timeline()->data();
    const int64_t playhead = timeline()->playhead();
    const bool marksUsed = behaviour.threePointEditing;
    const int64_t tlIn = marksUsed ? before.markIn : -1;
    const int64_t tlOut
        = marksUsed && before.markOut > std::max<int64_t>(tlIn, 0) ? before.markOut : -1;

    // Destination (spec 4.2-4.9). `at` = first frame; `fixedFrames` > 0 forces the length.
    int64_t at = playhead;
    int64_t fixedFrames = 0;
    bool backtime = false; // only an out mark: the clip ends at it
    double speed = 1.0;
    // Ripple overwrite (spec 4.11): the range that is replaced.
    int64_t replaceStart = 0;
    int64_t replaceEnd = 0;
    switch (mode) {
    case SourceEditMode::RippleOverwrite: {
        // The timeline in..out range, else the clip under the playhead (target video track
        // first); the source keeps its own length and the timeline ripples by the difference.
        if (tlIn >= 0 && tlOut > tlIn) {
            replaceStart = tlIn;
            replaceEnd = tlOut;
        } else {
            const ClipData* target = nullptr;
            if (const TrackData* t = before.track(TrackRef {TrackType::Video, m_targetVideo}))
                target = t->clipAt(playhead);
            if (!target)
                target = ops::clipUnderFrame(before, playhead, ops::autoSelectTracks());
            if (!target)
                return {};
            replaceStart = target->start;
            replaceEnd = target->end();
        }
        at = replaceStart;
        break;
    }
    case SourceEditMode::Replace: {
        // The clip under the playhead on the target video track (else any auto-select track)
        // keeps its place and length; the source fills it from its in point.
        const ClipData* target = nullptr;
        if (const TrackData* t = before.track(TrackRef {TrackType::Video, m_targetVideo}))
            target = t->clipAt(playhead);
        if (!target)
            target = ops::clipUnderFrame(before, playhead, ops::autoSelectTracks());
        if (!target)
            return {};
        at = target->start;
        fixedFrames = target->duration;
        break;
    }
    case SourceEditMode::AppendAtEnd:
        at = before.duration();
        break;
    case SourceEditMode::FitToFill:
        if (tlIn < 0 || tlOut < 0 || outUs <= inUs)
            return {}; // needs both timeline marks and a marked source range
        at = tlIn;
        fixedFrames = tlOut - tlIn;
        speed = double(outUs - inUs) / double(framesToUs(fixedFrames, before.fps));
        break;
    default:
        if (tlIn >= 0) {
            at = tlIn;
            if (tlOut > tlIn)
                fixedFrames = tlOut - tlIn; // timeline range wins over the source out
        } else if (tlOut > 0) {
            backtime = true;
        }
        break;
    }

    QVector<quint64> ids;
    int64_t end = 0;
    runEdit(project(), label, [&](TimelineData& data) {
        // With a forced length the source runs to its end and is cut to the range below.
        QVector<ops::PlacedClip> clips = makeMediaClips(mediaId, inUs,
            fixedFrames > 0 && mode != SourceEditMode::FitToFill ? 0 : outUs, m_targetVideo,
            m_targetAudio);
        if (clips.isEmpty())
            return false;
        int64_t frames = clips.front().clip.duration;
        if (mode == SourceEditMode::FitToFill)
            frames = fixedFrames;
        else if (fixedFrames > 0)
            frames = std::min(frames, fixedFrames); // not enough media: shorter clip
        if (clips.front().clip.sourceDurationUs == 0 && fixedFrames > 0)
            frames = fixedFrames; // stills fill any range
        int64_t start = at;
        if (backtime)
            start = std::max<int64_t>(0, tlOut - frames);
        for (ops::PlacedClip& p : clips) {
            p.clip.start = start;
            p.clip.duration = frames;
            p.clip.speed = speed;
        }
        end = start + frames;
        switch (mode) {
        case SourceEditMode::Insert:
            ids = ops::insertClips(data, clips, ops::RippleScope::AllTracks);
            break;
        case SourceEditMode::Overwrite:
        case SourceEditMode::Replace:
        case SourceEditMode::FitToFill:
        case SourceEditMode::AppendAtEnd:
            ids = ops::overwriteClips(data, clips);
            break;
        case SourceEditMode::PlaceOnTop:
            ids = ops::placeOnTop(data, clips);
            break;
        case SourceEditMode::RippleOverwrite:
            ids = ops::rippleOverwriteClips(data, clips, replaceStart, replaceEnd);
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
        // With a selection only the selected clips are cut, otherwise every track whose auto
        // track selector is on (spec 9.2).
        if (!selected.isEmpty())
            return !ops::splitClips(data, selected, frame).isEmpty();
        return !ops::splitAt(data, frame, ops::autoSelectTracks()).isEmpty();
    });
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
    if (!m_selectedEdit.isEmpty()) {
        trimSelectedEdit(frames);
        return;
    }
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty())
        return;
    runEdit(project(), tr("Nudge Clip"),
        [&](TimelineData& data) { return ops::moveClips(data, ids, frames, 0); });
}

int64_t TimelineController::applyEditTrim(
    TimelineData& data, const EditPointSelection& edit, int64_t frames, bool ripple) const
{
    if (edit.isEmpty() || frames == 0)
        return 0;
    if (edit.tailClip && edit.headClip) {
        const int64_t applied = ops::rollEdit(data, edit.tailClip, edit.headClip, frames);
        if (applied != 0 && m_linked) {
            // Linked pairs cut at the same frame roll along (spec 8.12).
            const QVector<quint64> lefts = ops::linkedClips(data, edit.tailClip);
            const QVector<quint64> rights = ops::linkedClips(data, edit.headClip);
            for (quint64 l : lefts) {
                if (l == edit.tailClip)
                    continue;
                const ClipData* lc = ops::clipById(data, l);
                for (quint64 r : rights) {
                    const ClipData* rc = ops::clipById(data, r);
                    // rollEdit() itself rejects pairs that do not touch on one track.
                    if (r != edit.headClip && lc && rc && lc->end() == rc->start)
                        ops::rollEdit(data, l, r, applied);
                }
            }
        }
        return applied;
    }
    const bool head = edit.headClip != 0;
    const quint64 primary = head ? edit.headClip : edit.tailClip;
    const QVector<quint64> ids = sameEdgeLinks(data, primary, head, m_linked);
    const int64_t applied = head ? ops::trimHead(data, primary, frames, ripple)
                                 : ops::trimTail(data, primary, frames, ripple);
    for (int i = 1; i < ids.size(); ++i) {
        if (head)
            ops::trimHead(data, ids[i], applied, ripple);
        else
            ops::trimTail(data, ids[i], applied, ripple);
    }
    return applied;
}

int64_t TimelineController::trimSelectedEdit(int64_t frames)
{
    const EditPointSelection edit = m_selectedEdit;
    if (edit.isEmpty() || frames == 0)
        return 0;
    const bool ripple = m_tool == TimelineTool::Trim;
    const bool roll = edit.tailClip && edit.headClip;
    int64_t applied = 0;
    runEdit(project(),
        roll         ? tr("Roll Edit")
            : ripple ? tr("Ripple Trim")
                     : tr("Trim Clip"),
        [&](TimelineData& data) {
            applied = applyEditTrim(data, edit, frames, ripple);
            return applied != 0;
        });
    return applied;
}

void TimelineController::extendEditToPlayhead()
{
    const TimelineData& d = timeline()->data();
    const ClipData* clip = ops::clipById(
        d, m_selectedEdit.headClip ? m_selectedEdit.headClip : m_selectedEdit.tailClip);
    if (!clip)
        return;
    const int64_t edge = m_selectedEdit.headClip ? clip->start : clip->end();
    trimSelectedEdit(timeline()->playhead() - edge);
}

void TimelineController::cycleEditPoint()
{
    const TimelineData& d = timeline()->data();
    const EditPointSelection e = m_selectedEdit;
    if (e.isEmpty())
        return;
    // The clips on both sides of the cut, if they touch.
    quint64 left = e.tailClip;
    quint64 right = e.headClip;
    const quint64 known = left ? left : right;
    const auto loc = ops::findClip(d, known);
    if (!loc)
        return;
    const auto& clips = d.track(loc->track)->clips;
    const size_t i = size_t(loc->clipIndex);
    if (!right && i + 1 < clips.size() && clips[i + 1].start == clips[i].end())
        right = clips[i + 1].id;
    if (!left && i > 0 && clips[i - 1].end() == clips[i].start)
        left = clips[i - 1].id;
    if (!left || !right)
        return; // a free edge has only one side
    // tail -> roll -> head -> tail
    if (e.tailClip && !e.headClip)
        setSelectedEdit({left, right});
    else if (e.tailClip && e.headClip)
        setSelectedEdit({0, right});
    else
        setSelectedEdit({left, 0});
}

void TimelineController::soloAutoSelect(TrackRef ref)
{
    runEdit(project(), tr("Change Track"), [&](TimelineData& data) {
        auto& list = data.tracks(ref.type);
        if (ref.index < 0 || ref.index >= int(list.size()))
            return false;
        bool changed = false;
        for (size_t i = 0; i < list.size(); ++i) {
            const bool on = int(i) == ref.index;
            changed |= list[i].autoSelect != on;
            list[i].autoSelect = on;
        }
        return changed;
    });
}

void TimelineController::trimToPlayhead(bool head, bool ripple)
{
    const int64_t f = timeline()->playhead();
    const TimelineData& d = timeline()->data();
    // Clips strictly around the playhead: from the selection, else on auto-select tracks.
    QVector<quint64> ids;
    auto around = [f](const ClipData& c) { return f > c.start && f < c.end(); };
    for (quint64 id : effectiveSelection()) {
        if (const ClipData* c = ops::clipById(d, id); c && around(*c))
            ids.append(id);
    }
    if (m_ctx.selection->clips().isEmpty()) {
        const ops::TrackFilter accept = ops::autoSelectTracks();
        for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
            const auto& list = d.tracks(type);
            for (size_t i = 0; i < list.size(); ++i) {
                if (!accept(TrackRef {type, int(i)}, list[i]))
                    continue;
                if (const ClipData* c = list[i].clipAt(f); c && around(*c))
                    ids.append(c->id);
            }
        }
    }
    if (ids.isEmpty())
        return;
    int64_t newPlayhead = f;
    const bool changed = runEdit(project(),
        ripple ? (head ? tr("Ripple Trim Start") : tr("Ripple Trim End"))
               : (head ? tr("Trim Start") : tr("Trim End")),
        [&](TimelineData& data) {
            bool any = false;
            for (quint64 id : ids) {
                const ClipData* c = ops::clipById(data, id);
                if (!c)
                    continue;
                if (head) {
                    // The playhead frame becomes the first frame of the clip.
                    const int64_t start = c->start;
                    any |= ops::trimHead(data, id, f - start, ripple) != 0;
                    if (ripple)
                        newPlayhead = start; // the same picture is now at the old start
                } else {
                    // The playhead frame is the first frame removed.
                    any |= ops::trimTail(data, id, f - c->end(), ripple) != 0;
                }
            }
            return any;
        });
    if (changed && newPlayhead != f)
        timeline()->setPlayhead(newPlayhead);
}

void TimelineController::trimStartToPlayhead()
{
    trimToPlayhead(true, false);
}

void TimelineController::trimEndToPlayhead()
{
    trimToPlayhead(false, false);
}

void TimelineController::rippleTrimStartToPlayhead()
{
    trimToPlayhead(true, true);
}

void TimelineController::rippleTrimEndToPlayhead()
{
    trimToPlayhead(false, true);
}

void TimelineController::setPlayheadClamped(int64_t frame)
{
    timeline()->setPlayhead(std::max<int64_t>(0, frame));
}

void TimelineController::goToPreviousEdit()
{
    // Only tracks with the auto track selector on count (spec 2.3).
    if (const auto f = ops::previousEditPoint(
            timeline()->data(), timeline()->playhead(), ops::autoSelectTracks()))
        setPlayheadClamped(*f);
}

void TimelineController::goToNextEdit()
{
    if (const auto f
        = ops::nextEditPoint(timeline()->data(), timeline()->playhead(), ops::autoSelectTracks()))
        setPlayheadClamped(*f);
}

void TimelineController::goToPreviousMarker()
{
    if (const auto f = ops::previousMarker(timeline()->data(), timeline()->playhead()))
        setPlayheadClamped(*f);
}

void TimelineController::goToNextMarker()
{
    if (const auto f = ops::nextMarker(timeline()->data(), timeline()->playhead()))
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
        // Clip under the playhead on the auto-select tracks, lowest video track first
        // (spec 3.5).
        clip = ops::clipUnderFrame(d, timeline()->playhead(), ops::autoSelectTracks());
    }
    if (clip)
        setMarks(clip->start, clip->end(), tr("Mark Clip"));
}

void TimelineController::markSelection()
{
    const TimelineData& d = timeline()->data();
    int64_t in = std::numeric_limits<int64_t>::max();
    int64_t out = -1;
    for (quint64 id : effectiveSelection()) {
        if (const ClipData* c = ops::clipById(d, id)) {
            in = std::min(in, c->start);
            out = std::max(out, c->end());
        }
    }
    if (out > 0)
        setMarks(in, out, tr("Mark Selection"));
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

void TimelineController::unlinkSelection()
{
    const QVector<quint64> ids = ops::withLinkedClips(timeline()->data(), m_ctx.selection->clips());
    runEdit(project(), tr("Unlink Clips"), [&](TimelineData& data) {
        const TimelineData before = data;
        ops::unlinkClips(data, ids);
        return !(data == before);
    });
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

void TimelineController::pasteInsert()
{
    if (m_clipboard.isEmpty())
        return;
    const int64_t at = timeline()->playhead();
    QVector<quint64> ids;
    runEdit(project(), tr("Paste Insert"), [&](TimelineData& data) {
        ids = ops::pasteClips(data, m_clipboard, at, -1, -1, true);
        return !ids.isEmpty();
    });
    if (!ids.isEmpty()) {
        m_ctx.selection->setClips(ids);
        timeline()->setPlayhead(at + m_clipboard.duration);
    }
}

// addDefaultTransition() lives in TimelineTransitions.cpp.

// ---- Tracks ----

void TimelineController::addTrack(TrackType type, int index)
{
    const QString label = type == TrackType::Video ? tr("Add Video Track")
        : type == TrackType::Audio                 ? tr("Add Audio Track")
                                                   : tr("Add Subtitle Track");
    runEdit(project(), label, [&](TimelineData& data) {
        ops::addTrack(data, type, index);
        return true;
    });
}

quint64 TimelineController::addSubtitleAtPlayhead()
{
    constexpr double kCaptionSeconds = 3.0; ///< default length of a new caption
    const TimelineData& before = timeline()->data();
    const int64_t at = timeline()->playhead();
    // Preferred track: the one holding the current caption selection, else ST1 upwards; the
    // first one that is unlocked and free at the playhead gets the caption (a new subtitle
    // track when none is).
    int preferred = 0;
    if (const auto loc = ops::findClip(before, m_ctx.selection->currentClip());
        loc && loc->track.type == TrackType::Subtitle)
        preferred = loc->track.index;
    int target = -1;
    const int count = int(before.subtitleTracks.size());
    for (int k = 0; k < count && target < 0; ++k) {
        const int i = (preferred + k) % count;
        const TrackData& t = before.subtitleTracks[size_t(i)];
        if (!t.locked && !t.clipAt(at))
            target = i;
    }
    quint64 id = 0;
    runEdit(project(), tr("Add Subtitle"), [&](TimelineData& data) {
        if (target < 0)
            target = ops::addTrack(data, TrackType::Subtitle);
        const TrackData& t = data.subtitleTracks[size_t(target)];
        // Default length, shortened to the gap before the next caption.
        int64_t length = EditBehaviour::frames(kCaptionSeconds, data.fps);
        for (const ClipData& c : t.clips) {
            if (c.start > at) {
                length = std::min(length, c.start - at);
                break;
            }
        }
        id = ops::overwriteClip(data, TrackRef {TrackType::Subtitle, target},
            subtitles::makeCaption(tr("Subtitle"), at, length));
        return id != 0;
    });
    if (id)
        m_ctx.selection->selectClip(id);
    return id;
}

void TimelineController::removeTrack(TrackRef ref)
{
    const bool removed = runEdit(project(), tr("Delete Track"),
        [&](TimelineData& data) { return ops::removeTrack(data, ref); });
    if (removed && ref.type != TrackType::Subtitle) {
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
        case FlagAutoSelect:
            t->autoSelect = !t->autoSelect;
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
    on("trim.nudge_left_multi", [this] { nudge(-EditBehaviour::fromSettings().multiFrameNudge); });
    on("trim.nudge_right_multi", [this] { nudge(EditBehaviour::fromSettings().multiFrameNudge); });
    on("trim.extend_edit", &TimelineController::extendEditToPlayhead);
    on("trim.cycle_edit", &TimelineController::cycleEditPoint);
    on("trim.trim_start", &TimelineController::trimStartToPlayhead);
    on("trim.trim_end", &TimelineController::trimEndToPlayhead);
    on("trim.ripple_trim_start", &TimelineController::rippleTrimStartToPlayhead);
    on("trim.ripple_trim_end", &TimelineController::rippleTrimEndToPlayhead);
    on("trim.select_nearest_edit", &TimelineController::selectNearestEdit);
    on("trim.select_nearest_clip", &TimelineController::selectNearestClipOrGap);
    on("trim.swap_left", [this] { swapSelection(-1); });
    on("trim.swap_right", [this] { swapSelection(1); });
    on("edit.ripple_cut", &TimelineController::rippleCut);
    on("edit.delete_gaps", &TimelineController::deleteGaps);
    on("edit.join", &TimelineController::joinSelection);
    on("clip.retime_controls", &TimelineController::toggleRetimeControls);
    on("clip.freeze_frame", &TimelineController::freezeFrame);
    on("clip.retime_curve", &TimelineController::showRetimeCurve);
    on("edit.select_forward", [this] { selectClipsFromPlayhead(true, false); });
    on("edit.select_backward", [this] { selectClipsFromPlayhead(false, false); });
    on("edit.select_forward_all", [this] { selectClipsFromPlayhead(true, true); });
    on("edit.select_backward_all", [this] { selectClipsFromPlayhead(false, true); });
    on("clip.match_frame", &TimelineController::matchFrame);
    on("mark.delete_marker", &TimelineController::deleteMarkerAtPlayhead);
    for (int n = 1; reg->contains(QStringLiteral("timeline.target_video_%1").arg(n)); ++n) {
        const QString num = QString::number(n);
        auto bind = [&](const QString& id, auto fn) {
            if (reg->contains(id))
                connect(reg->action(id), &QAction::triggered, this, fn);
        };
        bind(QStringLiteral("timeline.target_video_") + num,
            [this, n] { setTargetByNumber(TrackType::Video, n); });
        bind(QStringLiteral("timeline.target_audio_") + num,
            [this, n] { setTargetByNumber(TrackType::Audio, n); });
        bind(QStringLiteral("timeline.auto_select_video_") + num,
            [this, n] { toggleAutoSelectByNumber(TrackType::Video, n); });
        bind(QStringLiteral("timeline.auto_select_audio_") + num,
            [this, n] { toggleAutoSelectByNumber(TrackType::Audio, n); });
    }
    on("edit.split", &TimelineController::splitAtPlayhead);
    on("edit.delete", &TimelineController::deleteSelection);
    on("edit.ripple_delete", &TimelineController::rippleDeleteSelection);
    on("edit.cut", &TimelineController::cut);
    on("edit.copy", &TimelineController::copy);
    on("edit.paste", &TimelineController::paste);
    on("edit.paste_insert", &TimelineController::pasteInsert);
    on("edit.select_all", &TimelineController::selectAll);
    on("edit.deselect_all", &TimelineController::deselectAll);
    on("edit.insert", [this] { sourceEdit(SourceEditMode::Insert); });
    on("edit.overwrite", [this] { sourceEdit(SourceEditMode::Overwrite); });
    on("edit.ripple_overwrite", [this] { sourceEdit(SourceEditMode::RippleOverwrite); });
    on("edit.place_on_top", [this] { sourceEdit(SourceEditMode::PlaceOnTop); });
    on("edit.replace", [this] { sourceEdit(SourceEditMode::Replace); });
    on("edit.fit_to_fill", [this] { sourceEdit(SourceEditMode::FitToFill); });
    on("edit.append_at_end", [this] { sourceEdit(SourceEditMode::AppendAtEnd); });
    on("timeline.add_transition", &TimelineController::addDefaultTransition);
    on("timeline.add_video_track", [this] { addTrack(TrackType::Video); });
    on("timeline.add_audio_track", [this] { addTrack(TrackType::Audio); });
    on("timeline.add_subtitle_track", [this] { addTrack(TrackType::Subtitle); });
    on("timeline.add_subtitle", &TimelineController::addSubtitleAtPlayhead);
    on("clip.toggle_enabled", &TimelineController::toggleClipsEnabled);
    on("clip.link", &TimelineController::linkSelection);
    on("clip.unlink", &TimelineController::unlinkSelection);
    on("mark.in", &TimelineController::markIn);
    on("mark.out", &TimelineController::markOut);
    on("mark.clip", &TimelineController::markClip);
    on("mark.selection", &TimelineController::markSelection);
    on("mark.clear_in", &TimelineController::clearIn);
    on("mark.clear_out", &TimelineController::clearOut);
    on("mark.clear_both", &TimelineController::clearInOut);
    on("mark.go_to_in", &TimelineController::goToIn);
    on("mark.go_to_out", &TimelineController::goToOut);
    on("mark.add_marker", &TimelineController::addMarkerAtPlayhead);
    on("mark.modify_marker", &TimelineController::addAndModifyMarker);
    on("edit.paste_attributes", &TimelineController::pasteAttributesInteractive);
    toggle("trim.dynamic", [this](bool on) { setDynamicTrim(on); });
    // Dynamic trim follows the playhead while the program engine plays (spec 8.17).
    connect(timeline(), &Timeline::playheadChanged, this, &TimelineController::onPlayheadMoved);
    m_dynamicLast = timeline()->playhead();
    if (m_ctx.playback) {
        connect(m_ctx.playback, &PlaybackEngine::rateChanged, this, [this](double rate) {
            if (rate == 0.0)
                playbackStopped();
        });
    }
    // Pure playhead navigation over timeline edit points; the playback module must not connect
    // these a second time (see src/ui/timeline/readme.md).
    on("transport.prev_edit", &TimelineController::goToPreviousEdit);
    on("transport.next_edit", &TimelineController::goToNextEdit);
    on("transport.go_to_start", &TimelineController::goToStart);
    on("transport.go_to_end", &TimelineController::goToEnd);
    on("transport.prev_marker", &TimelineController::goToPreviousMarker);
    on("transport.next_marker", &TimelineController::goToNextMarker);
    // Timelines and compound clips (TimelineCompound.cpp).
    on("clip.new_compound", [this] { newCompoundClip(); });
    on("clip.decompose", &TimelineController::decomposeSelection);
    on("clip.open_in_timeline", &TimelineController::openSelectedInTimeline);
    on("timeline.new", [this] { newTimeline(); });
    on("timeline.duplicate", &TimelineController::duplicateActiveTimeline);
    on("timeline.delete", &TimelineController::deleteActiveTimeline);
    on("timeline.settings", [this] {
        if (m_timelineSettingsPrompt)
            m_timelineSettingsPrompt(project()->activeTimelineUid());
    });
    on("timeline.next_tab", [this] { cycleTab(1); });
    on("timeline.prev_tab", [this] { cycleTab(-1); });
    on("timeline.close_tab", &TimelineController::closeActiveTab);
}

} // namespace kk
