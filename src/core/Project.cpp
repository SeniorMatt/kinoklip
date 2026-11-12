// SPDX-License-Identifier: GPL-3.0-or-later
// Project implementation: snapshot undo commands, settings, JSON save/load.
// Timelines, tabs and compound clips: ProjectTimelines.cpp.
#include "core/Project.h"

#include "core/AudioTransitions.h"
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/FileParams.h"
#include "core/Logging.h"
#include "core/Retime.h"
#include "core/Serialization.h"
#include "core/UndoSnapshots.h"
#include "media/MediaSource.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr Rational kDefaultFps {24, 1}; // Resolve's default for new projects
constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

/// One undo step: before/after states of every timeline, the media pool and the project
/// format. The timelines are stored packed (core/UndoSnapshots.h): the before state is
/// packed against the after state of the step below it and the after state against the
/// before state, so a step only owns the clips it changed. The first redo() is skipped
/// because the change has already been applied live.
class SnapshotCommand : public QUndoCommand
{
public:
    using PackedSet = std::vector<std::shared_ptr<const undosnap::PackedTimeline>>;

    SnapshotCommand(Project* project, const QString& label, quint64 primary,
        const Project::TimelineSet& before, const Project::TimelineSet& after,
        const SnapshotCommand* below, MediaPoolState poolBefore, MediaPoolState poolAfter,
        Project::Format formatBefore, Project::Format formatAfter)
        : QUndoCommand(label)
        , m_project(project)
        , m_primary(primary)
        , m_poolBefore(std::move(poolBefore))
        , m_poolAfter(std::move(poolAfter))
        , m_formatBefore(formatBefore)
        , m_formatAfter(formatAfter)
    {
        m_before = packSet(before, below ? &below->m_after : nullptr);
        m_after = packSet(after, &m_before);
    }

    void undo() override
    {
        m_project->restoreProjectState(
            unpackSet(m_before), m_poolBefore, m_formatBefore, m_primary);
        emit m_project->editCommitted(false);
    }

    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_project->restoreProjectState(unpackSet(m_after), m_poolAfter, m_formatAfter, m_primary);
        emit m_project->editCommitted(false);
    }

private:
    /// Packs every timeline of @p set against the entry of the same uid in @p reference.
    static PackedSet packSet(const Project::TimelineSet& set, const PackedSet* reference)
    {
        PackedSet out;
        out.reserve(set.size());
        for (const auto& t : set) {
            std::shared_ptr<const undosnap::PackedTimeline> ref;
            if (reference) {
                for (const auto& r : *reference) {
                    if (r && r->shell && r->shell->uid == t->uid) {
                        ref = r;
                        break;
                    }
                }
            }
            out.push_back(undosnap::pack(t, ref));
        }
        return out;
    }

    static Project::TimelineSet unpackSet(const PackedSet& set)
    {
        Project::TimelineSet out;
        out.reserve(set.size());
        for (const auto& p : set)
            out.push_back(undosnap::unpack(*p));
        return out;
    }

    Project* m_project;
    quint64 m_primary;
    PackedSet m_before;
    PackedSet m_after;
    MediaPoolState m_poolBefore;
    MediaPoolState m_poolAfter;
    Project::Format m_formatBefore;
    Project::Format m_formatAfter;
    bool m_firstRedo = true;
};

/// True if two sets hold the same timelines with equal contents (pointer equality first:
/// inactive timelines are shared between snapshots, so most entries compare in O(1)).
bool sameSet(const Project::TimelineSet& a, const Project::TimelineSet& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i] && !(*a[i] == *b[i]))
            return false;
    }
    return true;
}

QJsonObject fpsToJson(Rational fps)
{
    return QJsonObject {{QLatin1String("num"), fps.num}, {QLatin1String("den"), fps.den}};
}

