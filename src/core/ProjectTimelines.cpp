// SPDX-License-Identifier: GPL-3.0-or-later
// Project, part 2: the timelines of a project (active timeline, tabs, view state), their media
// pool items, nesting and compound clips.
//
// Storage: m_slots lists every timeline in creation order. The active timeline's data lives
// in m_timeline (slot.data == nullptr); every other slot holds an immutable shared copy. A
// change to an inactive timeline replaces its pointer, so undo snapshots (TimelineSet) share
// unchanged timelines and compare them by pointer.
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "core/Logging.h"
#include "core/Project.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace kk {

namespace {

/// (uid, name) of every timeline: a change means timelinesChanged().
QVector<QPair<quint64, QString>> signature(const Project::TimelineSet& set)
{
    QVector<QPair<quint64, QString>> result;
    result.reserve(qsizetype(set.size()));
    for (const auto& t : set)
        result.append({t->uid, t->name});
    return result;
}

MediaItemKind itemKindOf(const TimelineData& data)
{
    return data.multicam ? MediaItemKind::MulticamClip
        : data.compound  ? MediaItemKind::CompoundClip
                         : MediaItemKind::Timeline;
}

} // namespace

// ---- Lookup ----

Project::TimelineSlot* Project::slot(quint64 uid)
{
    for (TimelineSlot& s : m_slots) {
        if (s.uid == uid)
            return &s;
    }
    return nullptr;
}

const Project::TimelineSlot* Project::slot(quint64 uid) const
{
    for (const TimelineSlot& s : m_slots) {
        if (s.uid == uid)
            return &s;
    }
    return nullptr;
}

const TimelineData* Project::lookup(quint64 uid) const
{
    if (uid == m_activeUid)
        return &m_timeline->data();
    const TimelineSlot* s = slot(uid);
    return s ? s->data.get() : nullptr;
}

QVector<quint64> Project::timelineUids() const
{
    QVector<quint64> result;
    result.reserve(qsizetype(m_slots.size()));
    for (const TimelineSlot& s : m_slots)
        result.append(s.uid);
    return result;
}

bool Project::hasTimeline(quint64 uid) const
{
    return slot(uid) != nullptr;
}

std::shared_ptr<const TimelineData> Project::timelineData(quint64 uid) const
{
    if (uid == m_activeUid)
        return m_timeline->plainSnapshot();
    const TimelineSlot* s = slot(uid);
    return s ? s->data : nullptr;
}

QString Project::timelineItemId(quint64 uid) const
{
    for (const MediaItem& item : m_mediaPool->items()) {
        if (item.isTimeline() && item.timelineUid == uid)
            return item.id;
    }
    return {};
}

Project::TimelineSet Project::currentSet() const
{
    TimelineSet set;
    set.reserve(m_slots.size());
    for (const TimelineSlot& s : m_slots)
        set.push_back(timelineData(s.uid));
    return set;
}

void Project::replaceTimeline(quint64 uid, std::shared_ptr<const TimelineData> data)
{
    if (uid == m_activeUid) {
        // Inside an edit: endEdit() notifies (no signal per replaced timeline).
        m_timeline->mutableData() = *data;
        return;
    }
    if (TimelineSlot* s = slot(uid))
        s->data = std::move(data);
}

compound::TimelineLookup Project::timelineLookup() const
{
    return [this](quint64 uid) { return lookup(uid); };
}

NestedTimelines Project::nestedTimelinesFor(const TimelineData& data) const
{
    // Render snapshots carry every timeline their Nested clips reach. The inactive ones are
    // shared pointers already, so building the map costs one hash insert per timeline. The
    // walk starts at @p data itself (not its uid): the source viewer's private timelines
    // (uid 0) reach project timelines too.
    NestedTimelines nested;
    const compound::TimelineLookup lk = timelineLookup();
    QVector<quint64> reach;
    QSet<quint64> seen {data.uid};
    for (const quint64 child : compound::directReferences(data)) {
        if (seen.contains(child))
            continue;
        seen.insert(child);
        reach.append(child);
        for (const quint64 deeper : compound::reachableTimelines(child, lk)) {
            if (!seen.contains(deeper)) {
                seen.insert(deeper);
                reach.append(deeper);
            }
        }
    }
    if (reach.isEmpty())
        return nested;
    auto map = std::make_shared<NestedTimelineMap>();
    for (const quint64 uid : std::as_const(reach)) {
        if (uid == data.uid && uid != 0)
            continue; // cannot happen without a cycle; never render the root inside itself
        auto d = timelineData(uid);
        if (!d)
            continue;
        if (d->multicam) {
            // One entry per angle (multicamViewKey()); cached until the timeline changes.
            AngleViews& cache = m_angleViews[uid];
            if (cache.source != d) {
                cache.source = d;
                cache.views.clear();
                for (int a = 0; a < multicam::angleCount(*d); ++a)
                    cache.views.append(
                        std::make_shared<const TimelineData>(multicam::angleView(*d, a)));
            }
            for (qsizetype a = 0; a < cache.views.size(); ++a)
                map->insert(multicamViewKey(uid, int(a)), cache.views[a]);
        }
        map->insert(uid, std::move(d));
    }
    nested.map = std::move(map);
    return nested;
}

