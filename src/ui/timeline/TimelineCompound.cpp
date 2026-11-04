// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 5: timelines (new, duplicate, delete, tabs) and compound clips
// (new, open, decompose). The work happens in core (Project / CompoundOps); this file adds
// the questions, the selection handling and the user-facing messages.
#include "core/EditOps.h"
#include "core/Plural.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk {

namespace {

QString binOf(const std::function<QString()>& provider)
{
    return provider ? provider() : QString();
}

} // namespace

quint64 TimelineController::newCompoundClip(const QString& name)
{
    Project* p = project();
    const QVector<quint64> ids = effectiveSelection();
    if (ids.isEmpty() || p->isEditing())
        return 0;
    QString finalName = name;
    if (finalName.isEmpty()) {
        const QString suggestion = p->uniqueTimelineName(tr("Compound Clip"));
        if (m_namePrompt) {
            const std::optional<QString> answer = m_namePrompt(
                tr("New Compound Clip"), tr("Compound clip name"), suggestion, tr("Create"));
            if (!answer)
                return 0;
            finalName = answer->trimmed().isEmpty() ? suggestion : answer->trimmed();
        } else {
            finalName = suggestion;
        }
    }
    QString error;
    const quint64 uid = p->createCompoundClip(ids, finalName, binOf(m_binProvider), &error);
    if (uid == 0) {
        if (m_messageSink && !error.isEmpty())
            m_messageSink(tr("New Compound Clip"), error);
        return 0;
    }
    // Select the new instance(s), like after any edit that creates clips.
    QVector<quint64> created;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& t : p->timeline()->data().tracks(type)) {
            for (const ClipData& c : t.clips) {
                if (c.kind == ClipKind::Nested && c.nestedUid == uid)
                    created.append(c.id);
            }
        }
    }
    m_ctx.selection->setClips(created);
    return uid;
}

bool TimelineController::decomposeSelection()
{
    Project* p = project();
    if (p->isEditing())
        return false;
    // One instance per compound clip pair: Project decomposes the linked part with it.
    QVector<quint64> instances;
    QSet<quint64> covered;
    const TimelineData& data = p->timeline()->data();
    for (const quint64 id : effectiveSelection()) {
        const ClipData* c = editops::clipById(data, id);
        if (!c || c->kind != ClipKind::Nested || covered.contains(id))
            continue;
        instances.append(id);
        for (const quint64 linked : editops::linkedClips(data, id))
            covered.insert(linked);
        covered.insert(id);
    }
    if (instances.isEmpty()) {
        if (m_messageSink)
            m_messageSink(tr("Decompose in Place"), tr("Select a compound clip first."));
        return false;
    }
    QVector<quint64> restored;
    for (const quint64 id : std::as_const(instances)) {
        QString error;
        const auto r = p->decomposeCompoundClip(id, &error);
        if (!r) {
            if (m_messageSink)
                m_messageSink(tr("Decompose in Place"), error);
            continue;
        }
        restored += *r;
    }
    if (restored.isEmpty())
        return false;
    m_ctx.selection->setClips(restored);
    return true;
}

bool TimelineController::openSelectedInTimeline()
{
    const TimelineData& data = project()->timeline()->data();
    for (const quint64 id : effectiveSelection()) {
        const ClipData* c = editops::clipById(data, id);
        if (!c || c->kind != ClipKind::Nested)
            continue;
        if (!project()->hasTimeline(c->nestedUid)) {
            if (m_messageSink)
                m_messageSink(tr("Open in Timeline"),
                    tr("The compound clip is offline: its timeline was deleted."));
            return false;
        }
        return project()->openTab(c->nestedUid);
    }
    return false;
}