bool writeJson(const QString& path, const QJsonObject& root, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace

Project::Project(QObject* parent)
    : QObject(parent)
    , m_mediaPool(new MediaPool(this))
    , m_links(new MediaLinks(this))
    , m_gallery(new GradeGallery(this))
    , m_timeline(new Timeline(this))
    , m_undoStack(new QUndoStack(this))
{
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this] { emitDirtyIfChanged(); });
    connect(m_links, &MediaLinks::changed, this, [this] { setSettingsDirty(true); });
    connect(m_gallery, &GradeGallery::changed, this, [this] {
        if (!m_galleryLoading)
            setSettingsDirty(true);
    });
    installResolver();
    newProject();
}

Project::~Project() = default;

// ---- Settings & document state ----

void Project::setCacheSettings(const MediaCacheSettings& settings)
{
    if (settings == m_cacheSettings)
        return;
    m_cacheSettings = settings;
    setSettingsDirty(true);
    emit cacheSettingsChanged();
}

ProjectSettings Project::settings() const
{
    return ProjectSettings {m_name, m_format.width, m_format.height, m_format.fps,
        m_format.retimeProcess, m_format.timelineStart, standardTransitionId(false),
        standardTransitionId(true), standardTransitionFrames()};
}

namespace {
QString defaultTransition(bool audio)
{
    return audio ? audiotransitions::defaultId() : QStringLiteral("cross_dissolve");
}
} // namespace

int64_t scaledTransitionFrames(int64_t frames, Rational from, Rational to)
{
    // The same duration at the new frame rate (0 = follows the preference, nothing to scale).
    if (frames <= 0 || from == to || !from.isValid() || !to.isValid())
        return frames;
    return std::max<int64_t>(1, std::llround(double(frames) * to.toDouble() / from.toDouble()));
}

QString Project::standardTransitionId(bool audio) const
{
    const QString& id = audio ? m_format.stdAudioTransition : m_format.stdVideoTransition;
    return id.isEmpty() ? defaultTransition(audio) : id;
}

int64_t Project::standardTransitionFrames() const
{
    if (m_format.stdTransitionFrames > 0)
        return m_format.stdTransitionFrames;
    return std::max<int64_t>(
        1, EditBehaviour::frames(EditBehaviour::fromSettings().transitionSeconds, m_format.fps));
}

bool Project::setStandardTransition(bool audio, const QString& effectId)
{
    const EffectDescriptor* desc = EffectRegistry::instance().find(effectId);
    const EffectCategory wanted
        = audio ? EffectCategory::AudioTransition : EffectCategory::Transition;
    if (!desc || desc->category != wanted)
        return false;
    if (standardTransitionId(audio) == effectId)
        return true;
    beginEdit(tr("Set as Standard Transition"));
    (audio ? m_format.stdAudioTransition : m_format.stdVideoTransition)
        = effectId == defaultTransition(audio) ? QString() : effectId;
    endEdit();
    setSettingsDirty(true);
    return true;
}

void Project::setSettings(const ProjectSettings& settings)
{
    setName(settings.name);
    const Rational fps = settings.fps.isValid() ? settings.fps : kDefaultFps;
    // An unchanged starting timecode keeps its HH:MM:SS:FF when only the rate changes.
    const int64_t start = settings.timelineStart == m_format.timelineStart
        ? convertTimecode(m_format.timelineStart, m_format.fps, fps)
        : settings.timelineStart;
    // The default ids are stored as empty, so old projects stay "follow the defaults".
    const auto storedId = [](const QString& wanted, bool audio) {
        return wanted == defaultTransition(audio) ? QString() : wanted;
    };
    const Format format {settings.width, settings.height, fps,
        settings.retimeProcess == RetimeProcess::ProjectDefault ? RetimeProcess::Nearest
                                                                : settings.retimeProcess,
        std::max<int64_t>(0, start), storedId(settings.stdVideoTransition, false),
        storedId(settings.stdAudioTransition, true),
        settings.stdTransitionFrames == standardTransitionFrames()
            ? scaledTransitionFrames(m_format.stdTransitionFrames, m_format.fps, fps)
            : std::max<int64_t>(0, settings.stdTransitionFrames)};
    if (format == m_format)
        return;
    const Rational oldFps = m_format.fps;
    // TODO(core): changing fps does not rescale existing clip positions yet.
    beginEdit(tr("Project Settings"));
    m_format = format;
    for (const quint64 uid : timelineUids()) {
        const auto data = timelineData(uid);
        auto copy = std::make_shared<TimelineData>(*data);
        copy->retimeProcess = format.retimeProcess; // project-wide, every timeline
        if (data->useProjectSettings) {
            copy->width = format.width;
            copy->height = format.height;
            copy->fps = format.fps;
            // The starting timecode keeps its HH:MM:SS:FF at the new rate.
            copy->startTimecode = convertTimecode(data->startTimecode, oldFps, format.fps);
        }
        if (*copy == *data)
            continue;
        replaceTimeline(uid, std::move(copy));
    }
    endEdit();
}