void Project::installResolver()
{
    m_timeline->setNestedResolver(
        [this](const TimelineData& data) { return nestedTimelinesFor(data); });
}

std::shared_ptr<const TimelineData> Project::renderSnapshot(quint64 uid) const
{
    if (uid > kTimelineUidMask) {
        // An angle view key (multicamViewKey()): that angle of the multicam timeline alone.
        const auto base = timelineData(uid & kTimelineUidMask);
        if (!base)
            return nullptr;
        auto view = std::make_shared<TimelineData>(multicam::angleView(*base, int(uid >> 40) - 1));
        view->nested = nestedTimelinesFor(*view);
        return view;
    }
    if (uid == m_activeUid)
        return m_timeline->snapshot();
    const auto plain = timelineData(uid);
    if (!plain)
        return nullptr;
    NestedTimelines nested = nestedTimelinesFor(*plain);
    if (!nested.map)
        return plain;
    auto copy = std::make_shared<TimelineData>(*plain);
    copy->nested = std::move(nested);
    return copy;
}

// ---- Derived data ----

void Project::syncDerived()
{
    // 0. Callers may assign a whole TimelineData to mutableData(): the uid stays the slot's.
    if (m_timeline->data().uid != m_activeUid)
        m_timeline->mutableData().uid = m_activeUid;
    const compound::TimelineLookup lk = [this](quint64 uid) { return lookup(uid); };
    // 1. Nested clips know the length of what they show (trim limits, like media).
    for (TimelineSlot& s : m_slots) {
        if (!compound::nestedSourcesOutdated(*lookup(s.uid), lk))
            continue;
        if (s.uid == m_activeUid) {
            compound::syncNestedSources(m_timeline->mutableData(), lk);
        } else {
            auto copy = std::make_shared<TimelineData>(*s.data);
            compound::syncNestedSources(*copy, lk);
            s.data = std::move(copy);
        }
    }
    // 2. One media pool item per timeline. The item's name is authoritative (renaming in the
    //    media pool renames the timeline), the info is derived from the timeline.
    QHash<quint64, QString> itemOf;
    QStringList orphans;
    for (const MediaItem& item : m_mediaPool->items()) {
        if (!item.isTimeline())
            continue;
        if (!slot(item.timelineUid) || itemOf.contains(item.timelineUid))
            orphans.append(item.id);
        else
            itemOf.insert(item.timelineUid, item.id);
    }
    for (const QString& id : std::as_const(orphans))
        m_mediaPool->removeItem(id);
    for (const TimelineSlot& s : m_slots) {
        const TimelineData* data = lookup(s.uid);
        const auto it = itemOf.constFind(s.uid);
        if (it == itemOf.constEnd()) {
            MediaItem item;
            item.kind = itemKindOf(*data);
            item.timelineUid = s.uid;
            item.name = data->name;
            item.info = compound::timelineInfo(*data);
            m_mediaPool->addItem(std::move(item));
            continue;
        }
        const MediaItem* item = m_mediaPool->item(*it);
        if (!item->name.isEmpty() && item->name != data->name) {
            auto copy = std::make_shared<TimelineData>(*data);
            copy->name = item->name;
            replaceTimeline(s.uid, std::move(copy));
            data = lookup(s.uid);
        }
        m_mediaPool->updateItem(*it, data->name, compound::timelineInfo(*data));
    }
}

