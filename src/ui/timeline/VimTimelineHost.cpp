// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/timeline/VimTimelineHost.h"

#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "core/TimelineData.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>

namespace kk {

namespace ops = editops;

VimTimelineHost::VimTimelineHost(AppContext& ctx, TimelineController* controller)
    : m_ctx(ctx)
    , m_controller(controller)
{
}

const TimelineData& VimTimelineHost::data() const
{
    return m_controller->timeline()->data();
}

bool VimTimelineHost::trigger(const QString& actionId)
{
    if (!m_ctx.actions || !m_ctx.actions->contains(actionId))
        return false;
    QAction* action = m_ctx.actions->action(actionId);
    if (!action->isEnabled())
        return false;
    action->trigger();
    return true;
}

int64_t VimTimelineHost::playhead() const
{
    return m_controller->timeline()->playhead();
}

void VimTimelineHost::setPlayhead(int64_t frame)
{
    m_controller->timeline()->setPlayhead(std::max<int64_t>(0, frame));
}

void VimTimelineHost::clearSelection()
{
    m_ctx.selection->clear();
}

void VimTimelineHost::openCommandLine()
{
    if (onCommandLine)
        onCommandLine();
}

void VimTimelineHost::openSearch()
{
    if (onSearch)
        onSearch();
}

void VimTimelineHost::runEx(const QString& line)
{
    if (onEx)
        onEx(line);
}

// ---- The timeline as a buffer ----

int VimTimelineHost::trackCount() const
{
    return int(data().videoTracks.size() + data().audioTracks.size());
}

TrackRef VimTimelineHost::trackAt(int index) const
{
    const int videos = int(data().videoTracks.size());
    index = std::clamp(index, 0, std::max(0, trackCount() - 1));
    if (index < videos)
        return {TrackType::Video, videos - 1 - index};
    return {TrackType::Audio, index - videos};
}

int VimTimelineHost::indexOf(TrackRef ref) const
{
    const int videos = int(data().videoTracks.size());
    if (ref.type == TrackType::Video)
        return videos - 1 - ref.index;
    if (ref.type == TrackType::Audio)
        return videos + ref.index;
    return -1;
}

int VimTimelineHost::cursorTrack() const
{
    const int count = trackCount();
    if (count == 0)
        return 0;
    if (m_cursorTrack < 0) {
        // Until the first j / k the cursor sits on the target video track.
        const int target = m_controller->targetVideoTrack();
        if (target >= 0)
            return std::clamp(indexOf({TrackType::Video, target}), 0, count - 1);
        return int(data().videoTracks.size()) < count ? int(data().videoTracks.size()) : 0;
    }
    return std::clamp(m_cursorTrack, 0, count - 1);
}

void VimTimelineHost::setCursorTrack(int index)
{
    if (trackCount() == 0)
        return;
    m_cursorTrack = std::clamp(index, 0, trackCount() - 1);
    const TrackRef ref = trackAt(m_cursorTrack);
    // The cursor track is where source edits go.
    m_controller->setTargetTrack(ref.type, ref.index);
    updateOverlay();
}

QVector<VimClipSpan> VimTimelineHost::clipSpans(int track) const
{
    QVector<VimClipSpan> spans;
    if (trackCount() == 0 || track < 0 || track >= trackCount())
        return spans;
    if (const TrackData* t = data().track(trackAt(track))) {
        spans.reserve(int(t->clips.size()));
        for (const ClipData& c : t->clips)
            spans.append({c.id, c.start, c.end()});
    }
    return spans;
}

QVector<quint64> VimTimelineHost::linkedClips(quint64 clipId) const
{
    return ops::linkedClips(data(), clipId);
}

QVector<int64_t> VimTimelineHost::markerFrames() const
{
    QVector<int64_t> frames;
    for (const Marker& m : data().markers)
        frames.append(m.frame);
    std::sort(frames.begin(), frames.end());
    return frames;
}

QVector<int64_t> VimTimelineHost::keyframeFrames() const
{
    // The clip under the cursor on the cursor track, else the selected clip.
    const ClipData* clip = nullptr;
    if (trackCount() > 0) {
        if (const TrackData* t = data().track(trackAt(cursorTrack())))
            clip = t->clipAt(playhead());
    }
    if (!clip && !m_ctx.selection->clips().isEmpty())
        clip = ops::clipById(data(), m_ctx.selection->clips().last());
    QVector<int64_t> frames;
    if (!clip)
        return frames;
    for (const int64_t local : keyframeops::allKeyFrames(*clip))
        frames.append(clip->start + local);
    return frames;
}

int64_t VimTimelineHost::timelineEnd() const
{
    int64_t end = 0;
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& t : data().tracks(type))
            end = std::max(end, t.end());
    }
    return end;
}

double VimTimelineHost::framesPerSecond() const
{
    const Rational fps = data().fps;
    return fps.den > 0 ? double(fps.num) / double(fps.den) : 25.0;
}

std::pair<int64_t, int64_t> VimTimelineHost::markedRange() const
{
    const TimelineData& d = data();
    if (d.markIn >= 0 && d.markOut > d.markIn)
        return {d.markIn, d.markOut};
    return {-1, -1};
}