void Project::setName(const QString& name)
{
    if (name == m_name)
        return;
    m_name = name;
    setSettingsDirty(true);
    emit nameChanged(m_name);
}

bool Project::isDirty() const
{
    return m_settingsDirty || !m_undoStack->isClean();
}

void Project::setFilePath(const QString& path)
{
    if (path == m_filePath)
        return;
    m_filePath = path;
    emit filePathChanged(m_filePath);
}

void Project::setSettingsDirty(bool dirty)
{
    m_settingsDirty = dirty;
    emitDirtyIfChanged();
}

void Project::emitDirtyIfChanged()
{
    const bool dirty = isDirty();
    if (dirty == m_lastDirty)
        return;
    m_lastDirty = dirty;
    emit dirtyChanged(dirty);
}

void Project::newProject()
{
    m_editDepth = 0;
    m_undoStack->clear();
    m_name = ProjectSettings {}.name;
    // New projects: 24 fps and timelines starting at 01:00:00:00, as in Resolve (spec 25.12).
    m_format = Format {kDefaultWidth, kDefaultHeight, kDefaultFps, RetimeProcess::Nearest,
        oneHourTimecode(kDefaultFps), {}, {}, 0};
    // New projects start from the Preferences > Editing duration; later preference changes
    // do not reach them.
    m_format.stdTransitionFrames = standardTransitionFrames();
    m_mediaPool->clear();
    m_links->clear();
    {
        const QScopedValueRollback guard(m_galleryLoading, true);
        m_gallery->clear();
    }
    m_cacheSettings = {};
    m_binViews.clear();
    TimelineData first
        = makeDefaultTimeline(tr("Timeline 1"), kDefaultFps, kDefaultWidth, kDefaultHeight);
    first.uid = 1;
    first.startTimecode = m_format.timelineStart;
    std::vector<TimelineSlot> timelineSlots;
    timelineSlots.push_back(
        {first.uid, std::make_shared<const TimelineData>(std::move(first)), 0, {}});
    resetTimelines(std::move(timelineSlots), 1, {1});
    setFilePath({});
    m_settingsDirty = false;
    emitDirtyIfChanged();
    emit nameChanged(m_name);
    emit cacheSettingsChanged();
    emit projectReset();
}

// ---- Save / load ----

Project::SaveSnapshot Project::captureSave() const
{
    SaveSnapshot snap;
    snap.name = m_name;
    snap.format = m_format;
    snap.pool = m_mediaPool->state();
    snap.timelines = currentSet();
    snap.views.reserve(qsizetype(m_slots.size()));
    for (const TimelineSlot& s : m_slots) {
        const bool active = s.uid == m_activeUid;
        snap.views.append({s.uid, active ? m_timeline->playhead() : s.playhead, s.view});
    }
    snap.activeUid = m_activeUid;
    snap.tabs = m_tabs;
    snap.mediaLinks = m_links->toJson(snap.pool);
    if (!m_gallery->stills().isEmpty())
        snap.gallery = m_gallery->toJson();
    snap.cacheSettings = m_cacheSettings.toJson();
    snap.binViews = m_binViews;
    return snap;
}

