// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the command layer (app/commands): every command on a small generated project, the
// result and the resulting project state read back from the saved file, and the error cases
// (unknown ids, out-of-range times, missing files, bad values).
#include "app/commands/Command.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "media/MediaInfo.h"
#include "ui/common/Resources.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::commands;

namespace {

const QString kVideo = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4"); // 10 s, V + A
const QString kAudio = QStringLiteral(KK_TEST_DATA_DIR "/generated/sine.mp3"); // 3 s

CommandResult call(const QString& name, QJsonObject args)
{
    return CommandRegistry::builtin().execute(name, args);
}

} // namespace

class TestCommands : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_project; ///< 24 fps project with kVideo and kAudio imported

    QString path(const QString& name) const { return m_dir.filePath(name); }

    /// Calls a command that must succeed on the shared project.
    CommandResult ok(const QString& name, QJsonObject args)
    {
        args.insert(QStringLiteral("project"), m_project);
        const CommandResult r = call(name, args);
        [&] { QVERIFY2(r.ok, qPrintable(name + ": " + r.errorCode + " " + r.error)); }();
        return r;
    }
    CommandResult bad(const QString& name, QJsonObject args, const QString& code)
    {
        args.insert(QStringLiteral("project"), m_project);
        const CommandResult r = call(name, args);
        [&] {
            QVERIFY2(!r.ok,
                qPrintable(name + " should have failed: "
                    + QJsonDocument(args).toJson(QJsonDocument::Compact)));
            QCOMPARE(r.errorCode, code);
            QVERIFY(!r.error.isEmpty());
        }();
        return r;
    }
    /// A fresh 24 fps project with media imported, saved at path("p.kkproj").
    void makeProject(const QString& file = QStringLiteral("p.kkproj"))
    {
        m_project = path(file);
        QVERIFY(call(QStringLiteral("project.new"),
            {{"project", m_project}, {"force", true}, {"width", 640}, {"height", 360}, {"fps", 24}})
                .ok);
        QVERIFY(call(QStringLiteral("media.import"),
            {{"project", m_project}, {"file", QJsonArray {kVideo, kAudio}}})
                .ok);
    }
    Project* load()
    {
        m_loaded = std::make_unique<Project>();
        QString error;
        if (!m_loaded->load(m_project, &error))
            qWarning() << error;
        return m_loaded.get();
    }
    std::unique_ptr<Project> m_loaded;
    QJsonArray clips(const QString& track = {})
    {
        QJsonObject a {{"project", m_project}};
        if (!track.isEmpty())
            a.insert("track", track);
        return call(QStringLiteral("clip.list"), a).data["clips"].toArray();
    }
    qint64 firstClipId(const QString& track)
    {
        return clips(track).first().toObject()["id"].toInteger();
    }
    /// The files of the media pool (media.list also lists the timelines).
    QJsonArray mediaFiles()
    {
        QJsonArray files;
        for (const QJsonValue& v :
            call(QStringLiteral("media.list"), {{"project", m_project}}).data["media"].toArray()) {
            if (v.toObject()["kind"].toString() == QLatin1String("file"))
                files.append(v);
        }
        return files;
    }
    QString videoId()
    {
        for (const QJsonValue& v : mediaFiles()) {
            if (v.toObject()["path"].toString() == kVideo)
                return v.toObject()["id"].toString();
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        kk::initResources();
        QVERIFY(m_dir.isValid());
        QVERIFY(QFile::exists(kVideo));
    }

    void registryListsEveryCommandWithParameters()
    {
        const QJsonArray list = CommandRegistry::builtin().describeAll()["commands"].toArray();
        QStringList names;
        for (const QJsonValue& v : list) {
            const QJsonObject c = v.toObject();
            names << c["name"].toString();
            QVERIFY(!c["description"].toString().isEmpty());
            for (const QJsonValue& p : c["parameters"].toArray()) {
                QVERIFY(!p.toObject()["name"].toString().isEmpty());
                QVERIFY(!p.toObject()["type"].toString().isEmpty());
                QVERIFY(!p.toObject()["description"].toString().isEmpty());
            }
        }
        for (const char* expected : {"version", "project.new", "project.info", "media.import",
                 "media.list", "media.relink", "timeline.list", "timeline.new", "timeline.select",
                 "clip.list", "clip.add", "clip.move", "clip.trim", "clip.split", "clip.delete",
                 "clip.set", "marker.add", "marker.list", "marker.delete", "transition.add",
                 "export.otio", "export.edl", "import.otio", "render"})
            QVERIFY2(names.contains(QLatin1String(expected)), expected);
        QCOMPARE(names.size(), QSet<QString>(names.begin(), names.end()).size()); // unique
    }

    void argumentChecking()
    {
        QCOMPARE(call("nope", {}).errorCode, QString("unknown_command"));
        QCOMPARE(call("project.info", {}).errorCode, QString("missing_parameter"));
        QCOMPARE(call("project.info", {{"project", "x"}, {"colour", 1}}).errorCode,
            QString("unknown_parameter"));
        QCOMPARE(call("project.new", {{"project", path("a.kkproj")}, {"width", "wide"}}).errorCode,
            QString("bad_value"));
        // Strings of numbers are accepted, as the command line passes strings.
        QVERIFY(call(
            "project.new", {{"project", path("s.kkproj")}, {"width", "800"}, {"height", "600"}})
                .ok);
        QCOMPARE(call("version", {}).ok, true);
        QVERIFY(call("version", {}).data["version"].toString().size() > 0);
        QCOMPARE(call("project.info", {{"project", path("missing.kkproj")}}).errorCode,
            QString("file_not_found"));
        QFile bad(path("broken.kkproj"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not json");
        bad.close();
        QCOMPARE(call("project.info", {{"project", path("broken.kkproj")}}).errorCode,
            QString("project_error"));
    }

    void projectNewAndInfo()
    {
        const QString file = path("new.kkproj");
        const CommandResult r = call("project.new",
            {{"project", file}, {"name", "Demo"}, {"width", 1280}, {"height", 720},
                {"fps", 29.97}});
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(QFile::exists(file));
        Project p;
        QVERIFY(p.load(file));
        QCOMPARE(p.settings().name, QString("Demo"));
        QCOMPARE(p.settings().width, 1280);
        QCOMPARE(p.settings().fps.num, 30000);
        QCOMPARE(p.settings().fps.den, 1001);
        QCOMPARE(p.timeline()->data().videoTracks.size(), size_t(1));
        const CommandResult info = call("project.info", {{"project", file}});
        QVERIFY(info.ok);
        QCOMPARE(info.data["name"].toString(), QString("Demo"));
        QCOMPARE(info.data["timelines"].toArray().size(), 1);
        QVERIFY(info.text.contains("Demo"));
        // Refuses to overwrite; --force overwrites; sizes are checked.
        QCOMPARE(call("project.new", {{"project", file}}).errorCode, QString("file_exists"));
        QVERIFY(call("project.new", {{"project", file}, {"force", true}}).ok);
        QCOMPARE(call("project.new", {{"project", file}, {"force", true}, {"width", 0}}).errorCode,
            QString("bad_value"));
        QCOMPARE(call("project.new", {{"project", file}, {"force", true}, {"fps", 0}}).errorCode,
            QString("bad_value"));
    }

    void mediaImportListRelink()
    {
        makeProject();
        const QJsonArray media = mediaFiles();
        QCOMPARE(media.size(), 2);
        const QJsonObject first = media.first().toObject();
        QCOMPARE(first["hasAudio"].toBool(), true);
        QCOMPARE(first["hasVideo"].toBool(), true);
        QCOMPARE(first["offline"].toBool(), false);
        QVERIFY(first["durationUs"].toInteger() > 9'000'000);
        QCOMPARE(mediaFiles().size(), 2);

        // Errors: a missing file alone, a file already in the pool.
        bad("media.import", {{"file", path("nothing.mp4")}}, "import_failed");
        bad("media.import", {{"file", kVideo}}, "import_failed");
        // A missing file next to a good one is a warning.
        QTemporaryDir other;
        const QString copy = other.filePath("copy.mp4");
        QVERIFY(QFile::copy(kVideo, copy));
        const CommandResult mixed
            = ok("media.import", {{"file", QJsonArray {copy, path("nothing.mp4")}}});
        QCOMPARE(mixed.data["imported"].toArray().size(), 1);
        QCOMPARE(mixed.data["warnings"].toArray().size(), 1);

        // Offline after the file is deleted; relinked from a folder, and one item by path.
        QVERIFY(QFile::remove(copy));
        QCOMPARE(call("media.list", {{"project", m_project}})
                     .data["media"]
                     .toArray()
                     .last()
                     .toObject()["offline"]
                     .toBool(),
            true);
        bad("media.relink", {{"folder", path("no-such-dir")}}, "file_not_found");
        bad("media.relink", {}, "bad_value");
        bad("media.relink", {{"id", "nope"}, {"path", kVideo}}, "unknown_media");
        QTemporaryDir where;
        QVERIFY(QFile::copy(kVideo, where.filePath("copy.mp4")));
        const CommandResult relinked = ok("media.relink", {{"folder", where.path()}});
        QCOMPARE(relinked.data["relinked"].toInt(), 1);
        QCOMPARE(relinked.data["offline"].toInt(), 0);
        const QString lastId = call("media.list", {{"project", m_project}})
                                   .data["media"]
                                   .toArray()
                                   .last()
                                   .toObject()["id"]
                                   .toString();
        QVERIFY(ok("media.relink", {{"id", lastId}, {"path", kVideo}}).ok);
        QCOMPARE(load()->mediaPool()->item(lastId)->path, kVideo);
    }

    void timelines()
    {
        makeProject();
        const CommandResult created
            = ok("timeline.new", {{"name", "Second"}, {"video-tracks", 2}, {"audio-tracks", 3}});
        const qint64 uid = created.data["id"].toInteger();
        QVERIFY(uid > 0);
        const QJsonArray list
            = call("timeline.list", {{"project", m_project}}).data["timelines"].toArray();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.last().toObject()["videoTracks"].toInt(), 2);
        QCOMPARE(list.last().toObject()["audioTracks"].toInt(), 3);
        QCOMPARE(list.last().toObject()["active"].toBool(), false);
        bad("timeline.new", {{"video-tracks", 0}}, "bad_value");
        bad("timeline.select", {{"timeline", "Missing"}}, "unknown_timeline");
        bad("timeline.select", {}, "missing_parameter");
        QVERIFY(ok("timeline.select", {{"timeline", "Second"}}).ok);
        QCOMPARE(load()->activeTimelineUid(), quint64(uid));
        // --timeline addresses a timeline for one call only: the active one does not change.
        const qint64 other = list.first().toObject()["id"].toInteger();
        ok("clip.add", {{"media", videoId()}, {"timeline", QString::number(other)}});
        QCOMPARE(load()->activeTimelineUid(), quint64(uid));
        QCOMPARE(clips().size(), 0); // the active (second) timeline is empty
        QCOMPARE(call("clip.list", {{"project", m_project}, {"timeline", QString::number(other)}})
                     .data["clips"]
                     .toArray()
                     .size(),
            2);
    }

    void clipAddAndList()
    {
        makeProject();
        // Appended: video + audio, linked, back to back.
        const CommandResult a = ok("clip.add", {{"media", videoId()}});
        QCOMPARE(a.data["clips"].toArray().size(), 2);
        QCOMPARE(clips("V1").size(), 1);
        QCOMPARE(clips("A1").size(), 1);
        const QJsonObject v = clips("V1").first().toObject();
        QCOMPARE(v["start"].toInteger(), 0);
        QCOMPARE(v["duration"].toInteger(), 240); // 10 s at 24 fps
        QCOMPARE(v["startTimecode"].toString(), QString("00:00:00:00"));
        QCOMPARE(v["durationTimecode"].toString(), QString("00:00:10:00"));
        QCOMPARE(v["sourceInUs"].toInteger(), 0);
        QVERIFY(qAbs(v["sourceOutUs"].toInteger() - 10'000'000) < 50'000);
        QCOMPARE(v["track"].toString(), QString("V1"));
        QVERIFY(v["linkGroup"].toInteger() != 0);
        QCOMPARE(
            v["linkGroup"].toInteger(), clips("A1").first().toObject()["linkGroup"].toInteger());
        ok("clip.add",
            {{"media", videoId()}, {"source-in", "00:00:02:00"}, {"source-out", "00:00:04:00"}});
        QCOMPARE(clips("V1").size(), 2);
        const QJsonObject second = clips("V1").last().toObject();
        QCOMPARE(second["start"].toInteger(), 240); // appended after the first
        QCOMPARE(second["duration"].toInteger(), 48);
        QCOMPARE(second["sourceInUs"].toInteger(), 2'000'000);
        // At a time, by timecode; audio-only media (looked up by name) on A2 needs that track.
        ok("clip.add", {{"media", "sine.mp3"}, {"track", "A1"}, {"at", "00:00:20:00"}});
        QCOMPARE(clips("A1").last().toObject()["start"].toInteger(), 480);
        QCOMPARE(clips().size(), 5);
        // Insert pushes later clips right.
        ok("clip.add",
            {{"media", videoId()}, {"at", 0}, {"insert", true}, {"source-in", 0},
                {"source-out", 24}});
        QCOMPARE(clips("V1").size(), 3);
        QCOMPARE(clips("V1").first().toObject()["duration"].toInteger(), 24);
        QCOMPARE(clips("V1")[1].toObject()["start"].toInteger(), 24);
        QString why;
        QVERIFY2(editops::checkInvariants(load()->timeline()->data(), &why), qPrintable(why));

        // Errors.
        bad("clip.add", {{"media", "nothing"}}, "unknown_media");
        bad("clip.add", {{"media", videoId()}, {"track", "V9"}}, "unknown_track");
        bad("clip.add", {{"media", videoId()}, {"track", "X1"}}, "unknown_track");
        bad("clip.add", {{"media", videoId()}, {"at", "-5"}}, "out_of_range");
        bad("clip.add", {{"media", videoId()}, {"at", "soon"}}, "bad_time");
        bad("clip.add", {{"media", videoId()}, {"source-in", 99999}}, "out_of_range");
        bad("clip.add", {{"media", videoId()}, {"source-in", 100}, {"source-out", 50}},
            "out_of_range");
        // Sound-only media on a video track puts its sound on the audio track of that number.
        ok("clip.add", {{"media", "sine.mp3"}, {"track", "V1"}, {"at", "00:01:00:00"}});
        QCOMPARE(clips("A1").last().toObject()["start"].toInteger(), 1440);
        // A picture-only file has nothing to put on an audio track.
        ok("media.import", {{"file", QStringLiteral(KK_TEST_DATA_DIR "/generated/still.jpg")}});
        bad("clip.add", {{"media", "still.jpg"}, {"track", "A1"}}, "no_content");
        bad("clip.list", {{"track", "V7"}}, "unknown_track");
    }

    void clipMoveTrimSplitDelete()
    {
        makeProject();
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}}); // 120 frames
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}}); // 120..240
        const qint64 idV1 = firstClipId("V1");
        const qint64 idA1 = firstClipId("A1");

        // move: linked audio follows; the timeline invariants hold.
        const CommandResult moved = ok("clip.move", {{"clip", idV1}, {"to", 300}});
        QCOMPARE(moved.data["clip"].toObject()["start"].toInteger(), 300);
        QCOMPARE(clips("A1").first().toObject()["start"].toInteger() == 300
                || clips("A1").last().toObject()["start"].toInteger() == 300,
            true);
        // to another track of the same kind; unlinked leaves the audio alone.
        ok("clip.move", {{"clip", idV1}, {"track", "V1"}, {"to", 120}, {"unlinked", true}});
        bad("clip.move", {{"clip", idV1}, {"to", -1}}, "out_of_range");
        bad("clip.move", {{"clip", idV1}, {"track", "A1"}}, "bad_value");
        bad("clip.move", {{"clip", idV1}, {"track", "V4"}}, "unknown_track");
        bad("clip.move", {{"clip", 9999}, {"to", 5}}, "unknown_clip");
        bad("clip.move", {{"clip", idV1}}, "missing_parameter");
        bad("clip.move", {{"clip", idV1}, {"to", 120}}, "nothing_to_do");

        // trim: tail shorter, head later; limits; ripple.
        makeProject();
        ok("clip.add",
            {{"media", videoId()}, {"source-in", "00:00:01:00"}, {"source-out", "00:00:05:00"}});
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}});
        const qint64 first = firstClipId("V1");
        const CommandResult t
            = ok("clip.trim", {{"clip", first}, {"edge", "tail"}, {"delta", -10}});
        QCOMPARE(t.data["applied"].toInteger(), -10);
        QCOMPARE(t.data["clip"].toObject()["duration"].toInteger(), 86);
        QCOMPARE(clips("A1").first().toObject()["duration"].toInteger(), 86); // linked partner
        ok("clip.trim", {{"clip", first}, {"edge", "head"}, {"delta", "00:00:00:10"}});
        QCOMPARE(clips("V1").first().toObject()["duration"].toInteger(), 76);
        QCOMPARE(clips("V1").first().toObject()["sourceInUs"].toInteger() > 1'000'000, true);
        const CommandResult rip
            = ok("clip.trim", {{"clip", first}, {"edge", "tail"}, {"delta", -6}, {"ripple", true}});
        QVERIFY(rip.ok);
        QCOMPARE(clips("V1").first().toObject()["duration"].toInteger(), 70);
        QCOMPARE(clips("V1").last().toObject()["start"].toInteger(), 90); // 96 pulled left by 6
        // Past the limit the edge stops there and the result says so; where it cannot move at all
        // (the second clip starts at the beginning of its media) it is an error.
        const CommandResult clamped
            = ok("clip.trim", {{"clip", first}, {"edge", "head"}, {"delta", 100000}});
        QCOMPARE(clamped.data["clamped"].toBool(), true);
        QCOMPARE(clamped.data["clip"].toObject()["duration"].toInteger(), 1);
        bad("clip.trim",
            {{"clip", clips("V1").last().toObject()["id"].toInteger()}, {"edge", "head"},
                {"delta", -5}},
            "out_of_range");
        bad("clip.trim", {{"clip", first}, {"edge", "tail"}, {"delta", 0}}, "bad_value");
        bad("clip.trim", {{"clip", first}, {"edge", "middle"}, {"delta", 1}}, "bad_value");
        bad("clip.trim", {{"clip", 9999}, {"edge", "tail"}, {"delta", 1}}, "unknown_clip");
    }

    void clipSplitAndDelete()
    {
        makeProject();
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}}); // 0..120 V1 + A1
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}}); // 120..240
        const qint64 idV1 = firstClipId("V1");

        // Split one clip (and its linked audio), strictly inside it.
        const CommandResult s = ok("clip.split", {{"at", 60}, {"clip", idV1}});
        QCOMPARE(s.data["clips"].toArray().size(), 2);
        QCOMPARE(clips("V1").size(), 3);
        QCOMPARE(clips("A1").size(), 3);
        QCOMPARE(clips("V1")[1].toObject()["start"].toInteger(), 60);
        QCOMPARE(clips("V1")[0].toObject()["duration"].toInteger(), 60);
        bad("clip.split", {{"at", 120}, {"clip", idV1}}, "out_of_range"); // not inside it
        bad("clip.split", {{"at", 5000}}, "out_of_range"); // nothing there
        bad("clip.split", {{"at", 5}, {"clip", 9999}}, "unknown_clip");
        // Without a clip: every clip on the auto-selected tracks at that frame.
        const CommandResult all = ok("clip.split", {{"at", "00:00:07:00"}}); // frame 168
        QCOMPARE(all.data["clips"].toArray().size(), 2);
        QCOMPARE(clips("V1").size(), 4);

        // Delete: a gap stays; ripple closes it; linked audio goes with the clip.
        const qint64 mid = clips("V1")[1].toObject()["id"].toInteger(); // 60..120
        const CommandResult d = ok("clip.delete", {{"clip", QString::number(mid)}});
        QCOMPARE(d.data["deleted"].toInteger(), 2);
        QCOMPARE(clips("V1").size(), 3);
        QCOMPARE(clips("V1")[1].toObject()["start"].toInteger(), 120); // gap 60..120 remains
        const qint64 third = clips("V1")[1].toObject()["id"].toInteger();
        ok("clip.delete", {{"clip", QJsonArray {QString::number(third)}}, {"ripple", true}});
        QCOMPARE(clips("V1").size(), 2);
        QCOMPARE(clips("V1")[1].toObject()["start"].toInteger(), 120); // 168 less the 48 removed
        QCOMPARE(clips("A1")[1].toObject()["start"].toInteger(), 120);
        bad("clip.delete", {{"clip", "9999"}}, "unknown_clip");
        bad("clip.delete", {{"clip", "abc"}}, "bad_value");
        // Several ids in one call, only the named clip with --unlinked.
        const qint64 a1 = firstClipId("A1");
        const CommandResult u
            = ok("clip.delete", {{"clip", QString::number(a1)}, {"unlinked", true}});
        QCOMPARE(u.data["deleted"].toInteger(), 1);
        QCOMPARE(clips("A1").size(), 1);
        QCOMPARE(clips("V1").size(), 2);
    }

    void clipSet()
    {
        makeProject();
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}}); // 120 frames
        const qint64 v = firstClipId("V1");
        const qint64 a = firstClipId("A1");
        const CommandResult r = ok("clip.set",
            {{"clip", v}, {"speed", 2}, {"name", "Fast"}, {"zoom", 1.5}, {"pos-x", 40},
                {"pos-y", -20}, {"rotation", 15}, {"opacity", 50}});
        const QJsonObject c = r.data["clip"].toObject();
        QCOMPARE(c["speed"].toDouble(), 2.0);
        QCOMPARE(c["duration"].toInteger(), 60); // twice as fast, half as long
        QCOMPARE(c["name"].toString(), QString("Fast"));
        QCOMPARE(c["transform"].toObject()["zoomX"].toDouble(), 1.5);
        QCOMPARE(c["transform"].toObject()["zoomY"].toDouble(), 1.5);
        QCOMPARE(c["transform"].toObject()["posX"].toDouble(), 40.0);
        QCOMPARE(c["transform"].toObject()["posY"].toDouble(), -20.0);
        QCOMPARE(c["transform"].toObject()["rotation"].toDouble(), 15.0);
        QCOMPARE(c["transform"].toObject()["opacity"].toDouble(), 50.0);
        QCOMPARE(clips("A1").first().toObject()["speed"].toDouble(), 2.0); // linked follows
        const CommandResult audio
            = ok("clip.set", {{"clip", a}, {"volume", -6}, {"enabled", false}});
        QCOMPARE(audio.data["clip"].toObject()["volumeDb"].toDouble(), -6.0);
        QCOMPARE(audio.data["clip"].toObject()["enabled"].toBool(), false);
        QCOMPARE(load()->timeline()->data().audioTracks[0].clips[0].enabled, false);
        ok("clip.set", {{"clip", a}, {"enabled", true}});
        QCOMPARE(clips("A1").first().toObject()["enabled"].toBool(), true);

        bad("clip.set", {{"clip", v}}, "missing_parameter");
        bad("clip.set", {{"clip", v}, {"speed", 0}}, "out_of_range");
        bad("clip.set", {{"clip", v}, {"speed", 1000}}, "out_of_range");
        bad("clip.set", {{"clip", v}, {"opacity", 150}}, "out_of_range");
        bad("clip.set", {{"clip", v}, {"volume", 40}}, "out_of_range");
        bad("clip.set", {{"clip", v}, {"zoom", 0}}, "out_of_range");
        bad("clip.set", {{"clip", 9999}, {"speed", 1}}, "unknown_clip");
    }

    void markers()
    {
        makeProject();
        ok("marker.add",
            {{"at", "00:00:02:00"}, {"name", "Intro"}, {"color", "red"}, {"note", "check"}});
        ok("marker.add", {{"at", 10}});
        const QJsonArray list
            = call("marker.list", {{"project", m_project}}).data["markers"].toArray();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].toObject()["frame"].toInteger(), 10); // sorted by frame
        QCOMPARE(list[1].toObject()["frame"].toInteger(), 48);
        QCOMPARE(list[1].toObject()["name"].toString(), QString("Intro"));
        QCOMPARE(list[1].toObject()["color"].toString(), QString("red"));
        QCOMPARE(list[1].toObject()["note"].toString(), QString("check"));
        QCOMPARE(list[1].toObject()["timecode"].toString(), QString("00:00:02:00"));
        QCOMPARE(load()->timeline()->data().markers.size(), 2);
        bad("marker.add", {{"at", 10}}, "marker_exists");
        bad("marker.add", {{"at", -3}}, "out_of_range");
        bad("marker.add", {{"at", 11}, {"color", "plaid"}}, "bad_value");
        bad("marker.add", {{"at", 11}, {"duration", 0}}, "out_of_range");
        ok("marker.delete", {{"at", 10}});
        QCOMPARE(call("marker.list", {{"project", m_project}}).data["markers"].toArray().size(), 1);
        bad("marker.delete", {{"at", 10}}, "unknown_marker");
    }

    void transitions()
    {
        makeProject();
        ok("clip.add",
            {{"media", videoId()}, {"source-in", "00:00:01:00"}, {"source-out", "00:00:05:00"}});
        ok("clip.add",
            {{"media", videoId()}, {"source-in", "00:00:01:00"}, {"source-out", "00:00:05:00"}});
        const qint64 second = clips("V1").last().toObject()["id"].toInteger();
        const CommandResult r
            = ok("transition.add", {{"clip", second}, {"edge", "head"}, {"duration", 12}});
        QCOMPARE(r.data["clip"].toObject()["transitionIn"].toObject()["type"].toString(),
            QString("cross_dissolve"));
        QVERIFY(r.data["clip"].toObject()["transitionIn"].toObject()["duration"].toInteger() > 0);
        const auto& placed = load()->timeline()->data().videoTracks[0].clips[1];
        QVERIFY(placed.transitionIn.has_value());
        // Audio gets an audio transition by default; a video type on audio is refused.
        const qint64 audio = clips("A1").last().toObject()["id"].toInteger();
        ok("transition.add", {{"clip", audio}, {"edge", "head"}, {"duration", 6}});
        bad("transition.add", {{"clip", audio}, {"edge", "tail"}, {"type", "cross_dissolve"}},
            "bad_value");
        bad("transition.add", {{"clip", second}, {"edge", "tail"}, {"type", "no_such"}},
            "bad_value");
        bad("transition.add", {{"clip", 9999}, {"edge", "tail"}}, "unknown_clip");
        bad("transition.add", {{"clip", second}, {"edge", "tail"}, {"duration", 0}},
            "out_of_range");
    }

    void exportAndImportOtio()
    {
        makeProject();
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:05:00"}});
        ok("marker.add", {{"at", 24}});
        const QString otio = path("t.otio");
        const QString edl = path("t.edl");
        const CommandResult e = ok("export.otio", {{"file", otio}});
        QVERIFY(QFile::exists(otio));
        QVERIFY(e.data["bytes"].toInt() > 100);
        QFile f(otio);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(f.readAll()).isObject());
        ok("export.edl", {{"file", edl}});
        QFile ef(edl);
        QVERIFY(ef.open(QIODevice::ReadOnly));
        QVERIFY(ef.readAll().contains("TITLE"));
        bad("export.otio", {{"file", path("no-dir/x.otio")}}, "write_failed");

        const CommandResult imported = ok("import.otio", {{"file", otio}});
        QVERIFY(imported.data["timeline"].toInteger() > 0);
        QVERIFY(imported.data["clips"].toInt() >= 1);
        QCOMPARE(
            call("timeline.list", {{"project", m_project}}).data["timelines"].toArray().size(), 2);
        bad("import.otio", {{"file", path("missing.otio")}}, "file_not_found");
        QFile junk(path("junk.otio"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("{}");
        junk.close();
        bad("import.otio", {{"file", path("junk.otio")}}, "import_failed");
    }

    void outputLeavesTheProjectUntouched()
    {
        makeProject();
        const QString before = path("before.kkproj");
        QVERIFY(QFile::copy(m_project, before));
        const QString out = path("after.kkproj");
        const CommandResult r
            = call("clip.add", {{"project", m_project}, {"media", videoId()}, {"output", out}});
        QVERIFY(r.ok);
        QCOMPARE(r.data["project"].toString(), out);
        QCOMPARE(clips().size(), 0); // the original has no clips
        Project p;
        QVERIFY(p.load(out));
        QVERIFY(!p.timeline()->data().videoTracks[0].clips.empty());
    }

    void renderAudioAndVideo()
    {
        makeProject();
        bad("render", {{"output", path("e.wav")}}, "empty_timeline");
        ok("clip.add", {{"media", videoId()}, {"source-out", "00:00:00:12"}}); // half a second
        // Audio only, with a preset: fast, no compositor.
        QList<double> seen;
        const CommandResult wav = CommandRegistry::builtin().execute("render",
            {{"project", m_project}, {"output", path("out.wav")}, {"preset", "audio_wav"}},
            [&](double f, const QString&) { seen << f; });
        QVERIFY2(wav.ok, qPrintable(wav.errorCode + " " + wav.error));
        QVERIFY(QFileInfo(path("out.wav")).size() > 1000);
        // Video, preset by the file extension.
        const CommandResult mp4 = ok("render", {{"output", path("out.mp4")}});
        QVERIFY(QFileInfo(path("out.mp4")).size() > 1000);
        QCOMPARE(mp4.data["frames"].toInteger(), 12);
        const media::MediaInfo info = media::probe(path("out.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.width, 640);
        // The project file is not changed by rendering.
        QCOMPARE(clips().size(), 2);
        bad("render", {{"output", path("x.mp4")}, {"preset", "no_such_preset"}}, "render_failed");
        bad("render", {{"output", path("x.unknown")}}, "render_failed");
    }
};

QTEST_MAIN(TestCommands)
#include "test_commands.moc"