void Project::notifyNestedChanges(const TimelineSet& before)
{
    // Inactive timelines that changed and are shown inside the active one: its render
    // snapshot must be rebuilt.
    QSet<quint64> changed;
    for (const auto& t : before) {
        if (t->uid != m_activeUid && timelineData(t->uid) != t)
            changed.insert(t->uid);
    }
    if (!changed.isEmpty()) {
        const QVector<quint64> reach = compound::reachableTimelines(
            m_activeUid, [this](quint64 uid) { return lookup(uid); });
        if (std::any_of(reach.begin(), reach.end(),
                [&changed](quint64 uid) { return changed.contains(uid); }))
            m_timeline->nestedContentChanged();
    }
    if (signature(before) != signature(currentSet()))
        emit timelinesChanged();
}

void Project::activateSlot(quint64 uid, bool storeCurrent)
{
    if (storeCurrent) {
        if (TimelineSlot* current = slot(m_activeUid)) {
            current->data = m_timeline->plainSnapshot();
            current->playhead = m_timeline->playhead();
        }
    }
    TimelineSlot* next = slot(uid);
    m_activeUid = uid;
    std::shared_ptr<const TimelineData> data = std::move(next->data);
    next->data = nullptr;
    m_timeline->setSharedData(std::move(data));
    m_timeline->setPlayhead(next->playhead);
}

void Project::applySet(const TimelineSet& set)
{
    if (set.empty())
        return;
    const TimelineSet before = currentSet();
    QSet<quint64> keep;
    for (const auto& t : set)
        keep.insert(t->uid);
    const bool activeKept = keep.contains(m_activeUid);
    const quint64 oldActive = m_activeUid;

    std::vector<TimelineSlot> timelineSlots;
    timelineSlots.reserve(set.size());
    for (const auto& t : set) {
        const TimelineSlot* existing = slot(t->uid);
        TimelineSlot s = existing ? *existing : TimelineSlot {t->uid, nullptr, 0, {}};
        if (activeKept && t->uid == m_activeUid) {
            s.data = nullptr;
            if (m_timeline->plainSnapshot() != t && !(m_timeline->data() == *t))
                m_timeline->setSharedData(t);
        } else {
            s.data = t;
        }
        timelineSlots.push_back(std::move(s));
    }
    m_slots = std::move(timelineSlots);
    m_nextTimelineUid = std::max(m_nextTimelineUid,
        (*std::max_element(set.begin(), set.end(),
             [](const auto& a, const auto& b) {
                 return a->uid < b->uid;
             }))->uid
            + 1);

    bool tabs = false;
    if (!activeKept) {
        // The active timeline no longer exists (undo of its creation): show an open tab, else
        // the first timeline.
        quint64 next = 0;
        for (const quint64 uid : std::as_const(m_tabs)) {
            if (keep.contains(uid)) {
                next = uid;
                break;
            }
        }
        if (next == 0)
            next = set.front()->uid;
        emit activeTimelineAboutToChange(oldActive);
        activateSlot(next, false);
    }
    const qsizetype tabCount = m_tabs.size();
    m_tabs.erase(std::remove_if(m_tabs.begin(), m_tabs.end(),
                     [&keep](quint64 uid) { return !keep.contains(uid); }),
        m_tabs.end());
    if (!m_tabs.contains(m_activeUid))
        m_tabs.append(m_activeUid);
    tabs = tabCount != m_tabs.size() || !activeKept;
    m_timeline->notifyChanged();
    notifyNestedChanges(before);
    if (tabs)
        emit tabsChanged();
    if (!activeKept)
        emit activeTimelineChanged(m_activeUid);
}

void Project::resetTimelines(
    std::vector<TimelineSlot> timelineSlots, quint64 active, QVector<quint64> tabs)
{
    // Before anything is replaced: views store their state into the old timeline's slot.
    emit activeTimelineAboutToChange(m_activeUid);
    m_angleViews.clear();
    m_slots = std::move(timelineSlots);
    m_nextTimelineUid = 1;
    for (const TimelineSlot& s : m_slots)
        m_nextTimelineUid = std::max(m_nextTimelineUid, s.uid + 1);
    m_activeUid = 0;
    activateSlot(active, false);
    m_tabs = std::move(tabs);
    if (!m_tabs.contains(active))
        m_tabs.append(active);
    syncDerived();
    m_timeline->notifyChanged();
    emit timelinesChanged();
    emit tabsChanged();
    emit activeTimelineChanged(m_activeUid);
}

void Project::addTimeline(TimelineData data, const QString& binId)
{
    const quint64 uid = data.uid;
    MediaItem item;
    item.kind = itemKindOf(data);
    item.timelineUid = uid;
    item.name = data.name;
    item.binId = binId;
    item.info = compound::timelineInfo(data);
    m_slots.push_back({uid, std::make_shared<const TimelineData>(std::move(data)), 0, {}});
    m_mediaPool->addItem(std::move(item));
}