QJsonObject Project::toJson(const SaveSnapshot& snap, const QString& path)
{
    const QDir projectDir = QFileInfo(path).absoluteDir();
    QJsonObject root;
    root[QLatin1String("formatVersion")] = serialization::kFormatVersion;
    root[QLatin1String("generator")] = QStringLiteral(KK_APP_NAME " " KK_APP_VERSION);
    root[QLatin1String("project")] = QJsonObject {{QLatin1String("name"), snap.name},
        {QLatin1String("width"), snap.format.width}, {QLatin1String("height"), snap.format.height},
        {QLatin1String("fps"), fpsToJson(snap.format.fps)},
        {QLatin1String("retimeProcess"), retime::processToString(snap.format.retimeProcess)},
        {QLatin1String("timelineStart"), qint64(snap.format.timelineStart)}};
    {
        QJsonObject p = root.value(QLatin1String("project")).toObject();
        if (!snap.format.stdVideoTransition.isEmpty())
            p[QLatin1String("stdVideoTransition")] = snap.format.stdVideoTransition;
        if (!snap.format.stdAudioTransition.isEmpty())
            p[QLatin1String("stdAudioTransition")] = snap.format.stdAudioTransition;
        if (snap.format.stdTransitionFrames > 0)
            p[QLatin1String("stdTransitionFrames")] = qint64(snap.format.stdTransitionFrames);
        root[QLatin1String("project")] = p;
    }
    root[QLatin1String("mediaPool")] = serialization::mediaPoolToJson(snap.pool, projectDir);
    QJsonArray timelines;
    QJsonArray views;
    for (size_t i = 0; i < snap.timelines.size(); ++i) {
        const TimelineData& t = *snap.timelines[i];
        if (fileparams::hasFileParams(t)) {
            // File parameters (LUTs) are stored absolute + relative to the project.
            TimelineData copy = t;
            fileparams::encodeForSave(copy, projectDir);
            timelines.append(serialization::timelineToJson(copy));
        } else {
            timelines.append(serialization::timelineToJson(t));
        }
        const SaveSnapshot::View& v = snap.views[qsizetype(i)];
        views.append(QJsonObject {{QLatin1String("uid"), qint64(v.uid)},
            {QLatin1String("playhead"), qint64(v.playhead)},
            {QLatin1String("pixelsPerFrame"), v.view.pixelsPerFrame},
            {QLatin1String("scrollX"), v.view.scrollX},
            {QLatin1String("scrollY"), v.view.scrollY}});
    }
    root[QLatin1String("timelines")] = timelines;
    root[QLatin1String("activeTimeline")] = qint64(snap.activeUid);
    QJsonArray tabs;
    for (const quint64 uid : snap.tabs)
        tabs.append(qint64(uid));
    root[QLatin1String("tabs")] = tabs;
    root[QLatin1String("timelineViews")] = views;
    root[QLatin1String("mediaLinks")] = snap.mediaLinks;
    if (!snap.gallery.isEmpty()) // format 16; omitted when empty (older files identical)
        root[QLatin1String("gallery")] = snap.gallery;
    root[QLatin1String("cacheSettings")] = snap.cacheSettings;
    if (!snap.binViews.isEmpty()) {
        QJsonArray binViews;
        // Sorted keys: saving the same project twice writes the same file.
        QStringList keys = snap.binViews.keys();
        keys.sort();
        for (const QString& key : std::as_const(keys)) {
            const BinViewState& v = snap.binViews[key];
            QJsonObject o {{QLatin1String("bin"), key}};
            if (!v.mode.isEmpty())
                o[QLatin1String("mode")] = v.mode;
            if (!v.sortKey.isEmpty())
                o[QLatin1String("sort")] = v.sortKey;
            if (v.descending)
                o[QLatin1String("descending")] = true;
            binViews.append(o);
        }
        root[QLatin1String("binViews")] = binViews;
    }
    return root;
}

