// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers for the project library tests: an isolated XDG environment and generated projects.
#pragma once

#include "core/EditOps.h"
#include "core/Project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace kk::test {

/// Points XDG_DATA_HOME (the trash), XDG_CACHE_HOME and XDG_CONFIG_HOME (QSettings) into
/// @p dir. Must run before the QApplication exists. @p dir should be on the home partition
/// (next to the test binary), so QFile::moveToTrash() uses $XDG_DATA_HOME/Trash.
inline void isolateXdg(const QString& dir)
{
    QDir().mkpath(dir + QStringLiteral("/data"));
    QDir().mkpath(dir + QStringLiteral("/cache"));
    QDir().mkpath(dir + QStringLiteral("/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dir + QStringLiteral("/data")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(dir + QStringLiteral("/cache")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(dir + QStringLiteral("/config")));
}

/// Options of a generated project.
struct ProjectSpec
{
    int width = 1920;
    int height = 1080;
    Rational fps {25, 1};
    int64_t clipFrames = 50; ///< length of the one solid clip on V1 (0 = empty timeline)
    QString mediaFile; ///< a media file put on V2 at frame 0 (the poster), optional
};

/// Writes a project named after the file to @p path with Project::save().
inline bool writeProject(const QString& path, const ProjectSpec& spec = {})
{
    Project project;
    ProjectSettings s = project.settings();
    s.name = QFileInfo(path).completeBaseName();
    s.width = spec.width;
    s.height = spec.height;
    s.fps = spec.fps;
    project.setSettings(s);
    project.setName(s.name);
    project.beginEdit(QStringLiteral("generate"));
    if (spec.clipFrames > 0) {
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.solidColor = Qt::darkCyan;
        clip.start = 0;
        clip.duration = spec.clipFrames;
        editops::overwriteClip(project.timeline()->mutableData(), TrackRef {}, clip);
    }
    project.endEdit();
    if (!spec.mediaFile.isEmpty()) {
        project.importMedia({spec.mediaFile});
        const MediaPoolState& pool = project.mediaPool()->state();
        if (!pool.items.isEmpty()) {
            project.beginEdit(QStringLiteral("media"));
            ClipData clip;
            clip.kind = ClipKind::Media;
            clip.mediaId = pool.items.last().id;
            clip.start = 0;
            clip.duration = 25;
            clip.sourceInUs = 500'000;
            clip.sourceDurationUs = pool.items.last().info.durationUs;
            editops::overwriteClip(
                project.timeline()->mutableData(), TrackRef {TrackType::Video, 1}, clip);
            project.endEdit();
        }
    }
    return project.save(path);
}

/// Rewrites the path of every media file item of the project at @p path to @p mediaPath (to
/// simulate media that went offline).
inline bool redirectMedia(const QString& path, const QString& mediaPath)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QJsonObject pool = root.value(QLatin1String("mediaPool")).toObject();
    QJsonArray items = pool.value(QLatin1String("items")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        QJsonObject item = items[i].toObject();
        if (item.contains(QLatin1String("path"))) {
            item[QLatin1String("path")] = mediaPath;
            item[QLatin1String("relativePath")] = QFileInfo(mediaPath).fileName();
            items[i] = item;
        }
    }
    pool[QLatin1String("items")] = items;
    root[QLatin1String("mediaPool")] = pool;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson());
    return true;
}

} // namespace kk::test