// ---- Active timeline and tabs ----

bool Project::setActiveTimeline(quint64 uid)
{
    if (isEditing() || !slot(uid))
        return false;
    if (uid != m_activeUid) {
        const quint64 old = m_activeUid;
        emit activeTimelineAboutToChange(old);
        activateSlot(uid, true);
        if (!m_tabs.contains(uid)) {
            const qsizetype at = m_tabs.indexOf(old);
            m_tabs.insert(at < 0 ? m_tabs.size() : at + 1, uid);
        }
        emit tabsChanged();
        emit activeTimelineChanged(uid);
    } else if (!m_tabs.contains(uid)) {
        m_tabs.append(uid);
        emit tabsChanged();
    }
    return true;
}

bool Project::openTab(quint64 uid)
{
    return setActiveTimeline(uid);
}

bool Project::closeTab(quint64 uid)
{
    const qsizetype index = m_tabs.indexOf(uid);
    if (index < 0 || m_tabs.size() <= 1 || isEditing())
        return false;
    if (uid == m_activeUid) {
        const quint64 next = index + 1 < m_tabs.size() ? m_tabs[index + 1] : m_tabs[index - 1];
        if (!setActiveTimeline(next))
            return false;
    }
    m_tabs.removeAll(uid);
    emit tabsChanged();
    return true;
}

void Project::moveTab(int from, int to)
{
    if (from < 0 || to < 0 || from >= m_tabs.size() || to >= m_tabs.size() || from == to)
        return;
    m_tabs.move(from, to);
    emit tabsChanged();
}

TimelineViewState Project::viewState(quint64 uid) const
{
    const TimelineSlot* s = slot(uid);
    return s ? s->view : TimelineViewState {};
}

void Project::setViewState(quint64 uid, const TimelineViewState& state)
{
    if (TimelineSlot* s = slot(uid))
        s->view = state;
}

// ---- Timeline management ----

QString Project::uniqueTimelineName(const QString& base) const
{
    QSet<QString> names;
    for (const TimelineSlot& s : m_slots)
        names.insert(lookup(s.uid)->name);
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!names.contains(candidate))
            return candidate;
    }
}

quint64 Project::createTimeline(const QString& name, const QString& binId)
{
    TimelineData t = makeDefaultTimeline(name.isEmpty() ? uniqueTimelineName(tr("Timeline")) : name,
        m_format.fps, m_format.width, m_format.height);
    t.uid = m_nextTimelineUid++;
    t.retimeProcess = m_format.retimeProcess;
    const quint64 uid = t.uid;
    beginEdit(tr("New Timeline"));
    addTimeline(std::move(t), binId);
    endEdit();
    return uid;
}

quint64 Project::duplicateTimeline(quint64 uid)
{
    const auto source = timelineData(uid);
    if (!source)
        return 0;
    TimelineData t = *source;
    t.uid = m_nextTimelineUid++;
    t.name = tr("%1 copy").arg(source->name);
    const MediaItem* item = m_mediaPool->item(timelineItemId(uid));
    const quint64 newUid = t.uid;
    beginEdit(tr("Duplicate Timeline"));
    addTimeline(std::move(t), item ? item->binId : QString());
    endEdit();
    return newUid;
}

bool Project::renameTimeline(quint64 uid, const QString& name)
{
    const auto source = timelineData(uid);
    if (!source || name.isEmpty())
        return false;
    beginEdit(tr("Rename Timeline"));
    auto copy = std::make_shared<TimelineData>(*source);
    copy->name = name;
    replaceTimeline(uid, std::move(copy));
    m_mediaPool->renameItem(timelineItemId(uid), name);
    endEdit();
    return true;
}

bool Project::deleteTimeline(quint64 uid)
{
    const auto data = timelineData(uid);
    if (!data || isEditing())
        return false;
    if (!data->compound) {
        const auto regular = std::count_if(m_slots.begin(), m_slots.end(),
            [this](const TimelineSlot& s) { return !lookup(s.uid)->compound; });
        if (regular <= 1)
            return false; // a project always keeps one timeline
    }
    if (uid == m_activeUid) {
        // Another open tab, else the first other regular timeline.
        quint64 next = 0;
        const qsizetype index = m_tabs.indexOf(uid);
        if (m_tabs.size() > 1)
            next = index + 1 < m_tabs.size() ? m_tabs[index + 1] : m_tabs[index - 1];
        for (const TimelineSlot& s : m_slots) {
            if (next == 0 && s.uid != uid && !lookup(s.uid)->compound)
                next = s.uid;
        }
        if (next == 0 || !setActiveTimeline(next))
            return false;
    }
    beginEdit(data->compound ? tr("Delete Compound Clip") : tr("Delete Timeline"));
    m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
                      [uid](const TimelineSlot& s) { return s.uid == uid; }),
        m_slots.end());
    m_mediaPool->removeItem(timelineItemId(uid));
    endEdit();
    if (m_tabs.removeAll(uid) > 0)
        emit tabsChanged();
    return true;
}