bool Project::fromJson(const QJsonObject& root, int version, const QDir& projectDir,
    const QString& fallbackName, QString* error)
{
    std::vector<TimelineSlot> timelineSlots;
    QVector<TimelineData> timelines;
    if (version < 3) {
        // Versions 1 and 2: a single "timeline" object. It becomes timeline uid 1; its media
        // pool item is created by syncDerived() below (in the root bin, like Resolve's
        // "Timeline 1"), so the project opens exactly as before.
        TimelineData t;
        QString why;
        if (!serialization::timelineFromJson(
                root.value(QLatin1String("timeline")).toObject(), t, &why)) {
            *error = why;
            return false;
        }
        t.uid = 1;
        timelines.append(std::move(t));
    } else {
        for (const QJsonValue& v : root.value(QLatin1String("timelines")).toArray()) {
            TimelineData t;
            QString why;
            if (!serialization::timelineFromJson(v.toObject(), t, &why)) {
                *error = why;
                return false;
            }
            fileparams::decodeAfterLoad(t, projectDir);
            timelines.append(std::move(t));
        }
        if (timelines.isEmpty()) {
            *error = tr("the project has no timeline");
            return false;
        }
    }
    // Unique, non-zero uids (a damaged file may repeat them; references to a repaired uid
    // then show the first timeline with that uid, the others become offline).
    QSet<quint64> used;
    quint64 maxUid = 0;
    for (const TimelineData& t : timelines)
        maxUid = std::max(maxUid, t.uid);
    for (TimelineData& t : timelines) {
        if (t.uid == 0 || used.contains(t.uid))
            t.uid = ++maxUid;
        used.insert(t.uid);
    }
    for (TimelineData& t : timelines)
        timelineSlots.push_back({t.uid, std::make_shared<const TimelineData>(std::move(t)), 0, {}});

    const QJsonObject project = root.value(QLatin1String("project")).toObject();
    const TimelineData& first = *timelineSlots.front().data;
    Format format {first.width, first.height, first.fps};
    if (project.contains(QLatin1String("width"))) {
        const QJsonObject fps = project.value(QLatin1String("fps")).toObject();
        format.width = project.value(QLatin1String("width")).toInt(format.width);
        format.height = project.value(QLatin1String("height")).toInt(format.height);
        const Rational r {fps.value(QLatin1String("num")).toInt(format.fps.num),
            fps.value(QLatin1String("den")).toInt(format.fps.den)};
        if (r.isValid())
            format.fps = r;
        format.retimeProcess = retime::processFromString(
            project.value(QLatin1String("retimeProcess")).toString(), RetimeProcess::Nearest);
        if (format.retimeProcess == RetimeProcess::ProjectDefault)
            format.retimeProcess = RetimeProcess::Nearest;
        // Format <= 16: no starting timecode (new timelines keep starting at 00:00:00:00).
        format.timelineStart
            = std::max<qint64>(0, project.value(QLatin1String("timelineStart")).toInteger(0));
        // Format <= 17: no standard transitions (Cross Dissolve, the preference's length).
        format.stdVideoTransition = project.value(QLatin1String("stdVideoTransition")).toString();
        format.stdAudioTransition = project.value(QLatin1String("stdAudioTransition")).toString();
        format.stdTransitionFrames
            = std::max<qint64>(0, project.value(QLatin1String("stdTransitionFrames")).toInteger(0));
    }
    for (const QJsonValue& v : root.value(QLatin1String("timelineViews")).toArray()) {
        const QJsonObject o = v.toObject();
        const quint64 uid = quint64(o.value(QLatin1String("uid")).toInteger());
        for (TimelineSlot& s : timelineSlots) {
            if (s.uid != uid)
                continue;
            s.playhead = std::max<qint64>(0, o.value(QLatin1String("playhead")).toInteger());
            s.view.pixelsPerFrame = o.value(QLatin1String("pixelsPerFrame")).toDouble();
            s.view.scrollX = o.value(QLatin1String("scrollX")).toInt();
            s.view.scrollY = o.value(QLatin1String("scrollY")).toInt();
        }
    }
    const auto exists = [&timelineSlots](quint64 uid) {
        return std::any_of(timelineSlots.begin(), timelineSlots.end(),
            [uid](const TimelineSlot& s) { return s.uid == uid; });
    };
    quint64 active = quint64(root.value(QLatin1String("activeTimeline")).toInteger());
    if (!exists(active))
        active = timelineSlots.front().uid;
    QVector<quint64> tabs;
    for (const QJsonValue& v : root.value(QLatin1String("tabs")).toArray()) {
        const quint64 uid = quint64(v.toInteger());
        if (exists(uid) && !tabs.contains(uid))
            tabs.append(uid);
    }

    m_editDepth = 0;
    m_undoStack->clear();
    m_name = project.value(QLatin1String("name")).toString(fallbackName);
    m_format = format;
    MediaPoolState pool = serialization::mediaPoolFromJson(
        root.value(QLatin1String("mediaPool")).toObject(), projectDir);
    if (version < 11) {
        // Version <= 10 -> 11: no file times were stored (10 is the attrs format without the
        // bins data). The file's modification time is read
        // now (one stat per item; mediaPoolFromJson already checks every path) and also used as
        // the "date added", the earliest moment the file can have been imported.
        for (MediaItem& m : pool.items) {
            if (m.kind != MediaItemKind::File || m.fileModifiedMs > 0)
                continue;
            const media::MediaSource source = media::MediaSource::resolve(m.path);
            if (source.exists())
                m.fileModifiedMs = source.modifiedMs();
            if (m.addedMs == 0)
                m.addedMs = m.fileModifiedMs;
        }
    }
    m_mediaPool->setState(std::move(pool));
    m_binViews.clear();
    for (const QJsonValue& v : root.value(QLatin1String("binViews")).toArray()) {
        const QJsonObject o = v.toObject();
        BinViewState state;
        state.mode = o.value(QLatin1String("mode")).toString();
        state.sortKey = o.value(QLatin1String("sort")).toString();
        state.descending = o.value(QLatin1String("descending")).toBool(false);
        if (!(state == BinViewState {}))
            m_binViews.insert(o.value(QLatin1String("bin")).toString(), state);
    }
    // Version 3 -> 4: no links and default cache settings (both keys are simply absent); the
    // application links proxies found in the proxy folder after loading.
    m_links->fromJson(root.value(QLatin1String("mediaLinks")).toArray());
    {
        // Version <= 15: no "gallery" (an empty array).
        const QScopedValueRollback guard(m_galleryLoading, true);
        m_gallery->fromJson(root.value(QLatin1String("gallery")).toArray());
    }
    m_cacheSettings
        = MediaCacheSettings::fromJson(root.value(QLatin1String("cacheSettings")).toObject());
    resetTimelines(std::move(timelineSlots), active, tabs);
    return true;
}