bool VimTimelineHost::trackLocked(int track) const
{
    if (track < 0 || track >= trackCount())
        return false;
    const TrackData* t = data().track(trackAt(track));
    return t && t->locked;
}

QVector<quint64> VimTimelineHost::selectedClips() const
{
    return m_controller->effectiveSelection();
}

// ---- Operations ----

void VimTimelineHost::beginUndoGroup()
{
    ++m_undoDepth;
    m_ctx.project->beginEdit(QObject::tr("Vim command"));
}

void VimTimelineHost::endUndoGroup()
{
    if (m_undoDepth <= 0)
        return;
    --m_undoDepth;
    m_ctx.project->endEdit();
}

std::vector<TrackRef> VimTimelineHost::tracksOf(const VimRange& range) const
{
    std::vector<TrackRef> tracks;
    if (range.allTracks) {
        for (int i = 0; i < trackCount(); ++i) {
            const TrackRef ref = trackAt(i);
            if (const TrackData* t = data().track(ref); t && t->autoSelect)
                tracks.push_back(ref);
        }
    } else {
        const int first = std::min(range.trackFirst, range.trackLast);
        const int last = std::max(range.trackFirst, range.trackLast);
        for (int i = first; i <= last && i < trackCount(); ++i) {
            if (!trackLocked(i)) // a locked track neither changes nor pulls its partners along
                tracks.push_back(trackAt(i));
        }
        if (m_controller->linkedSelection())
            tracks = ops::tracksWithLinkedPartners(data(), tracks, range.start, range.end);
    }
    return tracks;
}

void VimTimelineHost::store(QChar reg, const ops::Clipboard& board)
{
    if (board.isEmpty() || reg == QLatin1Char('_'))
        return;
    if (!reg.isNull() && reg != QLatin1Char('0'))
        m_registers.insert(reg, board);
    else
        m_controller->setClipboard(board); // the unnamed register is the editor's clipboard
    m_registers.insert(QLatin1Char('0'), board); // the last yank / delete
}

ops::Clipboard VimTimelineHost::registerContent(QChar reg) const
{
    if (reg.isNull())
        return m_controller->clipboard();
    return m_registers.value(reg);
}

bool VimTimelineHost::operate(const VimOperation& operation)
{
    const VimRange& range = operation.range;
    if (range.isEmpty())
        return false;
    Project* project = m_ctx.project;
    const bool yank = operation.op == VimOp::Yank;

    if (range.kind == VimRange::Kind::Clips) {
        // Clips on locked tracks are not touched, and do not pull their linked partners along.
        QVector<quint64> open;
        for (const quint64 id : range.clips) {
            const auto loc = ops::findClip(data(), id);
            if (loc && !data().track(loc->track)->locked)
                open.append(id);
        }
        if (open.isEmpty()) {
            showMessage(QObject::tr("The track is locked"));
            return false;
        }
        const QVector<quint64> ids = m_controller->withLinks(open);
        const ops::Clipboard board = ops::copyClips(data(), ids);
        if (board.isEmpty())
            return false;
        if (yank) {
            store(operation.reg, board);
            return true;
        }
        const bool ripple = operation.op == VimOp::Delete;
        int64_t first = INT64_MAX;
        for (const quint64 id : ids) {
            if (const ClipData* c = ops::clipById(data(), id))
                first = std::min(first, c->start);
        }
        const bool changed
            = timelineedit::runEdit(project, QObject::tr("Vim command"), [&](TimelineData& d) {
                  return ripple ? ops::rippleDeleteClips(d, ids, ops::RippleScope::AllTracks) > 0
                                : ops::liftClips(d, ids) > 0;
              });
        if (!changed) {
            showMessage(QObject::tr("The track is locked"));
            return false;
        }
        store(operation.reg, board);
        m_ctx.selection->clear();
        if (first != INT64_MAX)
            m_controller->setPlayheadAfterEdit(first);
        return true;
    }

    const std::vector<TrackRef> tracks = tracksOf(range);
    if (tracks.empty()) {
        showMessage(QObject::tr("The track is locked"));
        return false;
    }
    const ops::TrackFilter filter = ops::trackSetFilter(tracks);
    const ops::Clipboard board = ops::copyRange(data(), range.start, range.end, filter);
    if (yank) {
        store(operation.reg, board);
        return !board.isEmpty();
    }
    const bool ripple = operation.op == VimOp::Delete;
    const bool changed
        = timelineedit::runEdit(project, QObject::tr("Vim command"), [&](TimelineData& d) {
              return ripple ? ops::extractRange(d, range.start, range.end, filter)
                            : ops::liftRange(d, range.start, range.end, filter);
          });
    if (!changed) {
        showMessage(QObject::tr("The track is locked"));
        return false;
    }
    store(operation.reg, board);
    m_controller->setPlayheadAfterEdit(range.start);
    return true;
}

bool VimTimelineHost::registerHasContent(QChar reg) const
{
    return !registerContent(reg).isEmpty();
}