int Project::timelineUseCount(quint64 uid) const
{
    int count = 0;
    for (const TimelineSlot& s : m_slots) {
        const TimelineData& t = *lookup(s.uid);
        for (TrackType type : {TrackType::Video, TrackType::Audio}) {
            for (const TrackData& track : t.tracks(type)) {
                count += int(
                    std::count_if(track.clips.begin(), track.clips.end(), [uid](const ClipData& c) {
                        return c.kind == ClipKind::Nested && c.nestedUid == uid;
                    }));
            }
        }
    }
    return count;
}

bool Project::setTimelineFormat(
    quint64 uid, bool useProjectSettings, int width, int height, Rational fps)
{
    const auto source = timelineData(uid);
    if (!source)
        return false;
    auto copy = std::make_shared<TimelineData>(*source);
    copy->useProjectSettings = useProjectSettings;
    if (useProjectSettings) {
        copy->width = m_format.width;
        copy->height = m_format.height;
        copy->fps = m_format.fps;
    } else {
        copy->width = std::max(16, width);
        copy->height = std::max(16, height);
        if (fps.isValid())
            copy->fps = fps;
    }
    if (*copy == *source)
        return true;
    // TODO(core): changing fps does not rescale existing clip positions yet.
    beginEdit(tr("Timeline Settings"));
    replaceTimeline(uid, std::move(copy));
    endEdit();
    return true;
}

// ---- Nesting / compound clips ----

bool Project::canNest(quint64 inner, quint64 outer) const
{
    return compound::canNest(inner, outer, [this](quint64 uid) { return lookup(uid); });
}

quint64 Project::createCompoundClip(
    const QVector<quint64>& clipIds, const QString& name, const QString& binId, QString* error)
{
    TimelineData parent = m_timeline->data();
    const quint64 uid = m_nextTimelineUid;
    const QString finalName = name.isEmpty() ? uniqueTimelineName(tr("Compound Clip")) : name;
    std::optional<compound::CollapseResult> result
        = compound::collapse(parent, clipIds, uid, finalName, error);
    if (!result)
        return 0;
    ++m_nextTimelineUid;
    beginEdit(tr("New Compound Clip"));
    m_timeline->mutableData() = std::move(parent);
    addTimeline(std::move(result->nested), binId);
    // The instances point at their media pool item too (name, reveal in the media pool).
    const QString itemId = timelineItemId(uid);
    for (const quint64 id : std::as_const(result->instanceIds)) {
        if (ClipData* c = editops::clipById(m_timeline->mutableData(), id))
            c->mediaId = itemId;
    }
    endEdit();
    return uid;
}

std::optional<QVector<quint64>> Project::decomposeCompoundClip(quint64 clipId, QString* error)
{
    const TimelineData& data = m_timeline->data();
    const ClipData* clip = editops::clipById(data, clipId);
    if (!clip || clip->kind != ClipKind::Nested) {
        if (error)
            *error = tr("Select a compound clip to decompose.");
        return std::nullopt;
    }
    const auto nested = timelineData(clip->nestedUid);
    if (!nested) {
        if (error)
            *error = tr("The compound clip is offline (its timeline was deleted).");
        return std::nullopt;
    }
    // The linked audio/video part of the same compound clip is decomposed with it.
    QVector<quint64> ids {clipId};
    for (const quint64 id : editops::linkedClips(data, clipId)) {
        const ClipData* c = editops::clipById(data, id);
        if (id != clipId && c && c->kind == ClipKind::Nested && c->nestedUid == clip->nestedUid)
            ids.append(id);
    }
    TimelineData copy = data;
    std::optional<QVector<quint64>> restored = compound::decompose(copy, ids, *nested, error);
    if (!restored)
        return std::nullopt;
    beginEdit(tr("Decompose in Place"));
    m_timeline->mutableData() = std::move(copy);
    endEdit();
    return restored;
}

} // namespace kk