void Project::setBinViewState(const QString& binKey, const BinViewState& state)
{
    if (state == BinViewState {})
        m_binViews.remove(binKey);
    else
        m_binViews.insert(binKey, state);
}

bool Project::save(const QString& path, QString* error)
{
    if (!saveCopy(path, error))
        return false;
    markSaved(path);
    return true;
}

void Project::markSaved(const QString& path)
{
    setFilePath(QFileInfo(path).absoluteFilePath());
    m_undoStack->setClean();
    m_settingsDirty = false;
    emitDirtyIfChanged();
}

QByteArray Project::documentJson(const QString& path) const
{
    return QJsonDocument(toJson(captureSave(), path)).toJson(QJsonDocument::Indented);
}

bool Project::loadDocument(const QByteArray& json, const QString& documentPath, QString* error)
{
    return loadBytes(json, documentPath, documentPath, error);
}

bool Project::saveCopy(const QString& path, QString* error, const QJsonObject& extra) const
{
    return writeSnapshot(captureSave(), path, error, extra);
}

bool Project::writeSnapshot(
    const SaveSnapshot& snapshot, const QString& path, QString* error, const QJsonObject& extra)
{
    QJsonObject root = toJson(snapshot, path);
    if (!extra.isEmpty())
        root[QLatin1String("autosave")] = extra;
    return writeJson(path, root, error);
}

bool Project::load(const QString& path, QString* error)
{
    return loadFrom(path, path, error);
}

