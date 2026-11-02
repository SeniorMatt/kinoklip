// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 2: delete / ripple delete / ripple cut (clips, gaps, in-out ranges),
// clipboard, selection commands (forward / backward, nearest edit or clip), join, swap, match
// frame, track targeting by number, marker deletion, clip duration and speed.
#include "core/EditBehaviour.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

/// Id of the clip next to @p clipId on its track (@p direction -1 = previous, +1 = next), or 0.
quint64 neighbourOf(const TimelineData& data, quint64 clipId, int direction)
{
    const auto loc = ops::findClip(data, clipId);
    if (!loc)
        return 0;
    const auto& clips = data.track(loc->track)->clips;
    const int i = loc->clipIndex + (direction < 0 ? -1 : 1);
    return i >= 0 && i < int(clips.size()) ? clips[size_t(i)].id : 0;
}

} // namespace

bool TimelineController::hasMarkedRange() const
{
    const TimelineData& d = timeline()->data();
    return d.markIn >= 0 && d.markOut > d.markIn;
}

// ---- Delete, ripple delete, ripple cut ----

void TimelineController::deleteSelection()
{
    if (const auto t = m_ctx.selection->transition()) {
        deleteTransition(*t); // Backspace on a selected transition (spec 13.10)
        return;
    }
    if (m_selectedGap) {
        // A selected gap is removed by either delete key (spec 10.5).
        rippleDeleteSelection();
        return;
    }
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty()) {
        runEdit(project(), tr("Delete"),
            [&](TimelineData& data) { return ops::liftClips(data, ids) > 0; });
        m_ctx.selection->clear();
        return;
    }
    // Nothing selected: the marked range on the auto-select tracks (spec 10.7). The marks
    // stay, they still describe the (now empty) range.
    if (hasMarkedRange()) {
        const TimelineData& d = timeline()->data();
        const int64_t in = d.markIn;
        const int64_t out = d.markOut;
        runEdit(project(), tr("Lift Range"), [&](TimelineData& data) {
            return ops::liftRange(data, in, out, ops::autoSelectTracks());
        });
    }
}

void TimelineController::rippleDeleteSelection()
{
    if (const auto t = m_ctx.selection->transition()) {
        deleteTransition(*t); // a transition has no length of its own to ripple
        return;
    }
    if (const auto gap = m_selectedGap) {
        setSelectedGap(std::nullopt);
        closeGapAt(gap->track, gap->start);
        return;
    }
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty()) {
        // Later clips on the tracks of the deleted clips close up; the other auto-select
        // tracks follow where they have room, keeping sync (spec 10.2-10.4).
        runEdit(project(), tr("Ripple Delete"), [&](TimelineData& data) {
            return ops::rippleDeleteClips(data, ids, ops::RippleScope::AllTracks) > 0;
        });
        m_ctx.selection->clear();
        return;
    }
    if (hasMarkedRange()) {
        // Extract the marked range on the auto-select tracks (spec 10.8).
        const TimelineData& d = timeline()->data();
        const int64_t in = d.markIn;
        const int64_t out = d.markOut;
        const bool changed = runEdit(project(), tr("Ripple Delete Range"), [&](TimelineData& data) {
            if (!ops::extractRange(data, in, out, ops::autoSelectTracks()))
                return false;
            data.markIn = -1; // the range no longer exists
            data.markOut = -1;
            return true;
        });
        if (changed)
            timeline()->setPlayhead(in);
        return;
    }
    // Nothing selected: optionally (EditBehaviour) close the gap under the playhead on the
    // target video track; by default nothing happens (spec 10.5).
    if (EditBehaviour::fromSettings().rippleDeleteGapAtPlayhead) {
        const TrackRef ref {TrackType::Video, std::max(0, m_targetVideo)};
        closeGapAt(ref, timeline()->playhead());
    }
}

void TimelineController::rippleCut()
{
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty()) {
        const ops::Clipboard board = ops::copyClips(timeline()->data(), ids);
        if (runEdit(project(), tr("Ripple Cut"), [&](TimelineData& data) {
                return ops::rippleDeleteClips(data, ids, ops::RippleScope::AllTracks) > 0;
            })) {
            m_clipboard = board;
            m_ctx.selection->clear();
        }
        return;
    }
    if (!hasMarkedRange())
        return;
    const TimelineData& d = timeline()->data();
    const int64_t in = d.markIn;
    const int64_t out = d.markOut;
    const ops::Clipboard board = ops::copyRange(d, in, out, ops::autoSelectTracks());
    if (board.isEmpty())
        return; // nothing to cut (an empty range is not put on the clipboard)
    const bool changed = runEdit(project(), tr("Ripple Cut"), [&](TimelineData& data) {
        if (!ops::extractRange(data, in, out, ops::autoSelectTracks()))
            return false;
        data.markIn = -1;
        data.markOut = -1;
        return true;
    });
    if (changed) {
        m_clipboard = board;
        timeline()->setPlayhead(in);
    }
}

void TimelineController::deleteGaps()
{
    runEdit(project(), tr("Delete Gaps"),
        [&](TimelineData& data) { return ops::deleteGaps(data, ops::autoSelectTracks()) > 0; });
}

