// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 4: timeline and clip markers with the marker editor (spec 18),
// paste attributes (spec 16.4) and dynamic trim (spec 8.17).
#include "core/ClipColors.h"
#include "core/ParamKeys.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QAction>
#include <QSettings>

#include <algorithm>

namespace kk {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

/// Inserts @p marker into the frame-sorted @p list unless one exists on its frame.
bool insertMarker(QVector<Marker>& list, const Marker& marker)
{
    auto it = std::lower_bound(list.begin(), list.end(), marker.frame,
        [](const Marker& m, int64_t f) { return m.frame < f; });
    if (it != list.end() && it->frame == marker.frame)
        return false;
    list.insert(it, marker);
    return true;
}

Marker* markerAt(QVector<Marker>& list, int64_t frame)
{
    for (Marker& m : list) {
        if (m.frame == frame)
            return &m;
    }
    return nullptr;
}

} // namespace

// ---- Markers ----

QColor TimelineController::newMarkerColor()
{
    const QString id = QSettings().value(QLatin1String(prefs::kNewMarkerColor)).toString();
    for (const PaletteColor& c : markerColorPalette()) {
        if (id == QLatin1String(c.id))
            return QColor(c.rgb);
    }
    return QColor(markerColorPalette().front().rgb);
}

QString TimelineController::newMarkerName(const QVector<Marker>& existing)
{
    // Highest number of the default names (translated pattern); other names are ignored.
    const QString pattern = tr("Marker %1");
    const qsizetype at = pattern.indexOf(QLatin1String("%1"));
    const QString prefix = pattern.left(at);
    const QString suffix = pattern.mid(at + 2);
    int highest = 0;
    for (const Marker& m : existing) {
        if (!m.name.startsWith(prefix) || !m.name.endsWith(suffix)
            || m.name.size() <= prefix.size() + suffix.size())
            continue;
        bool ok = false;
        const int n
            = m.name.mid(prefix.size(), m.name.size() - prefix.size() - suffix.size()).toInt(&ok);
        if (ok && n > highest)
            highest = n;
    }
    return pattern.arg(highest + 1);
}

void TimelineController::addMarkerAtPlayhead()
{
    placeMarker(false);
}

void TimelineController::addAndModifyMarker()
{
    placeMarker(true);
}

void TimelineController::placeMarker(bool forceEdit)
{
    const TimelineData& d = timeline()->data();
    const int64_t f = timeline()->playhead();
    // A selected clip under the playhead gets a clip marker (spec 18.4); the current clip of
    // the selection first.
    quint64 clipId = 0;
    QVector<quint64> candidates = m_ctx.selection->clips();
    std::reverse(candidates.begin(), candidates.end()); // currentClip() is the last one
    for (quint64 id : std::as_const(candidates)) {
        const auto loc = ops::findClip(d, id);
        if (loc && !d.track(loc->track)->locked
            && d.track(loc->track)->clips[size_t(loc->clipIndex)].contains(f)) {
            clipId = id;
            break;
        }
    }
    if (clipId) {
        const int64_t local = f - ops::clipById(d, clipId)->start;
        const QVector<Marker>& existing = ops::clipById(d, clipId)->markers;
        const bool exists = std::any_of(existing.cbegin(), existing.cend(),
            [local](const Marker& m) { return m.frame == local; });
        if (exists && !forceEdit) {
            // M toggles: a clip marker starting at the playhead is removed (owner request 0.4.0).
            runEdit(project(), tr("Delete Marker"), [&](TimelineData& data) {
                ClipData* clip = ops::clipById(data, clipId);
                return clip->markers.removeIf([local](const Marker& m) { return m.frame == local; })
                    > 0;
            });
            return;
        }
        bool added = false;
        runEdit(project(), tr("Add Marker"), [&](TimelineData& data) {
            ClipData* clip = ops::clipById(data, clipId);
            Marker m;
            m.frame = local;
            m.name = newMarkerName(clip->markers);
            m.color = newMarkerColor();
            added = insertMarker(clip->markers, m);
            return added;
        });
        if (!added || forceEdit)
            editClipMarker(clipId, local);
        return;
    }
    const bool exists = std::any_of(
        d.markers.cbegin(), d.markers.cend(), [f](const Marker& m) { return m.frame == f; });
    if (exists && !forceEdit) {
        deleteMarkerAtPlayhead(); // M toggles a timeline marker (owner request 0.4.0)
        return;
    }
    if (!exists)
        addMarker();
    if (forceEdit)
        editTimelineMarker(f);
}

bool TimelineController::editTimelineMarker(int64_t frame)
{
    if (!m_markerEditor)
        return false;
    const TimelineData& d = timeline()->data();
    const auto it = std::find_if(d.markers.cbegin(), d.markers.cend(),
        [frame](const Marker& m) { return m.frame == frame; });
    if (it == d.markers.cend())
        return false;
    Marker edited = *it;
    if (!m_markerEditor(edited, false))
        return false;
    edited.frame = frame; // the editor changes name, colour, duration and note only
    return runEdit(project(), tr("Modify Marker"), [&](TimelineData& data) {
        Marker* m = markerAt(data.markers, frame);
        if (!m || *m == edited)
            return false;
        *m = edited;
        return true;
    });
}