bool Project::loadRecovered(
    const QString& autosavePath, const QString& documentPath, QString* error)
{
    if (!loadFrom(autosavePath, documentPath, error))
        return false;
    setFilePath(documentPath.isEmpty() ? QString() : QFileInfo(documentPath).absoluteFilePath());
    setSettingsDirty(true);
    return true;
}

bool Project::loadFrom(const QString& path, const QString& documentPath, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        qCWarning(lcCore) << "load failed:" << message;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(tr("Cannot open %1: %2").arg(path, file.errorString()));
    return loadBytes(file.readAll(), path, documentPath, error);
}

bool Project::loadBytes(
    const QByteArray& json, const QString& path, const QString& documentPath, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        qCWarning(lcCore) << "load failed:" << message;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject())
        return fail(tr("%1 is not a valid project file: %2").arg(path, parseError.errorString()));

    const QJsonObject root = doc.object();
    if (!root.contains(QLatin1String("formatVersion")))
        return fail(tr("%1 is not a %2 project file").arg(path, QStringLiteral(KK_APP_NAME)));
    const int version = root.value(QLatin1String("formatVersion")).toInt();
    if (version > serialization::kFormatVersion)
        return fail(tr("%1 was created by a newer version (format %2)").arg(path).arg(version));
    // Version 1 -> 2 (keyframe interp -> Bezier handles) is converted per key inside
    // serialization::paramSetFromJson(), which recognises the v1 "interp" field. Versions 1
    // and 2 -> 3 (one "timeline" object -> "timelines") happens in fromJson().
    // Version 3 -> 4 only added clip masks ("masks", absent = none): a version-3 document is a
    // valid version-4 document without masks, and it is written back as version 4.
    // Version 4 -> 5 (retiming): a clip's top-level "speed" becomes one constant segment
    // inside serialization::clipFromJson() (no "retime" object = older format).
    // Versions 4 - 6 -> 7 only added adjustment / generator clips, subtitle tracks and
    // subtitle media items (all optional fields, absent = none): nothing to convert.
    // Versions 5 - 7 -> 8 (multicam): no multicam timelines, Nested clips without
    // "multicamAngle" load as -1 (= the whole nested timeline, exactly what they showed).
    // Versions <= 9 -> 10 only added clip fades and Clip Attributes (optional fields,
    // absent = none): nothing to convert.
    // Versions <= 11 -> 12 (Audio page mixer) only added the optional "mix" (audio tracks)
    // and "mixer" (timelines) objects; absent = a neutral mixer (main bus only, every track
    // routed to it, faders at 0 dB), which plays exactly like the older mix: nothing to
    // convert (serialization::trackMixFromJson() / mixerFromJson() give the defaults).
    // Versions <= 12 -> 13 only added the optional marker "duration" (absent = 1 frame, what
    // every older marker is): nothing to convert.
    // Versions <= 13 -> 14 only added the optional timeline "guides" (absent = no guides,
    // display only): nothing to convert.

    // Media paths stored relative to the document resolve against the document's folder
    // (for a recovered autosave that is the original project file, not the autosave dir).
    const QDir projectDir = QFileInfo(documentPath.isEmpty() ? path : documentPath).absoluteDir();
    QString loadError;
    if (!fromJson(root, version, projectDir, QFileInfo(path).completeBaseName(), &loadError))
        return fail(tr("%1: %2").arg(path, loadError));
    setFilePath(QFileInfo(path).absoluteFilePath());
    m_settingsDirty = false;
    emitDirtyIfChanged();
    emit nameChanged(m_name);
    emit cacheSettingsChanged();
    emit projectReset();
    return true;
}

// ---- Editing protocol ----

void Project::beginEdit(const QString& label)
{
    if (m_editDepth++ > 0)
        return;
    // Edit > Undo shows the label ("Undo Ripple Delete"): every step needs one.
    if (label.isEmpty())
        qCWarning(lcCore) << "undo step without a name";
    m_editLabel = label;
    m_editCancelled = false;
    m_editKeepsPlaying = false;
    m_editBefore = currentSet();
    m_editPoolBefore = m_mediaPool->state();
    m_editFormatBefore = m_format;
    m_editPrimary = m_activeUid;
}