bool VimTimelineHost::pasteRegister(QChar reg, int count, bool insert)
{
    const ops::Clipboard board = registerContent(reg);
    if (board.isEmpty() || count < 1)
        return false;
    // Pasting goes through the controller's own paste (it knows the playhead, the selection
    // and the focus rules): the register is put on the clipboard for the duration of it.
    const ops::Clipboard keep = m_controller->clipboard();
    m_controller->setClipboard(board);
    m_ctx.project->beginEdit(QObject::tr("Vim paste"));
    for (int i = 0; i < count; ++i) {
        if (insert)
            m_controller->pasteInsert();
        else
            m_controller->paste();
    }
    m_ctx.project->endEdit();
    m_controller->setClipboard(keep);
    return true;
}

void VimTimelineHost::showVisual(const VimVisual& visual)
{
    m_visual = visual;
    if (visual.kind == VimVisual::Kind::Line) {
        // V: the clips touched by the frames and tracks are selected, so they show as such.
        const int64_t lo = std::min(visual.anchor.frame, visual.cursor.frame);
        const int64_t hi = std::max(visual.anchor.frame, visual.cursor.frame) + 1;
        const int first = std::min(visual.anchor.track, visual.cursor.track);
        const int last = std::max(visual.anchor.track, visual.cursor.track);
        QVector<quint64> ids;
        for (int t = first; t <= last; ++t) {
            for (const VimClipSpan& c : clipSpans(t)) {
                if (c.start < hi && c.end > lo)
                    ids.append(c.id);
            }
        }
        m_ctx.selection->setClips(m_controller->videoClipCurrent(ids));
    }
    updateOverlay();
}

void VimTimelineHost::updateOverlay()
{
    TimelineController::VimOverlay overlay;
    if (m_cursorShown && trackCount() > 0) {
        overlay.cursor = true;
        overlay.cursorTrack = trackAt(cursorTrack());
    }
    if (m_cursorShown && m_visual.kind != VimVisual::Kind::None && trackCount() > 0) {
        overlay.visual = m_visual.kind == VimVisual::Kind::Char ? 1
            : m_visual.kind == VimVisual::Kind::Line            ? 2
                                                                : 3;
        overlay.frameFrom = std::min(m_visual.anchor.frame, m_visual.cursor.frame);
        overlay.frameTo = std::max(m_visual.anchor.frame, m_visual.cursor.frame);
        overlay.trackFrom = trackAt(m_visual.anchor.track);
        overlay.trackTo = trackAt(m_visual.cursor.track);
        if (m_visual.kind == VimVisual::Kind::Char)
            overlay.trackFrom = overlay.trackTo;
    }
    m_controller->setVimOverlay(overlay);
}

void VimTimelineHost::showCursor(bool on)
{
    m_cursorShown = on;
    if (!on)
        m_visual = {};
    updateOverlay();
}

bool VimTimelineHost::selectCursorClip()
{
    if (!m_ctx.selection->clips().isEmpty() || trackCount() == 0)
        return false;
    const TrackData* track = data().track(trackAt(cursorTrack()));
    const ClipData* clip = track ? track->clipAt(playhead()) : nullptr;
    if (!clip)
        return false;
    m_ctx.selection->setClips({clip->id});
    return true;
}

void VimTimelineHost::restoreSelection(bool selectedByCursor)
{
    if (selectedByCursor)
        m_ctx.selection->clear();
}

void VimTimelineHost::copyToClipboard(const QString& text)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

QString VimTimelineHost::timecode(int64_t frame) const
{
    return timecode::formatRecord(frame, data());
}

void VimTimelineHost::showMessage(const QString& text)
{
    if (onMessage)
        onMessage(text);
}

bool VimTimelineHost::search(const QString& pattern)
{
    m_pattern = pattern.trimmed();
    return !m_pattern.isEmpty() && searchStep(true, 1);
}

bool VimTimelineHost::searchStep(bool forward, int count)
{
    if (m_pattern.isEmpty())
        return false;
    // Matches: marker names and the names of clips (at their first frame), in time order.
    QVector<int64_t> hits;
    for (const Marker& m : data().markers) {
        if (m.name.contains(m_pattern, Qt::CaseInsensitive))
            hits.append(m.frame);
    }
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& t : data().tracks(type)) {
            for (const ClipData& c : t.clips) {
                if (c.name.contains(m_pattern, Qt::CaseInsensitive))
                    hits.append(c.start);
            }
        }
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    if (hits.isEmpty())
        return false;
    int64_t at = playhead();
    for (int i = 0; i < std::max(1, count); ++i) {
        const auto it = forward ? std::upper_bound(hits.begin(), hits.end(), at)
                                : std::lower_bound(hits.begin(), hits.end(), at);
        if (forward) {
            at = it == hits.end() ? hits.first() : *it; // wraps round, as Vim does
        } else {
            at = it == hits.begin() ? hits.last() : *(it - 1);
        }
    }
    setPlayhead(at);
    const int index = int(std::lower_bound(hits.begin(), hits.end(), at) - hits.begin()) + 1;
    showMessage(QObject::tr("/%1  [%2/%3]").arg(m_pattern).arg(index).arg(hits.size()));
    return true;
}

} // namespace kk
