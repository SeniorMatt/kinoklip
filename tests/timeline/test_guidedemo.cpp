// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the projects the user guide's screenshots are taken from (scripts/guide_screenshots.py):
//   KK_GUIDE_MEDIA=<dir with interview.mp4, broll.mp4, texture.mp4, music.wav, greenscreen.mp4,
//                   background.mp4>   (the script generates them with ffmpeg)
//   KK_GUIDE_DIR=<out dir>            writes edit.kkproj, greenscreen-raw.kkproj,
//                                     greenscreen.kkproj, greenscreen-matte.kkproj, music.kkproj
// Without KK_GUIDE_MEDIA the edit project uses stand-in solids and the media projects are
// skipped; every project is checked (invariants, save and load round trip) either way.
// Also checks the screenshot geometry sidecar (src/app/ScreenshotGeometry) the script draws from.
#include "TimelineTestUtils.h"
#include "app/ScreenshotGeometry.h"
#include "core/VideoEffectKeys.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::test;

namespace {

/// Media clip of @p item on video track @p vTrack (plus linked audio on @p aTrack when the
/// file has sound and @p aTrack >= 0). Returns the video clip id (or the audio one).
quint64 place(
    TimelineData& d, const MediaItem& item, int64_t start, int64_t duration, int vTrack, int aTrack)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = item.id;
    c.name = item.name;
    c.start = start;
    c.duration = duration;
    c.sourceDurationUs = item.info.durationUs;
    QVector<editops::PlacedClip> placed;
    if (vTrack >= 0 && item.info.hasVideo)
        placed.append({TrackRef {TrackType::Video, vTrack}, c});
    if (aTrack >= 0 && item.info.hasAudio)
        placed.append({TrackRef {TrackType::Audio, aTrack}, c});
    const QVector<quint64> ids = editops::overwriteClips(d, placed);
    if (ids.size() > 1)
        editops::linkClips(d, ids);
    return ids.isEmpty() ? 0 : ids.front();
}

const MediaItem* itemNamed(const Project& project, const QString& baseName)
{
    for (const MediaItem& item : project.mediaPool()->items()) {
        if (QFileInfo(item.path).completeBaseName() == baseName)
            return &item;
    }
    return nullptr;
}

} // namespace