void Project::endEdit()
{
    if (m_editDepth == 0) {
        qCWarning(lcCore) << "endEdit() without beginEdit()";
        return;
    }
    if (--m_editDepth > 0)
        return;

    TimelineSet before = std::move(m_editBefore);
    MediaPoolState poolBefore = std::move(m_editPoolBefore);
    m_editBefore = {};
    m_editPoolBefore = {};
    if (m_editCancelled) {
        m_format = m_editFormatBefore;
        if (!(m_mediaPool->state() == poolBefore))
            m_mediaPool->setState(std::move(poolBefore));
        applySet(before);
        return;
    }
    syncDerived();
    m_timeline->notifyChanged();
    TimelineSet after = currentSet();
    const bool timelinesChanged = !sameSet(before, after);
    const bool poolChanged = !(poolBefore == m_mediaPool->state());
    const bool formatChanged = !(m_editFormatBefore == m_format);
    if (!timelinesChanged && !poolChanged && !formatChanged)
        return;
    if (timelinesChanged)
        notifyNestedChanges(before);
    // The step below (the last done one) shares its after state with this step's before.
    const int below = m_undoStack->index() - 1;
    const auto* belowCommand
        = below >= 0 ? dynamic_cast<const SnapshotCommand*>(m_undoStack->command(below)) : nullptr;
    m_undoStack->push(new SnapshotCommand(this, m_editLabel, m_editPrimary, before, after,
        belowCommand, std::move(poolBefore), m_mediaPool->state(), m_editFormatBefore, m_format));
    emit editCommitted(m_editKeepsPlaying);
}

QString Project::closeStaleEdit()
{
    const QString label = openEditLabel();
    while (m_editDepth > 0)
        endEdit();
    return label;
}

void Project::cancelEdit()
{
    if (m_editDepth == 0) {
        qCWarning(lcCore) << "cancelEdit() without beginEdit()";
        return;
    }
    m_editCancelled = true;
    if (--m_editDepth > 0)
        return;
    m_format = m_editFormatBefore;
    if (!(m_mediaPool->state() == m_editPoolBefore))
        m_mediaPool->setState(m_editPoolBefore);
    applySet(m_editBefore);
    m_editBefore = {};
    m_editPoolBefore = {};
}

void Project::undo()
{
    if (isEditing()) {
        qCWarning(lcCore) << "undo ignored while an edit is open";
        return;
    }
    m_undoStack->undo();
}

void Project::redo()
{
    if (isEditing()) {
        qCWarning(lcCore) << "redo ignored while an edit is open";
        return;
    }
    m_undoStack->redo();
}

ImportResult Project::importMedia(const QStringList& paths, const QString& binId)
{
    beginEdit(tr("Import Media"));
    ImportResult result = m_mediaPool->importFiles(paths, binId);
    endEdit();
    return result;
}

void Project::restoreState(const TimelineData& timeline, const MediaPoolState& pool)
{
    if (!(m_timeline->data() == timeline)) {
        TimelineData copy = timeline;
        copy.uid = m_activeUid;
        m_timeline->setData(std::move(copy));
    }
    if (!(m_mediaPool->state() == pool))
        m_mediaPool->setState(pool);
}

void Project::restoreProjectState(
    const TimelineSet& timelines, const MediaPoolState& pool, const Format& format, quint64 primary)
{
    // Show the user what changes: undoing a step made in another timeline switches to it.
    if (primary != m_activeUid && slot(primary)) {
        const bool inTarget = std::any_of(timelines.begin(), timelines.end(),
            [primary](const auto& t) { return t->uid == primary; });
        if (inTarget)
            setActiveTimeline(primary);
    }
    m_format = format;
    if (!(m_mediaPool->state() == pool))
        m_mediaPool->setState(pool);
    applySet(timelines);
}

} // namespace kk