// ---- Clipboard ----

void TimelineController::copy()
{
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty()) {
        m_clipboard = ops::copyClips(timeline()->data(), ids);
        return;
    }
    // Nothing selected: the marked range on the auto-select tracks (spec 16.5).
    if (hasMarkedRange()) {
        const TimelineData& d = timeline()->data();
        const ops::Clipboard board
            = ops::copyRange(d, d.markIn, d.markOut, ops::autoSelectTracks());
        if (!board.isEmpty())
            m_clipboard = board;
    }
}

void TimelineController::cut()
{
    const QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty()) {
        m_clipboard = ops::copyClips(timeline()->data(), ids);
        runEdit(project(), tr("Cut"),
            [&](TimelineData& data) { return ops::liftClips(data, ids) > 0; });
        m_ctx.selection->clear();
        return;
    }
    if (!hasMarkedRange())
        return;
    const TimelineData& d = timeline()->data();
    const int64_t in = d.markIn;
    const int64_t out = d.markOut;
    const ops::Clipboard board = ops::copyRange(d, in, out, ops::autoSelectTracks());
    if (board.isEmpty())
        return;
    if (runEdit(project(), tr("Cut"), [&](TimelineData& data) {
            return ops::liftRange(data, in, out, ops::autoSelectTracks());
        }))
        m_clipboard = board;
}

// ---- Linking ----

void TimelineController::linkSelection()
{
    const QVector<quint64> ids = m_ctx.selection->clips();
    if (ids.size() < 1)
        return;
    const TimelineData& d = timeline()->data();
    // Already one group (every selected clip shares a non-zero group): the toggle unlinks.
    quint64 group = 0;
    bool oneGroup = true;
    for (quint64 id : ids) {
        const ClipData* c = ops::clipById(d, id);
        if (!c || c->linkGroup == 0 || (group != 0 && c->linkGroup != group)) {
            oneGroup = false;
            break;
        }
        group = c->linkGroup;
    }
    if (oneGroup) {
        unlinkSelection();
        return;
    }
    runEdit(project(), tr("Link Clips"),
        [&](TimelineData& data) { return ops::linkClips(data, ids) != 0; });
}

// ---- Selection commands ----

void TimelineController::selectClipsFromPlayhead(bool forward, bool allTracks)
{
    const TimelineData& d = timeline()->data();
    const int64_t f = timeline()->playhead();
    const ops::TrackFilter accept = allTracks ? ops::unlockedTracks() : ops::autoSelectTracks();
    QVector<quint64> ids;
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto& list = d.tracks(type);
        for (size_t i = 0; i < list.size(); ++i) {
            if (!accept(TrackRef {type, int(i)}, list[i]))
                continue;
            for (const ClipData& c : list[i].clips) {
                if (forward ? c.start >= f : c.end() <= f)
                    ids.append(c.id);
            }
        }
    }
    m_ctx.selection->setClips(ids);
}

void TimelineController::selectNearestEdit()
{
    const TimelineData& d = timeline()->data();
    const int64_t f = timeline()->playhead();
    const ops::TrackFilter accept = ops::autoSelectTracks();
    // Video tracks first; audio only if no video track has an edit.
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto& list = d.tracks(type);
        int64_t best = std::numeric_limits<int64_t>::max();
        EditPointSelection edit;
        for (size_t t = 0; t < list.size(); ++t) {
            if (!accept(TrackRef {type, int(t)}, list[t]))
                continue;
            const auto& clips = list[t].clips;
            for (size_t i = 0; i < clips.size(); ++i) {
                const ClipData& c = clips[i];
                const bool cutBefore = i > 0 && clips[i - 1].end() == c.start;
                // Head (with the touching tail as a roll), and a free tail.
                const int64_t dh = std::abs(c.start - f);
                if (dh < best) {
                    best = dh;
                    edit = {cutBefore ? clips[i - 1].id : 0, c.id};
                }
                const bool cutAfter = i + 1 < clips.size() && clips[i + 1].start == c.end();
                const int64_t dt = std::abs(c.end() - f);
                if (!cutAfter && dt < best) {
                    best = dt;
                    edit = {c.id, 0};
                }
            }
        }
        if (!edit.isEmpty()) {
            setSelectedEdit(edit);
            return;
        }
    }
}

void TimelineController::selectNearestClipOrGap()
{
    const TimelineData& d = timeline()->data();
    const int64_t f = timeline()->playhead();
    if (const ClipData* c = ops::clipUnderFrame(d, f, ops::autoSelectTracks())) {
        setSelectedEdit({});
        setSelectedGap(std::nullopt);
        m_ctx.selection->setClips({c->id});
        return;
    }
    selectGapAt(TrackRef {TrackType::Video, std::max(0, m_targetVideo)}, f);
}

// ---- Join, swap ----

