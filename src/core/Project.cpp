// SPDX-License-Identifier: GPL-3.0-or-later
// Project implementation: snapshot undo commands, settings, JSON save/load.
#include "core/Project.h"

#include "core/Logging.h"
#include "core/Serialization.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUndoCommand>
#include <QUndoStack>

namespace kk {

namespace {

constexpr Rational kDefaultFps {25, 1};
constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

/// One undo step: complete before/after copies of the timeline and media pool.
/// The first redo() is skipped because the change has already been applied live.
class SnapshotCommand : public QUndoCommand
{
public:
    SnapshotCommand(Project* project, const QString& label, TimelineData timelineBefore,
        TimelineData timelineAfter, MediaPoolState poolBefore, MediaPoolState poolAfter)
        : QUndoCommand(label)
        , m_project(project)
        , m_timelineBefore(std::move(timelineBefore))
        , m_timelineAfter(std::move(timelineAfter))
        , m_poolBefore(std::move(poolBefore))
        , m_poolAfter(std::move(poolAfter))
    { }

    void undo() override { m_project->restoreState(m_timelineBefore, m_poolBefore); }

    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_project->restoreState(m_timelineAfter, m_poolAfter);
    }

private:
    Project* m_project;
    TimelineData m_timelineBefore;
    TimelineData m_timelineAfter;
    MediaPoolState m_poolBefore;
    MediaPoolState m_poolAfter;
    bool m_firstRedo = true;
};

QJsonObject projectToJson(
    const QString& name, const Timeline& timeline, const MediaPool& pool, const QString& path)
{
    const QDir projectDir = QFileInfo(path).absoluteDir();
    QJsonObject root;
    root[QLatin1String("formatVersion")] = serialization::kFormatVersion;
    root[QLatin1String("generator")] = QStringLiteral(KK_APP_NAME " " KK_APP_VERSION);
    root[QLatin1String("project")] = QJsonObject {{QLatin1String("name"), name}};
    root[QLatin1String("mediaPool")] = serialization::mediaPoolToJson(pool.state(), projectDir);
    root[QLatin1String("timeline")] = serialization::timelineToJson(timeline.data());
    return root;
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
    , m_timeline(new Timeline(this))
    , m_undoStack(new QUndoStack(this))
{
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this] { emitDirtyIfChanged(); });
    newProject();
}

Project::~Project() = default;

// ---- Settings & document state ----

ProjectSettings Project::settings() const
{
    const TimelineData& d = m_timeline->data();
    return ProjectSettings {m_name, d.width, d.height, d.fps};
}

void Project::setSettings(const ProjectSettings& settings)
{
    setName(settings.name);
    const TimelineData& d = m_timeline->data();
    if (d.width == settings.width && d.height == settings.height && d.fps == settings.fps)
        return;
    // TODO(core): changing fps does not rescale existing clip positions yet.
    beginEdit(tr("Project Settings"));
    TimelineData& m = m_timeline->mutableData();
    m.width = settings.width;
    m.height = settings.height;
    m.fps = settings.fps.isValid() ? settings.fps : kDefaultFps;
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
    m_timeline->setData(
        makeDefaultTimeline(tr("Timeline 1"), kDefaultFps, kDefaultWidth, kDefaultHeight));
    m_timeline->setPlayhead(0);
    m_mediaPool->clear();
    setFilePath({});
    m_settingsDirty = false;
    emitDirtyIfChanged();
    emit nameChanged(m_name);
    emit projectReset();
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

bool Project::saveCopy(const QString& path, QString* error) const
{
    return writeJson(path, projectToJson(m_name, *m_timeline, *m_mediaPool, path), error);
}

bool Project::load(const QString& path, QString* error)
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
    // serialization::paramSetFromJson(), which recognises the v1 "interp" field; nothing else
    // changed, so no whole-document migration is needed.

    TimelineData timeline;
    QString timelineError;
    if (!serialization::timelineFromJson(
            root.value(QLatin1String("timeline")).toObject(), timeline, &timelineError)) {
        return fail(tr("%1: %2").arg(path, timelineError));
    }
    const QDir projectDir = QFileInfo(path).absoluteDir();
    MediaPoolState pool = serialization::mediaPoolFromJson(
        root.value(QLatin1String("mediaPool")).toObject(), projectDir);

    m_editDepth = 0;
    m_undoStack->clear();
    m_name = root.value(QLatin1String("project"))
                 .toObject()
                 .value(QLatin1String("name"))
                 .toString(QFileInfo(path).completeBaseName());
    m_timeline->setData(std::move(timeline));
    m_timeline->setPlayhead(0);
    m_mediaPool->setState(std::move(pool));
    setFilePath(QFileInfo(path).absoluteFilePath());
    m_settingsDirty = false;
    emitDirtyIfChanged();
    emit nameChanged(m_name);
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
    m_editTimelineBefore = m_timeline->data();
    m_editPoolBefore = m_mediaPool->state();
}

void Project::endEdit()
{
    if (m_editDepth == 0) {
        qCWarning(lcCore) << "endEdit() without beginEdit()";
        return;
    }
    if (--m_editDepth > 0)
        return;

    TimelineData before = std::move(m_editTimelineBefore);
    MediaPoolState poolBefore = std::move(m_editPoolBefore);
    m_editTimelineBefore = {};
    m_editPoolBefore = {};
    if (m_editCancelled) {
        restoreState(before, poolBefore);
        return;
    }
    m_timeline->notifyChanged();
    const bool timelineChanged = !(before == m_timeline->data());
    const bool poolChanged = !(poolBefore == m_mediaPool->state());
    if (!timelineChanged && !poolChanged)
        return;
    m_undoStack->push(new SnapshotCommand(this, m_editLabel, std::move(before), m_timeline->data(),
        std::move(poolBefore), m_mediaPool->state()));
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
    restoreState(m_editTimelineBefore, m_editPoolBefore);
    m_editTimelineBefore = {};
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
    if (!(m_timeline->data() == timeline))
        m_timeline->setData(timeline);
    if (!(m_mediaPool->state() == pool))
        m_mediaPool->setState(pool);
}

} // namespace kk