bool TimelineController::editClipMarker(quint64 clipId, int64_t localFrame)
{
    if (!m_markerEditor)
        return false;
    const ClipData* clip = ops::clipById(timeline()->data(), clipId);
    if (!clip)
        return false;
    QVector<Marker> copy = clip->markers;
    Marker* current = markerAt(copy, localFrame);
    if (!current)
        return false;
    Marker edited = *current;
    if (!m_markerEditor(edited, true))
        return false;
    edited.frame = localFrame;
    return runEdit(project(), tr("Modify Marker"), [&](TimelineData& data) {
        ClipData* c = ops::clipById(data, clipId);
        Marker* m = c ? markerAt(c->markers, localFrame) : nullptr;
        if (!m || *m == edited)
            return false;
        *m = edited;
        return true;
    });
}

// ---- Paste attributes ----

void TimelineController::pasteAttributesInteractive()
{
    if (m_clipboard.isEmpty() || effectiveSelection().isEmpty())
        return;
    PasteAttributeSet attributes;
    if (m_pastePrompt) {
        const auto chosen = m_pastePrompt();
        if (!chosen)
            return;
        attributes = *chosen;
    }
    pasteAttributes(attributes);
}

bool TimelineController::pasteAttributes(const PasteAttributeSet& attributes)
{
    const QVector<quint64> ids = effectiveSelection();
    if (m_clipboard.isEmpty() || ids.isEmpty() || !attributes.any())
        return false;
    // The first copied clip of each type is the source of that type's attributes.
    const ClipData* sourceVideo = nullptr;
    const ClipData* sourceAudio = nullptr;
    for (const ops::ClipboardClip& c : m_clipboard.clips) {
        if (c.type == TrackType::Video && !sourceVideo)
            sourceVideo = &c.clip;
        if (c.type == TrackType::Audio && !sourceAudio)
            sourceAudio = &c.clip;
    }
    return runEdit(project(), tr("Paste Attributes"), [&](TimelineData& data) {
        bool changed = false;
        for (quint64 id : ids) {
            const auto loc = ops::findClip(data, id);
            if (!loc || data.track(loc->track)->locked)
                continue;
            const bool video = loc->track.type == TrackType::Video;
            const ClipData* src = video ? sourceVideo : sourceAudio;
            if (!src)
                continue;
            ClipData& c = data.track(loc->track)->clips[size_t(loc->clipIndex)];
            const ClipData before = c;
            if (video && attributes.transform)
                c.transform = src->transform; // keyframes keep their clip-local timing
            if (video && attributes.effects) {
                c.effects = src->effects;
                for (EffectInstance& fx : c.effects)
                    fx.id = data.allocateId();
            }
            if (!video && attributes.audioEffects) {
                c.effects = src->effects;
                for (EffectInstance& fx : c.effects)
                    fx.id = data.allocateId();
            }
            if (!video) {
                for (const auto& [enabled, key] : {std::pair {attributes.volume, keys::VolumeDb},
                         std::pair {attributes.pan, keys::Pan}}) {
                    if (!enabled)
                        continue;
                    if (src->audio.values.contains(key))
                        c.audio.values.insert(key, src->audio.values.value(key));
                    else
                        c.audio.values.remove(key);
                }
            }
            changed |= !(c == before);
            if (attributes.speed)
                changed |= timelineedit::applyClipRetime(data, id, *src);
        }
        return changed;
    });
}

// ---- Dynamic trim ----

void TimelineController::setDynamicTrim(bool on)
{
    if (m_dynamicTrim == on)
        return;
    endDynamicRun();
    m_dynamicTrim = on;
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("trim.dynamic")))
        m_ctx.actions->action(QStringLiteral("trim.dynamic"))->setChecked(on);
    emit stateChanged();
}

bool TimelineController::isPlaying() const
{
    if (m_playing)
        return m_playing();
    return m_ctx.playback && m_ctx.playback->isPlaying();
}

void TimelineController::onPlayheadMoved(int64_t frame)
{
    const int64_t delta = frame - m_dynamicLast;
    m_dynamicLast = frame;
    // An edit point, else selected clips (dynamic slip / slide, spec 8.21).
    const QVector<quint64> clips = m_selectedEdit.isEmpty() && m_tool != TimelineTool::Blade
        ? effectiveSelection()
        : QVector<quint64> {};
    const bool active
        = m_dynamicTrim && (!m_selectedEdit.isEmpty() || !clips.isEmpty()) && isPlaying();
    if (!active) {
        endDynamicRun();
        return;
    }
    if (delta == 0)
        return;
    if (!m_dynamicOpen) {
        // One undo step per playback run; closed when playback stops (endDynamicRun()).
        project()->beginEdit(tr("Dynamic Trim"));
        project()->markEditKeepsPlaying(); // playback is what drives it (spec 8.17)
        m_dynamicOpen = true;
    }
    if (!m_selectedEdit.isEmpty()) {
        // The edit point follows the playhead: a roll, or a ripple trim in the trim tool.
        applyEditTrim(
            timeline()->mutableData(), m_selectedEdit, delta, m_tool == TimelineTool::Trim);
    } else {
        // Selected clips: the selection tool slides them, the trim tool slips them (slides
        // them when they were picked by the name band), as a drag following the playhead.
        const bool slide = m_tool == TimelineTool::Select || slideSelected();
        applyBodyTrim(timeline()->mutableData(), clips, delta, slide);
    }
    timeline()->notifyChanged();
}

void TimelineController::endDynamicRun()
{
    if (!m_dynamicOpen)
        return;
    m_dynamicOpen = false;
    project()->endEdit();
}

} // namespace kk