void TimelineController::joinSelection()
{
    // Pairs (left, right) to join.
    QVector<std::pair<quint64, quint64>> pairs;
    const TimelineData& d = timeline()->data();
    if (m_selectedEdit.tailClip && m_selectedEdit.headClip) {
        pairs.append({m_selectedEdit.tailClip, m_selectedEdit.headClip});
        if (m_linked) {
            // Linked parts cut at the same frame join along.
            for (quint64 l : ops::linkedClips(d, m_selectedEdit.tailClip)) {
                const quint64 r = neighbourOf(d, l, +1);
                if (l != m_selectedEdit.tailClip && r
                    && ops::linkedClips(d, m_selectedEdit.headClip).contains(r))
                    pairs.append({l, r});
            }
        }
    } else {
        const QVector<quint64> ids = effectiveSelection();
        for (quint64 id : ids) {
            const quint64 r = neighbourOf(d, id, +1);
            if (r && ids.contains(r))
                pairs.append({id, r});
        }
    }
    if (pairs.isEmpty())
        return;
    // Right to left, so a chain a|b|c joins into a: joining (b, c) first keeps the pair
    // (a, b) valid.
    std::sort(pairs.begin(), pairs.end(), [&d](const auto& x, const auto& y) {
        return ops::clipById(d, x.first)->start > ops::clipById(d, y.first)->start;
    });
    const bool changed = runEdit(project(), tr("Join Clips"), [&](TimelineData& data) {
        bool any = false;
        for (const auto& [left, right] : std::as_const(pairs))
            any |= ops::joinClips(data, left, right);
        return any;
    });
    if (changed)
        setSelectedEdit({});
}

void TimelineController::swapSelection(int direction)
{
    const quint64 primary = m_ctx.selection->currentClip();
    if (!primary)
        return;
    const TimelineData& d = timeline()->data();
    const quint64 neighbour = neighbourOf(d, primary, direction);
    if (!neighbour)
        return;
    // Linked parts swap along when their neighbour is linked to the primary's neighbour.
    QVector<quint64> partners;
    if (m_linked) {
        const QVector<quint64> neighbourLinks = ops::linkedClips(d, neighbour);
        for (quint64 id : ops::linkedClips(d, primary)) {
            if (id != primary && neighbourLinks.contains(neighbourOf(d, id, direction)))
                partners.append(id);
        }
    }
    runEdit(project(), tr("Swap Clips"), [&](TimelineData& data) {
        if (!ops::swapWithNeighbour(data, primary, direction))
            return false;
        for (quint64 id : std::as_const(partners))
            ops::swapWithNeighbour(data, id, direction);
        return true;
    });
}

// ---- Match frame, targeting, markers ----

void TimelineController::matchFrame()
{
    if (!m_matchFrame)
        return;
    const TimelineData& d = timeline()->data();
    const int64_t f = timeline()->playhead();
    const ClipData* clip = nullptr;
    for (quint64 id : m_ctx.selection->clips()) {
        const ClipData* c = ops::clipById(d, id);
        if (c && c->contains(f) && c->kind == ClipKind::Media) {
            clip = c;
            break;
        }
    }
    if (!clip) {
        if (const TrackData* t = d.track(TrackRef {TrackType::Video, m_targetVideo}))
            clip = t->clipAt(f);
    }
    if (!clip || clip->kind != ClipKind::Media) {
        clip = ops::clipUnderFrame(d, f, ops::autoSelectTracks());
    }
    if (!clip || clip->kind != ClipKind::Media || clip->mediaId.isEmpty())
        return;
    m_matchFrame(clip->mediaId, clip->sourceTimeUsAt(f, d.fps), clip->sourceInUs,
        clip->sourceTimeUsAt(clip->end(), d.fps));
}

void TimelineController::setTargetByNumber(TrackType type, int number)
{
    const int index = number - 1;
    if (index >= 0 && index < int(timeline()->data().tracks(type).size()))
        setTargetTrack(type, index);
}

void TimelineController::toggleAutoSelectByNumber(TrackType type, int number)
{
    const int index = number - 1;
    if (index >= 0 && index < int(timeline()->data().tracks(type).size()))
        toggleTrackFlag(TrackRef {type, index}, FlagAutoSelect);
}

void TimelineController::deleteMarkerAtPlayhead()
{
    const int64_t f = timeline()->playhead();
    runEdit(project(), tr("Delete Marker"), [&](TimelineData& data) {
        const auto before = data.markers.size();
        data.markers.removeIf([f](const Marker& m) { return m.frame == f; });
        return data.markers.size() != before;
    });
}

// ---- Duration and speed ----

bool TimelineController::setSelectionDuration(int64_t frames)
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty() || frames < 1)
        return false;
    return runEdit(project(), tr("Change Clip Duration"), [&](TimelineData& data) {
        bool any = false;
        for (quint64 id : ids) {
            const ClipData* c = ops::clipById(data, id);
            if (c)
                any |= ops::trimTail(data, id, frames - c->duration, true) != 0;
        }
        return any;
    });
}

bool TimelineController::setSelectionSpeed(double speed)
{
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty() || !(speed > 0.0))
        return false;
    return runEdit(project(), tr("Change Clip Speed"), [&](TimelineData& data) {
        bool any = false;
        for (quint64 id : ids)
            any |= timelineedit::applyClipSpeed(data, id, speed);
        return any;
    });
}

} // namespace kk
