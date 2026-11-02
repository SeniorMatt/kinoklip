// SPDX-License-Identifier: GPL-3.0-or-later
// Project implementation: snapshot undo commands, settings, JSON save/load.
// Timelines, tabs and compound clips: ProjectTimelines.cpp.
#include "core/Project.h"

#include "core/Logging.h"
#include "core/Serialization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>

namespace kk {

namespace {

constexpr Rational kDefaultFps {25, 1};
constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

/// One undo step: before/after copies of every timeline (shared, immutable), the media pool
/// and the project format. The first redo() is skipped because the change has already been
/// applied live.
class SnapshotCommand : public QUndoCommand
{
public:
    SnapshotCommand(Project* project, const QString& label, quint64 primary,
        Project::TimelineSet before, Project::TimelineSet after, MediaPoolState poolBefore,
        MediaPoolState poolAfter, Project::Format formatBefore, Project::Format formatAfter)
        : QUndoCommand(label)
        , m_project(project)
        , m_primary(primary)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_poolBefore(std::move(poolBefore))
        , m_poolAfter(std::move(poolAfter))
        , m_formatBefore(formatBefore)
        , m_formatAfter(formatAfter)
    { }

    void undo() override
    {
        m_project->restoreProjectState(m_before, m_poolBefore, m_formatBefore, m_primary);
    }

    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_project->restoreProjectState(m_after, m_poolAfter, m_formatAfter, m_primary);
    }

private:
    Project* m_project;
    quint64 m_primary;
    Project::TimelineSet m_before;
    Project::TimelineSet m_after;
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
    , m_timeline(new Timeline(this))
    , m_undoStack(new QUndoStack(this))
{
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this] { emitDirtyIfChanged(); });
    connect(m_links, &MediaLinks::changed, this, [this] { setSettingsDirty(true); });
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
    return ProjectSettings {m_name, m_format.width, m_format.height, m_format.fps};
}

void Project::setSettings(const ProjectSettings& settings)
{
    setName(settings.name);
    const Format format {
        settings.width, settings.height, settings.fps.isValid() ? settings.fps : kDefaultFps};
    if (format == m_format)
        return;
    // TODO(core): changing fps does not rescale existing clip positions yet.
    beginEdit(tr("Project Settings"));
    m_format = format;
    for (const quint64 uid : timelineUids()) {
        const auto data = timelineData(uid);
        if (!data->useProjectSettings)
            continue;
        if (data->width == format.width && data->height == format.height && data->fps == format.fps)
            continue;
        auto copy = std::make_shared<TimelineData>(*data);
        copy->width = format.width;
        copy->height = format.height;
        copy->fps = format.fps;
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
    m_format = Format {kDefaultWidth, kDefaultHeight, kDefaultFps};
    m_mediaPool->clear();
    m_links->clear();
    m_cacheSettings = {};
    TimelineData first
        = makeDefaultTimeline(tr("Timeline 1"), kDefaultFps, kDefaultWidth, kDefaultHeight);
    first.uid = 1;
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

QJsonObject Project::toJson(const QString& path) const
{
    const QDir projectDir = QFileInfo(path).absoluteDir();
    QJsonObject root;
    root[QLatin1String("formatVersion")] = serialization::kFormatVersion;
    root[QLatin1String("generator")] = QStringLiteral(KK_APP_NAME " " KK_APP_VERSION);
    root[QLatin1String("project")] = QJsonObject {{QLatin1String("name"), m_name},
        {QLatin1String("width"), m_format.width}, {QLatin1String("height"), m_format.height},
        {QLatin1String("fps"), fpsToJson(m_format.fps)}};
    root[QLatin1String("mediaPool")]
        = serialization::mediaPoolToJson(m_mediaPool->state(), projectDir);
    QJsonArray timelines;
    QJsonArray views;
    for (const TimelineSlot& s : m_slots) {
        timelines.append(serialization::timelineToJson(*timelineData(s.uid)));
        const bool active = s.uid == m_activeUid;
        const TimelineViewState v = s.view;
        views.append(QJsonObject {{QLatin1String("uid"), qint64(s.uid)},
            {QLatin1String("playhead"), qint64(active ? m_timeline->playhead() : s.playhead)},
            {QLatin1String("pixelsPerFrame"), v.pixelsPerFrame},
            {QLatin1String("scrollX"), v.scrollX}, {QLatin1String("scrollY"), v.scrollY}});
    }
    root[QLatin1String("timelines")] = timelines;
    root[QLatin1String("activeTimeline")] = qint64(m_activeUid);
    QJsonArray tabs;
    for (const quint64 uid : m_tabs)
        tabs.append(qint64(uid));
    root[QLatin1String("tabs")] = tabs;
    root[QLatin1String("timelineViews")] = views;
    root[QLatin1String("mediaLinks")] = m_links->toJson(m_mediaPool->state());
    root[QLatin1String("cacheSettings")] = m_cacheSettings.toJson();
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
    m_mediaPool->setState(serialization::mediaPoolFromJson(
        root.value(QLatin1String("mediaPool")).toObject(), projectDir));
    // Version 3 -> 4: no links and default cache settings (both keys are simply absent); the
    // application links proxies found in the proxy folder after loading.
    m_links->fromJson(root.value(QLatin1String("mediaLinks")).toArray());
    m_cacheSettings
        = MediaCacheSettings::fromJson(root.value(QLatin1String("cacheSettings")).toObject());
    resetTimelines(std::move(timelineSlots), active, tabs);
    return true;
}

bool Project::save(const QString& path, QString* error)
{
    if (!saveCopy(path, error))
        return false;
    setFilePath(QFileInfo(path).absoluteFilePath());
    m_undoStack->setClean();
    m_settingsDirty = false;
    emitDirtyIfChanged();
    return true;
}

bool Project::saveCopy(const QString& path, QString* error, const QJsonObject& extra) const
{
    QJsonObject root = toJson(path);
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
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
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
    m_editLabel = label;
    m_editCancelled = false;
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
    m_undoStack->push(
        new SnapshotCommand(this, m_editLabel, m_editPrimary, std::move(before), std::move(after),
            std::move(poolBefore), m_mediaPool->state(), m_editFormatBefore, m_format));
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
