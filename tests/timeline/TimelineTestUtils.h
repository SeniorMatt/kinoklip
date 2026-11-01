// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers for the timeline tests: an AppContext fixture and a dense demo timeline used
// by the UI tests, the benchmark and the screenshot check.
#pragma once

#include "app/Theme.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>

namespace kk::test {

/// One-time application setup for widget tests (test settings, resources, dark theme).
inline void initTimelineTestCase(const char* name)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
    QCoreApplication::setApplicationName(QLatin1String(name));
    QSettings().clear();
    initResources();
    theme::apply(*qApp);
}

/// Everything main() creates that the timeline needs.
struct TimelineFixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;

    TimelineFixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
    }

    TimelineData& data() { return project.timeline()->mutableData(); }
    const TimelineData& view() const { return project.timeline()->data(); }
};

/// A solid clip [start, start + duration).
inline ClipData solidClip(
    int64_t start, int64_t duration, const QString& name = QStringLiteral("Solid"))
{
    ClipData c;
    c.kind = ClipKind::Solid;
    c.name = name;
    c.start = start;
    c.duration = duration;
    c.solidColor = QColor(0x40, 0x60, 0x80);
    return c;
}

/// A title clip [start, start + duration).
inline ClipData titleClip(int64_t start, int64_t duration, const QString& text)
{
    ClipData c;
    c.kind = ClipKind::Title;
    c.name = text;
    c.start = start;
    c.duration = duration;
    c.title = EffectRegistry::instance().defaultParams(QStringLiteral("text_title"));
    c.title.setStatic(keys::Text, text);
    return c;
}