quint64 TimelineController::newTimeline(const QString& name)
{
    Project* p = project();
    if (p->isEditing())
        return 0;
    QString finalName = name;
    if (finalName.isEmpty() && m_newTimelinePrompt) {
        // Resolve's Create New Timeline: start timecode, name, video and audio track counts.
        NewTimelineOptions suggestion;
        suggestion.name = p->uniqueTimelineName(tr("Timeline"));
        suggestion.startTimecode = p->settings().timelineStart;
        const std::optional<NewTimelineOptions> answer = m_newTimelinePrompt(suggestion);
        if (!answer)
            return 0;
        const QString chosen = answer->name.trimmed();
        const quint64 uid = p->createTimeline(chosen.isEmpty() ? suggestion.name : chosen,
            binOf(m_binProvider), answer->videoTracks, answer->audioTracks, answer->startTimecode);
        p->openTab(uid);
        return uid;
    }
    if (finalName.isEmpty()) {
        const QString suggestion = p->uniqueTimelineName(tr("Timeline"));
        finalName = suggestion;
        if (m_namePrompt) {
            const std::optional<QString> answer
                = m_namePrompt(tr("New Timeline"), tr("Timeline name"), suggestion, tr("Create"));
            if (!answer)
                return 0;
            if (!answer->trimmed().isEmpty())
                finalName = answer->trimmed();
        }
    }
    const quint64 uid = p->createTimeline(finalName, binOf(m_binProvider));
    p->openTab(uid);
    return uid;
}

quint64 TimelineController::duplicateActiveTimeline()
{
    Project* p = project();
    if (p->isEditing())
        return 0;
    const quint64 uid = p->duplicateTimeline(p->activeTimelineUid());
    if (uid)
        p->openTab(uid);
    return uid;
}

bool TimelineController::deleteActiveTimeline()
{
    Project* p = project();
    const quint64 uid = p->activeTimelineUid();
    const auto data = p->timelineData(uid);
    if (!data || p->isEditing())
        return false;
    const int uses = p->timelineUseCount(uid);
    QString text = tr("Delete the timeline \"%1\"? This can be undone.").arg(data->name);
    if (uses > 0)
        text += QLatin1Char('\n')
            + plural(uses, tr("%n clip in another timeline shows it and will become offline."),
                tr("%n clips in other timelines show it and will become offline."));
    if (m_confirmPrompt && !m_confirmPrompt(tr("Delete Timeline"), text))
        return false;
    if (!p->deleteTimeline(uid)) {
        if (m_messageSink)
            m_messageSink(
                tr("Delete Timeline"), tr("A project always keeps at least one timeline."));
        return false;
    }
    return true;
}

void TimelineController::cycleTab(int step)
{
    Project* p = project();
    const QVector<quint64>& tabs = p->tabs();
    if (tabs.size() < 2)
        return;
    const qsizetype index = tabs.indexOf(p->activeTimelineUid());
    const qsizetype next = (index + step + tabs.size()) % tabs.size();
    p->setActiveTimeline(tabs[next]);
}

bool TimelineController::closeActiveTab()
{
    return project()->closeTab(project()->activeTimelineUid());
}

QVector<quint64> TimelineController::copyClipsFrom(
    quint64 sourceUid, const QVector<quint64>& clipIds, int64_t frame, TrackRef track, bool insert)
{
    Project* p = project();
    const auto source = p->timelineData(sourceUid);
    if (!source || clipIds.isEmpty() || p->isEditing())
        return {};
    // A nested clip showing the destination (directly or through others) cannot go there.
    const quint64 target = p->activeTimelineUid();
    QVector<quint64> ids;
    for (const quint64 id : clipIds) {
        const ClipData* c = editops::clipById(*source, id);
        if (c && (c->kind != ClipKind::Nested || p->canNest(c->nestedUid, target)))
            ids.append(id);
    }
    const editops::Clipboard board = editops::copyClips(*source, ids);
    if (board.isEmpty())
        return {};
    QVector<quint64> created;
    timelineedit::runEdit(p, insert ? tr("Insert Clips") : tr("Copy Clips"), [&](TimelineData& d) {
        const int index = std::max(0, track.index);
        created = editops::pasteClips(d, board, std::max<int64_t>(0, frame), index, index, insert);
        return !created.isEmpty();
    });
    if (!created.isEmpty())
        m_ctx.selection->setClips(created);
    return created;
}

} // namespace kk