class TestGuideDemo : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_guidedemo"); }

    void editProject()
    {
        Project project;
        QVERIFY(buildDemoTimeline(project, mediaDir()) > 10);
        project.setName(QStringLiteral("My First Video"));
        saveAndCheck(project, QStringLiteral("edit.kkproj"));
    }

    void greenScreenProjects_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("view"); // empty = no key on the clip
        QTest::newRow("raw") << QStringLiteral("greenscreen-raw.kkproj") << QString();
        QTest::newRow("keyed") << QStringLiteral("greenscreen.kkproj") << fxchoice::Final;
        QTest::newRow("matte") << QStringLiteral("greenscreen-matte.kkproj") << fxchoice::Matte;
    }

    void greenScreenProjects()
    {
        QFETCH(QString, file);
        QFETCH(QString, view);
        Project project;
        if (!importAll(project, {"background.mp4", "greenscreen.mp4"}))
            QSKIP("KK_GUIDE_MEDIA has no background.mp4 / greenscreen.mp4");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        d.name = QStringLiteral("Green Screen");
        while (d.videoTracks.size() < 2)
            editops::addTrack(d, TrackType::Video);
        const int64_t length = 10 * 25;
        place(d, *itemNamed(project, QStringLiteral("background")), 0, length, 0, 0);
        const quint64 fg
            = place(d, *itemNamed(project, QStringLiteral("greenscreen")), 0, length, 1, -1);
        if (!view.isEmpty()) {
            ClipData* c = editops::clipById(d, fg);
            QVERIFY(c);
            EffectInstance key = EffectRegistry::instance().createEffect(
                QLatin1String(fxid::ChromaKey), d.allocateId());
            key.params.setStatic(fxkeys::View, view);
            c->effects.append(key);
        }
        project.endEdit();
        project.timeline()->setPlayhead(length / 2);
        project.setName(QStringLiteral("Green Screen Test"));
        saveAndCheck(project, file);
    }

    void musicProject()
    {
        Project project;
        if (!importAll(project, {"interview.mp4", "music.wav"}))
            QSKIP("KK_GUIDE_MEDIA has no interview.mp4 / music.wav");
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& d = project.timeline()->mutableData();
        d.name = QStringLiteral("With Music");
        d.audioTracks[1].name = QStringLiteral("Music");
        const int64_t fps = 25;
        place(d, *itemNamed(project, QStringLiteral("interview")), 0, 12 * fps, 0, 0);
        const quint64 music
            = place(d, *itemNamed(project, QStringLiteral("music")), 0, 12 * fps, -1, 1);
        ClipData* m = editops::clipById(d, music);
        QVERIFY(m);
        m->fadeInFrames = 2 * fps;
        m->fadeOutFrames = 3 * fps;
        m->audio.setBase(keys::VolumeDb, -8);
        d.audioTracks[1].height = 90; // room for the level line and fade handles
        d.audioTracks[0].height = 70;
        project.endEdit();
        project.timeline()->setPlayhead(6 * fps);
        project.setName(QStringLiteral("Music Video"));
        saveAndCheck(project, QStringLiteral("music.kkproj"));
    }

    /// The sidecar lists named widgets, tool bar buttons by action id, clips, transitions and
    /// the playhead, in the panel's own coordinates.
    void geometrySidecar()
    {
        TimelineFixture f;
        buildDemoTimeline(f.project); // stand-in solids: add the transition the media gets
        f.project.beginEdit(QStringLiteral("Transition"));
        f.data().videoTracks[0].clips[1].transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 25);
        f.project.endEdit();
        TimelinePanel panel(f.ctx);
        panel.resize(1600, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.timelineWidget()->zoomToFit();
        QTest::qWait(20);
        const QString path = m_tmp.filePath(QStringLiteral("geometry.json"));
        QVERIFY(
            writeScreenshotGeometry(&panel, path, {}, &f.view(), f.project.timeline()->playhead()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(doc.value(QStringLiteral("width")).toInt(), 1600);
        auto find = [&](const QString& key, const QString& value) {
            for (const QJsonValue v : doc.value(QStringLiteral("entries")).toArray()) {
                if (v.toObject().value(key).toVariant().toString() == value)
                    return v.toObject();
            }
            return QJsonObject();
        };
        auto rectOf = [](const QJsonObject& o) {
            const QJsonArray r = o.value(QStringLiteral("rect")).toArray();
            return QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        };
        const QRect inside(0, 0, 1600, 520);
        const QJsonObject blade = find(QStringLiteral("action"), QStringLiteral("tool.blade"));
        QVERIFY(!blade.isEmpty());
        QVERIFY(inside.contains(rectOf(blade)));
        const QJsonObject slider
            = find(QStringLiteral("name"), QStringLiteral("TimelineZoomSlider"));
        QVERIFY(!slider.isEmpty());
        // V1's first clip starts at frame 0, right of the track headers; the transition at the
        // head of V1's second clip is centred on the cut between the two.
        QJsonObject first, second, transition, playhead;
        for (const QJsonValue v : doc.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString kind = o.value(QStringLiteral("kind")).toString();
            const bool v1 = o.value(QStringLiteral("track")).toString() == QLatin1String("V1");
            const int index = o.value(QStringLiteral("index")).toInt();
            if (kind == QLatin1String("clip") && v1 && index == 0)
                first = o;
            if (kind == QLatin1String("clip") && v1 && index == 1)
                second = o;
            if (kind == QLatin1String("transition") && v1 && index == 1)
                transition = o;
            if (kind == QLatin1String("playhead"))
                playhead = o;
        }
        QVERIFY(
            !first.isEmpty() && !second.isEmpty() && !transition.isEmpty() && !playhead.isEmpty());
        QCOMPARE(
            first.value(QStringLiteral("text")).toString(), f.view().videoTracks[0].clips[0].name);
        QVERIFY(rectOf(first).left() >= timelinemetrics::kHeaderWidth - 1);
        const int cut = rectOf(second).left();
        QVERIFY(rectOf(transition).left() < cut && rectOf(transition).right() > cut);
        QVERIFY(rectOf(playhead).width() == 1 && inside.contains(rectOf(playhead)));
    }

private:
    QTemporaryDir m_tmp;

    static QString mediaDir() { return qEnvironmentVariable("KK_GUIDE_MEDIA"); }

    /// Imports @p names from KK_GUIDE_MEDIA; false if one is missing.
    static bool importAll(Project& project, std::initializer_list<const char*> names)
    {
        const QString dir = mediaDir();
        if (dir.isEmpty())
            return false;
        QStringList paths;
        for (const char* name : names) {
            const QString path = QDir(dir).filePath(QLatin1String(name));
            if (!QFileInfo::exists(path))
                return false;
            paths << path;
        }
        return project.importMedia(paths).importedIds.size() == paths.size();
    }

    /// Checks @p project, saves it into KK_GUIDE_DIR (else a temporary folder) and loads it back.
    void saveAndCheck(Project& project, const QString& file)
    {
        QString why;
        QVERIFY2(editops::checkInvariants(project.timeline()->data(), &why), qPrintable(why));
        QString dir = qEnvironmentVariable("KK_GUIDE_DIR");
        if (dir.isEmpty())
            dir = m_tmp.path();
        QVERIFY(QDir().mkpath(dir));
        const QString path = QDir(dir).filePath(file);
        QVERIFY2(project.save(path, &why), qPrintable(why));
        Project loaded;
        QVERIFY2(loaded.load(path, &why), qPrintable(why));
        QCOMPARE(loaded.timeline()->data().videoTracks.size(),
            project.timeline()->data().videoTracks.size());
        QCOMPARE(loaded.timeline()->data().duration(), project.timeline()->data().duration());
    }
};

QTEST_MAIN(TestGuideDemo)
#include "test_guidedemo.moc"