/// Fills the project with a dense, realistic edit: cut interview on V1/A1 (linked), B-roll and
/// titles on V2/V3, music on A2 with volume keyframes, transitions, an effect, a disabled clip,
/// markers and in/out marks. Media files from @p mediaDir (interview.mp4, broll.mp4,
/// texture.mp4, music.wav) are imported when present; otherwise solids stand in for them.
/// One undo step. Returns the number of clips created.
inline int buildDemoTimeline(Project& project, const QString& mediaDir = {})
{
    namespace ops = editops;
    QStringList files;
    for (const char* name : {"interview.mp4", "broll.mp4", "texture.mp4", "music.wav"}) {
        const QString path = QDir(mediaDir).filePath(QLatin1String(name));
        if (!mediaDir.isEmpty() && QFileInfo::exists(path))
            files << path;
    }
    QHash<QString, const MediaItem*> media;
    if (!files.isEmpty()) {
        project.importMedia(files);
        for (const MediaItem& item : project.mediaPool()->items())
            media.insert(QFileInfo(item.path).completeBaseName(), &item);
    }

    project.beginEdit(QStringLiteral("Demo"));
    TimelineData& d = project.timeline()->mutableData();
    d.name = QStringLiteral("Demo Timeline");
    while (d.videoTracks.size() < 3)
        ops::addTrack(d, TrackType::Video);
    d.videoTracks[2].name = QStringLiteral("Titles");
    d.audioTracks[1].name = QStringLiteral("Music");

    const int64_t fps = 25;
    int clips = 0;
    // Media clip (video + linked audio) or a stand-in solid when the file is missing.
    auto av = [&](const char* name, int64_t start, int64_t duration, int64_t srcSec, int vTrack) {
        const MediaItem* item = media.value(QLatin1String(name));
        if (!item) {
            ClipData s = solidClip(start, duration, QLatin1String(name));
            ops::overwriteClip(d, TrackRef {TrackType::Video, vTrack}, s);
            ++clips;
            return quint64(0);
        }
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = item->id;
        c.name = item->name;
        c.start = start;
        c.duration = duration;
        c.sourceInUs = srcSec * 1000000;
        c.sourceDurationUs = item->info.durationUs;
        QVector<ops::PlacedClip> placed {{TrackRef {TrackType::Video, vTrack}, c}};
        if (item->info.hasAudio && vTrack == 0)
            placed.append({TrackRef {TrackType::Audio, 0}, c});
        const QVector<quint64> ids = ops::overwriteClips(d, placed);
        if (ids.size() > 1)
            ops::linkClips(d, ids);
        clips += int(ids.size());
        return ids.isEmpty() ? quint64(0) : ids.front();
    };

    // V1 / A1: the cut.
    av("interview", 0, 6 * fps, 0, 0);
    const quint64 tex = av("texture", 6 * fps, 5 * fps, 2, 0);
    av("interview", 11 * fps, 7 * fps, 9, 0);
    av("broll", 18 * fps, 6 * fps, 3, 0);
    av("interview", 24 * fps, 4 * fps, 20, 0);
    av("texture", 28 * fps, 3 * fps, 12, 0);
    if (ClipData* c = ops::clipById(d, tex))
        c->transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), fps);
    // V2: B-roll overlays with an effect and opacity keyframes, a disabled clip.
    const quint64 b1 = av("broll", 3 * fps, 4 * fps, 10, 1);
    if (ClipData* c = ops::clipById(d, b1)) {
        c->effects.append(EffectRegistry::instance().createEffect(
            QStringLiteral("gaussian_blur"), d.allocateId()));
        AnimatedValue& op = c->transform.animated(keys::Opacity, 100);
        op.setKey(0, 0);
        op.setKey(12, 100);
        op.setKey(88, 100);
        op.setKey(99, 0);
    }
    const quint64 b2 = av("texture", 13 * fps, 4 * fps, 20, 1);
    if (ClipData* c = ops::clipById(d, b2))
        c->enabled = false;
    av("broll", 20 * fps, 3 * fps, 15, 1);
    ops::overwriteClip(d, TrackRef {TrackType::Video, 1},
        solidClip(25 * fps, 5 * fps, QStringLiteral("Color Matte")));
    // V3: titles.
    ops::overwriteClip(d, TrackRef {TrackType::Video, 2},
        titleClip(1 * fps, 4 * fps, QStringLiteral("Opening Title")));
    ops::overwriteClip(d, TrackRef {TrackType::Video, 2},
        titleClip(12 * fps, 5 * fps, QStringLiteral("Lower Third - Interview")));
    ClipData endTitle = titleClip(26 * fps, 5 * fps, QStringLiteral("End Credits"));
    endTitle.transitionOut
        = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), fps);
    ops::overwriteClip(d, TrackRef {TrackType::Video, 2}, endTitle);
    clips += 4;
    // A2: music with a fade in/out on the volume.
    if (const MediaItem* m = media.value(QStringLiteral("music"))) {
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = m->id;
        c.name = m->name;
        c.start = 0;
        c.duration = 31 * fps;
        c.sourceDurationUs = m->info.durationUs;
        AnimatedValue& vol = c.audio.animated(keys::VolumeDb, 0);
        vol.setKey(0, -40);
        vol.setKey(2 * fps, -6);
        vol.setKey(29 * fps, -6);
        vol.setKey(31 * fps - 1, -40);
        ops::overwriteClip(d, TrackRef {TrackType::Audio, 1}, c);
        ++clips;
    }
    d.markers.append(Marker {4 * fps, QStringLiteral("Beat"), QColor(0x4c, 0x9b, 0xe8)});
    d.markers.append(Marker {11 * fps, QStringLiteral("Cut"), QColor(0xe8, 0x9b, 0x3c)});
    d.markers.append(Marker {24 * fps, QStringLiteral("Outro"), QColor(0x6c, 0xc0, 0x5c)});
    d.markIn = 9 * fps;
    d.markOut = 21 * fps;
    project.endEdit();
    project.timeline()->setPlayhead(int64_t(14.5 * fps));
    return clips;
}

/// @p count solid clips spread over the four default tracks (benchmark data), 1-30 frames
/// long with small gaps, some with transitions and effects.
inline void buildManyClips(TimelineData& d, int count)
{
    namespace ops = editops;
    int64_t at[4] = {0, 0, 0, 0};
    for (int i = 0; i < count; ++i) {
        const int t = i % 4;
        const int64_t len = 5 + (i * 7919) % 26;
        ClipData c = solidClip(at[t], len, QStringLiteral("Clip %1").arg(i));
        if (i % 9 == 0)
            c.effects.append(EffectInstance {d.allocateId(), QStringLiteral("gaussian_blur")});
        if (i % 13 == 0)
            c.transitionIn = TransitionData {};
        const TrackRef ref
            = t < 2 ? TrackRef {TrackType::Video, t} : TrackRef {TrackType::Audio, t - 2};
        ops::overwriteClip(d, ref, c);
        at[t] += len + (i % 5 == 0 ? 3 : 0);
    }
}

} // namespace kk::test

/// Fails the current test if the timeline violates its invariants.
#define KK_VERIFY_TIMELINE(data)                                                                   \
    do {                                                                                           \
        QString why_;                                                                              \
        QVERIFY2(kk::editops::checkInvariants((data), &why_), qPrintable(why_));                   \
    } while (false)
