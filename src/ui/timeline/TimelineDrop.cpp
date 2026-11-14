// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget drag and drop: media items, source ranges, effects, transitions, generators
// (MIME contract in ui/common/DragMime.h). planDrop() turns a MIME payload and a position into
// a DropPlan; the same plan drives the ghost preview while dragging and the edit on drop, so
// what the user sees is exactly what they get.
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"
#include "platform/DroppedFiles.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/common/FolderImport.h"
#include "ui/common/MediaImport.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/DroppedFileProbe.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTimer>

namespace kk {

using namespace timelinemetrics;
namespace ops = editops;

namespace {
/// Default transition length for drops (frames = 1 s).
constexpr int kDropTransitionSeconds = 1;

bool hasInternalFormat(const QMimeData* mime)
{
    return mime->hasFormat(QLatin1String(mime::MediaItems))
        || mime->hasFormat(QLatin1String(mime::SourceRange))
        || mime->hasFormat(QLatin1String(mime::Effect))
        || mime->hasFormat(QLatin1String(mime::TimelineClips));
}

/// Our own payloads, or files from a file manager / the desktop (Dolphin: text/uri-list;
/// sandboxed sources: the document portal's file transfer).
bool hasTimelineFormat(const QMimeData* mime)
{
    return hasInternalFormat(mime) || platform::hasDroppedFiles(mime);
}
} // namespace

TimelineWidget::DropPlan TimelineWidget::planDrop(
    const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers)
{
    DropPlan plan;
    const TimelineData& data = m_ctx.project->timeline()->data();
    TimelineHit hit = m_layout.hitTest(data, pos);
    const int nv = int(data.videoTracks.size());
    const int na = int(data.audioTracks.size());

    // Empty space of the track area (Resolve): above the top video track a drop creates a new
    // video track, below the last audio track a new audio track; the gap between V1 and A1
    // counts as the nearer of the two. Ruler and headers never take drops.
    const auto& rows = m_layout.rows();
    const bool inTrackArea
        = pos.x() >= kHeaderWidth && pos.y() >= kRulerHeight && hit.area == TimelineHit::Area::None;
    enum class Zone {
        Rows,
        Above,
        Below
    } zone = Zone::Rows;
    if (inTrackArea && !rows.empty()) {
        const QRect top = m_layout.rowRect(0);
        const QRect bottom = m_layout.rowRect(int(rows.size()) - 1);
        if (pos.y() < top.top()) {
            zone = rows.front().ref.type == TrackType::Video ? Zone::Above : Zone::Rows;
        } else if (pos.y() > bottom.bottom()) {
            zone = rows.back().ref.type == TrackType::Audio ? Zone::Below : Zone::Rows;
        } else {
            // the V/A divider: the row whose edge is nearest
            int best = -1;
            double bestDistance = 0;
            for (size_t i = 0; i < rows.size(); ++i) {
                const QRect r = m_layout.rowRect(int(i));
                const double d
                    = std::min(std::abs(pos.y() - r.top()), std::abs(pos.y() - r.bottom()));
                if (best < 0 || d < bestDistance) {
                    best = int(i);
                    bestDistance = d;
                }
            }
            if (best >= 0) {
                hit.area = TimelineHit::Area::Track;
                hit.row = best;
                hit.track = rows[size_t(best)].ref;
            }
        }
        if (zone == Zone::Above) {
            hit.area = TimelineHit::Area::Track;
            hit.track = TrackRef {TrackType::Video, nv};
            plan.newVideoTrack = true;
            const int h = std::min(top.height(), top.top() - kRulerHeight);
            plan.newTrackBand = QRect(0, top.top() - h, top.width(), h);
            plan.newTrackBandType = TrackType::Video;
        } else if (zone == Zone::Below) {
            hit.area = TimelineHit::Area::Track;
            hit.track = TrackRef {TrackType::Audio, na};
            plan.newAudioTrack = true;
            plan.newTrackBand = QRect(0, bottom.bottom() + 1, bottom.width(), bottom.height());
            plan.newTrackBandType = TrackType::Audio;
        }
    }
    if (hit.area != TimelineHit::Area::Track && hit.area != TimelineHit::Area::Clip)
        return plan;
    plan.insert = modifiers & Qt::ControlModifier;

    // Destination tracks: the row under the mouse for its type, the same index (clamped) for
    // the other type, so V2 pairs with A2 like the source patch would. In a new-track zone the
    // other type uses the same index too and is created when missing (new V3 pairs with A3).
    const int idx = hit.track.index;
    int videoTrack = nv == 0 ? -1 : std::min(idx, nv - 1);
    int audioTrack = na == 0 ? -1 : std::min(idx, na - 1);
    if (zone == Zone::Above) {
        videoTrack = nv;
        audioTrack = std::min(idx, na);
        plan.newAudioTrack = audioTrack == na;
    } else if (zone == Zone::Below) {
        audioTrack = na;
        videoTrack = std::min(idx, nv);
        plan.newVideoTrack = videoTrack == nv;
    }
    int64_t frame = m_layout.nearestFrameAtX(pos.x());

    auto snapStart = [&](int64_t f, int64_t duration) {
        if (!m_controller->snappingNow(modifiers))
            return f;
        const auto snaps = ops::snapCandidates(data, m_ctx.project->timeline()->playhead());
        const int64_t tol = snapToleranceFrames();
        const auto s1 = ops::snapToNearest(snaps, f, tol);
        const auto s2 = ops::snapToNearest(snaps, f + duration, tol);
        if (s1 && (!s2 || std::abs(*s1 - f) <= std::abs(*s2 - f - duration)))
            return *s1;
        if (s2)
            return std::max<int64_t>(0, *s2 - duration);
        return f;
    };

    // ---- Clips of another timeline (stacked timelines): a copy ----
    if (mime->hasFormat(QLatin1String(mime::TimelineClips))) {
        planClipCopy(mime, hit, pos, plan);
        return plan;
    }

    // ---- Media items / source range / files from outside: one group of clips per source,
    // placed in sequence. Files (a file manager, the desktop) are planned from their probe
    // (DroppedFileProbe, started on DragEnter) or the pool item of the same file, so the ghost
    // shows their real length like a media pool drag; the drop imports them and re-plans the
    // new items at the same position. ----
    struct Source
    {
        const MediaItem* item = nullptr;
        int64_t inUs = 0;
        int64_t outUs = 0;
    };
    QVector<Source> sources;
    const MediaPool* pool = m_ctx.project->mediaPool();
    const bool files = !hasInternalFormat(mime);
    if (files) {
        if (!platform::hasDroppedFiles(mime) || videoTrack < 0)
            return plan;
        plan.kind = DropPlan::Kind::Files;
        for (const QString& path : m_dragFiles) {
            const QString absolute = QFileInfo(path).absoluteFilePath();
            const MediaItem* item = pool->itemByPath(absolute);
            if (!item && m_fileProbe)
                item = m_fileProbe->item(absolute); // nullptr: a folder or not importable
            if (item)
                sources.append({item, 0, 0});
        }
    } else if (mime->hasFormat(QLatin1String(mime::SourceRange))) {
        const QJsonObject o
            = QJsonDocument::fromJson(mime->data(QLatin1String(mime::SourceRange))).object();
        sources.append({pool->item(o.value(QLatin1String("mediaId")).toString()),
            int64_t(o.value(QLatin1String("inUs")).toDouble()),
            int64_t(o.value(QLatin1String("outUs")).toDouble())});
    } else if (mime->hasFormat(QLatin1String(mime::MediaItems))) {
        const QString text = QString::fromUtf8(mime->data(QLatin1String(mime::MediaItems)));
        for (const QString& id : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            sources.append({pool->item(id.trimmed()), 0, 0});
    }
    if (files || !sources.isEmpty()) {
        int64_t at = -1;
        for (const Source& s : sources) {
            const MediaItem* item = s.item;
            if (!item)
                continue;
            if (item->isSubtitle()) {
                // A subtitle file: one caption per cue on subtitle tracks (the row under the
                // mouse when it is a subtitle track, else ST1; overlapping cues on the tracks
                // above), timed from the drop point. Missing tracks are created on drop.
                const int base = hit.track.type == TrackType::Subtitle ? hit.track.index : 0;
                const int64_t length = std::max<int64_t>(
                    1, usToFrames(subtitles::cuesDurationUs(item->cues), data.fps));
                if (at < 0)
                    at = snapStart(frame, length);
                QVector<ops::PlacedClip> group;
                int maxTrack = -1;
                for (const subtitles::PlacedCaption& c :
                    subtitles::captionsFromCues(item->cues, data.fps, at)) {
                    group.append({TrackRef {TrackType::Subtitle, base + c.lane}, c.clip});
                    maxTrack = std::max(maxTrack, base + c.lane);
                }
                plan.newSubtitleTracks = std::max(
                    plan.newSubtitleTracks, maxTrack + 1 - int(data.subtitleTracks.size()));
                at += length;
                if (!group.isEmpty())
                    plan.groups.append(group);
                continue;
            }
            // makeMediaClips() only knows existing tracks: ask for track 0 of a type that will be
            // created and move the clip to the new index afterwards.
            QVector<ops::PlacedClip> group = m_controller->makeMediaClips(*item, s.inUs, s.outUs,
                videoTrack >= nv ? std::min(0, nv - 1) : videoTrack,
                audioTrack >= na ? std::min(0, na - 1) : audioTrack);
            for (ops::PlacedClip& p : group)
                p.track.index = p.track.type == TrackType::Video ? videoTrack : audioTrack;
            if (group.isEmpty())
                continue;
            if (at < 0)
                at = snapStart(frame, group.front().clip.duration);
            for (ops::PlacedClip& p : group)
                p.clip.start = at;
            at += group.front().clip.duration;
            plan.groups.append(group);
        }
        const bool onlyCaptions = std::all_of(plan.groups.cbegin(), plan.groups.cend(),
            [](const auto& g) { return g.front().track.type == TrackType::Subtitle; });
        if (onlyCaptions) {
            // Captions never need the new video / audio track of the empty-space zones; new
            // subtitle tracks are shown in the band instead.
            plan.newVideoTrack = plan.newAudioTrack = false;
            plan.newTrackBandType = TrackType::Subtitle;
        }
        // Only the tracks a clip lands on are created: a video-only clip dropped above the top
        // video track makes a new video track and no empty audio track (and the reverse).
        const auto needs = [&plan](TrackType type, int index) {
            return std::any_of(plan.groups.cbegin(), plan.groups.cend(), [&](const auto& g) {
                return std::any_of(g.cbegin(), g.cend(), [&](const ops::PlacedClip& p) {
                    return p.track.type == type && p.track.index >= index;
                });
            });
        };
        plan.newVideoTrack = plan.newVideoTrack && needs(TrackType::Video, nv);
        plan.newAudioTrack = plan.newAudioTrack && needs(TrackType::Audio, na);
        if (files)
            plan.kind = DropPlan::Kind::Files; // with or without ghosts: the drop imports
        else if (!plan.groups.isEmpty())
            plan.kind = DropPlan::Kind::Clips;
        return plan;
    }

    // ---- Effects library: effect, transition, generator or title ----
    if (!mime->hasFormat(QLatin1String(mime::Effect)))
        return plan;
    const QString effectId = QString::fromUtf8(mime->data(QLatin1String(mime::Effect))).trimmed();
    const EffectDescriptor* desc = EffectRegistry::instance().find(effectId);
    if (!desc)
        return plan;
    plan.effectId = effectId;

    switch (desc->category) {
    case EffectCategory::Generator:
    case EffectCategory::Title:
    case EffectCategory::Effects: {
        if (videoTrack < 0)
            return plan;
        std::optional<ClipData> clip = effectactions::makeGeneratorClip(
            effectId, data.fps, QSizeF(data.width, data.height)); // Region Blur: mask size
        if (!clip)
            return plan;
        clip->start = snapStart(frame, clip->duration);
        // Titles, generators and adjustment clips always go onto a video track: the row under
        // the mouse when it is one (a new one above the top track), else V1.
        if (zone == Zone::Below)
            return DropPlan {};
        const int track = hit.track.type == TrackType::Video ? hit.track.index : 0;
        plan.newAudioTrack = false;
        plan.groups.append({ops::PlacedClip {TrackRef {TrackType::Video, track}, *clip}});
        plan.kind = DropPlan::Kind::Clips;
        return plan;
    }
    case EffectCategory::VideoEffect:
    case EffectCategory::AudioEffect: {
        const TrackType wanted
            = desc->category == EffectCategory::VideoEffect ? TrackType::Video : TrackType::Audio;
        if (hit.area != TimelineHit::Area::Clip || hit.track.type != wanted)
            return plan;
        plan.kind = DropPlan::Kind::Effect;
        plan.clipId = hit.clipId;
        return plan;
    }
    case EffectCategory::Transition:
    case EffectCategory::AudioTransition: {
        // Video transitions go onto video clips, audio transitions onto audio clips; the other
        // kind of track refuses the drop.
        if (hit.area != TimelineHit::Area::Clip
            || !effectactions::transitionFitsTrack(desc->category, hit.track.type))
            return plan;
        const auto loc = ops::findClip(data, hit.clipId);
        const TrackData& track = *data.track(loc->track);
        size_t index = size_t(loc->clipIndex);
        const ClipData* c = &track.clips[index];
        bool head = hit.frame < (c->start + c->end()) / 2.0;
        // The tail of a clip touching its right neighbour is that neighbour's head.
        if (!head && index + 1 < track.clips.size() && track.clips[index + 1].start == c->end()) {
            ++index;
            head = true;
            c = &track.clips[index];
        }
        const bool touching = head && index > 0 && track.clips[index - 1].end() == c->start;
        plan.kind = DropPlan::Kind::Transition;
        plan.clipId = c->id;
        plan.head = head;
        plan.transitionFrames = std::min(
            std::max<int64_t>(1, usToFrames(int64_t(kDropTransitionSeconds) * 1000000, data.fps)),
            c->duration);
        const int64_t d = plan.transitionFrames;
        if (head) {
            plan.previewFrom = touching ? c->start - d / 2 : c->start;
        } else {
            plan.previewFrom = c->end() - d;
        }
        plan.previewTo = plan.previewFrom + d;
        plan.previewRow = m_layout.rowOf(loc->track);
        return plan;
    }
    case EffectCategory::ClipProperties:
        return plan;
    }
    return plan;
}

void TimelineWidget::showDropPreview(const DropPlan& plan)
{
    m_overlay.ghosts.clear();
    m_overlay.dropClip = 0;
    m_overlay.dropTransitionRow = -1;
    switch (plan.kind) {
    case DropPlan::Kind::Clips:
    case DropPlan::Kind::Files: // probed files: the clips the drop will place
    case DropPlan::Kind::TimelineClips:
        for (const auto& group : plan.groups) {
            for (const ops::PlacedClip& p : group) {
                const int row = m_layout.rowOf(p.track);
                // A clip on a track the drop creates: drawn in the empty band where the new
                // track will appear (only the new video track above, or audio below, shows).
                if (row < 0
                    && (plan.newTrackBand.isEmpty() || p.track.type != plan.newTrackBandType))
                    continue;
                m_overlay.ghosts.push_back({row, row < 0 ? plan.newTrackBand : QRect(),
                    p.clip.start, p.clip.duration, p.clip.name});
            }
        }
        break;
    case DropPlan::Kind::None:
        break;
    case DropPlan::Kind::Effect:
        m_overlay.dropClip = plan.clipId;
        break;
    case DropPlan::Kind::Transition:
        m_overlay.dropTransitionRow = plan.previewRow;
        m_overlay.dropTransitionFrom = plan.previewFrom;
        m_overlay.dropTransitionTo = plan.previewTo;
        break;
    }
    viewport()->update();
}

void TimelineWidget::clearDropPreview()
{
    showDropPreview(DropPlan {});
}

bool TimelineWidget::executeDrop(const DropPlan& plan)
{
    Project* project = m_ctx.project;
    QVector<quint64> created;
    switch (plan.kind) {
    case DropPlan::Kind::None:
    case DropPlan::Kind::Files: // dropEvent() imports the files and re-plans
        return false;
    case DropPlan::Kind::TimelineClips:
        return !m_controller
                    ->copyClipsFrom(plan.sourceUid, plan.sourceClips, plan.copyFrame,
                        plan.copyTrack, plan.insert)
                    .isEmpty();
    case DropPlan::Kind::Clips: {
        project->beginEdit(plan.insert ? tr("Insert Clip") : tr("Add Clip"));
        TimelineData& data = project->timeline()->mutableData();
        // New tracks first: the planned refs use index = the old track count.
        if (plan.newVideoTrack)
            ops::addTrack(data, TrackType::Video);
        if (plan.newAudioTrack)
            ops::addTrack(data, TrackType::Audio);
        for (int i = 0; i < plan.newSubtitleTracks; ++i)
            ops::addTrack(data, TrackType::Subtitle);
        for (const auto& group : plan.groups) {
            // Captions of a subtitle file keep their own timing: always overwrite.
            const bool captions = group.front().track.type == TrackType::Subtitle;
            const QVector<quint64> ids = plan.insert && !captions
                ? ops::insertClips(data, group, ops::RippleScope::AllTracks)
                : ops::overwriteClips(data, group);
            // Video + audio of one source are linked; the captions of a subtitle file are not.
            if (ids.size() > 1 && group.front().track.type != TrackType::Subtitle)
                ops::linkClips(data, ids);
            created += ids;
        }
        if (created.isEmpty()) {
            project->cancelEdit();
            return false;
        }
        project->endEdit();
        m_ctx.selection->setClips(m_controller->videoClipCurrent(created));
        return true;
    }
    case DropPlan::Kind::Effect: {
        project->beginEdit(tr("Add Effect"));
        TimelineData& data = project->timeline()->mutableData();
        const auto loc = ops::findClip(data, plan.clipId);
        if (!loc || data.track(loc->track)->locked) {
            project->cancelEdit();
            return false;
        }
        const quint64 fxId = data.allocateId();
        ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        clip.effects.append(EffectRegistry::instance().createEffect(plan.effectId, fxId));
        project->endEdit();
        m_ctx.selection->selectClip(plan.clipId);
        return true;
    }
    case DropPlan::Kind::Transition: {
        project->beginEdit(tr("Add Transition"));
        TimelineData& data = project->timeline()->mutableData();
        const auto loc = ops::findClip(data, plan.clipId);
        if (!loc || data.track(loc->track)->locked) {
            project->cancelEdit();
            emit m_controller->editRefused(tr("The track is locked."));
            return false;
        }
        const TransitionData t
            = EffectRegistry::instance().createTransition(plan.effectId, plan.transitionFrames);
        // One transition per cut: placeTransition also clears a stale one on the left clip.
        if (!ops::placeTransition(data, plan.clipId, plan.head, t)) {
            project->cancelEdit();
            emit m_controller->editRefused(tr("The transition does not fit at that edit."));
            return false;
        }
        project->endEdit();
        return true;
    }
    }
    return false;
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!hasTimelineFormat(event->mimeData())) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    startFileDrag(event->mimeData(), event->position(), event->modifiers());
    showDropPreview(planDrop(event->mimeData(), event->position(), event->modifiers()));
    logDropEvent("timeline", event);
}

void TimelineWidget::startFileDrag(
    const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers)
{
    m_dragFiles.clear();
    if (hasInternalFormat(mime) || !platform::hasDroppedFiles(mime))
        return;
    // Read the paths once (on Wayland every read of another client's drag data is a pipe) and
    // probe them on workers: the drag never waits, the ghost grows to the real length when a
    // probe answers (onFileProbed()). A portal key alone has no paths before the drop.
    m_dragFiles = platform::draggedLocalFiles(mime);
    m_dragPos = pos;
    m_dragModifiers = modifiers;
    if (m_dragFiles.isEmpty())
        return;
    if (!m_fileProbe) {
        m_fileProbe = new DroppedFileProbe(this);
        connect(m_fileProbe, &DroppedFileProbe::probed, this, [this] {
            if (m_dragFiles.isEmpty())
                return; // the drag is over
            QMimeData urls; // planDrop() reads m_dragFiles; the URLs only mark a file drag
            urls.setUrls({QUrl::fromLocalFile(m_dragFiles.front())});
            showDropPreview(planDrop(&urls, m_dragPos, m_dragModifiers));
        });
    }
    m_fileProbe->request(m_dragFiles);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event)
{
    m_dragPos = event->position();
    m_dragModifiers = event->modifiers();
    const DropPlan plan = planDrop(event->mimeData(), event->position(), event->modifiers());
    showDropPreview(plan);
    if (plan.kind == DropPlan::Kind::None)
        event->ignore();
    else
        event->acceptProposedAction();
    logDropEvent("timeline", event);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    // Leaving the timeline, or Escape in the drag source: the ghost goes.
    m_dragFiles.clear();
    clearDropPreview();
}

void TimelineWidget::dropEvent(QDropEvent* event)
{
    DropPlan plan = planDrop(event->mimeData(), event->position(), event->modifiers());
    m_dragFiles.clear();
    clearDropPreview();
    qCDebug(lcDnd) << "timeline drop at" << event->position() << "plan" << int(plan.kind)
                   << describeMime(event->mimeData());
    if (plan.kind == DropPlan::Kind::Files) {
        // Read the paths now (the source's data is only available during the drop), finish
        // the drop, then import from the event loop: importing may show a modal report, and a
        // nested event loop inside the drop would keep the source (Dolphin) waiting and, on
        // Wayland, the compositor's drag session open. Import is one undo step (into the media
        // pool's current bin); placing the items exactly like a media pool drag at the same
        // position is a second one, so one Undo takes the clips off the timeline and leaves
        // the items in the pool, as after importing first and dragging from the pool.
        const QStringList paths = platform::droppedLocalFiles(event->mimeData());
        if (paths.isEmpty()) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        const QPointF pos = event->position();
        const Qt::KeyboardModifiers modifiers = event->modifiers();
        QTimer::singleShot(0, this, [this, paths, pos, modifiers] {
            // A folder goes into the media pool only (with its subfolders as bins), not onto
            // the timeline: placing a whole folder tree as clips is rarely what was meant.
            QStringList files = paths;
            const QStringList folders = takeFolders(files);
            if (!folders.isEmpty())
                importMediaFoldersWithFeedback(m_ctx, folders, FolderImportOptions {}, this);
            const QStringList ids = importDroppedFiles(files);
            if (ids.isEmpty())
                return;
            QMimeData items;
            items.setData(QLatin1String(mime::MediaItems), ids.join(QLatin1Char('\n')).toUtf8());
            executeDrop(planDrop(&items, pos, modifiers));
        });
        return;
    }
    if (executeDrop(plan)) {
        event->acceptProposedAction();
    } else {
        if (plan.kind == DropPlan::Kind::None)
            explainRefusedEffectDrop(event->mimeData());
        event->ignore();
    }
}

void TimelineWidget::explainRefusedEffectDrop(const QMimeData* mime)
{
    // A drop of an Effects library item that found no target says why (the status bar).
    if (!m_controller || !mime->hasFormat(QLatin1String(mime::Effect)))
        return;
    const EffectDescriptor* desc = EffectRegistry::instance().find(
        QString::fromUtf8(mime->data(QLatin1String(mime::Effect))).trimmed());
    if (!desc)
        return;
    switch (desc->category) {
    case EffectCategory::Transition:
        emit m_controller->editRefused(
            tr("Drop a video transition on a video clip, at its start, end or an edit point."));
        break;
    case EffectCategory::AudioTransition:
        emit m_controller->editRefused(
            tr("Drop an audio transition on an audio clip, at its start, end or an edit point."));
        break;
    case EffectCategory::VideoEffect:
        emit m_controller->editRefused(tr("Drop a video effect on a video clip."));
        break;
    case EffectCategory::AudioEffect:
        emit m_controller->editRefused(tr("Drop an audio effect on an audio clip."));
        break;
    default:
        break;
    }
}

QStringList TimelineWidget::importDroppedFiles(const QStringList& files)
{
    // A file already in the media pool is used as it is (Resolve does not import it twice);
    // only the others are imported, and the ids come back in the order of the drag.
    MediaPool* pool = m_ctx.project->mediaPool();
    auto existing = [pool](const QString& path) {
        const MediaItem* item = pool->itemByPath(QFileInfo(path).absoluteFilePath());
        return item ? item->id : QString();
    };
    QStringList fresh;
    for (const QString& path : files) {
        if (existing(path).isEmpty())
            fresh.append(path);
    }
    QStringList imported = fresh.isEmpty()
        ? QStringList()
        : importMediaWithFeedback(m_ctx, fresh, this, m_controller->targetBin());
    QStringList ids;
    for (const QString& path : files) {
        const QString id = existing(path);
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
            imported.removeAll(id);
        }
    }
    ids += imported; // stored copies of a single-file project have other paths: in order
    return ids;
}

} // namespace kk
