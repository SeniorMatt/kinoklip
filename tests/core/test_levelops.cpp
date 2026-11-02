// SPDX-License-Identifier: GPL-3.0-or-later
// Level lines, fades and Clip Attributes in core (spec section 32): LevelOps edits, fades on
// split / join, attributes mirrored from the media pool to clips (undo included), project
// format 10 round trip and older files.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace kk;
using namespace kk::test;
namespace lo = levelops;

class TestLevelOps : public QObject
{
    Q_OBJECT

    /// Timeline with one audio clip (id returned) on A1, frames [10, 110).
    static quint64 audioClip(TimelineData& tl)
    {
        ClipData c = mediaClip(10, 100);
        c.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(c);
        return c.id;
    }

private slots:
    void offsetWithoutKeys()
    {
        TimelineData tl = timeline();
        const quint64 id = audioClip(tl);
        QVERIFY(lo::offsetVolume(tl, id, 20, -6.0));
        QCOMPARE(lo::volumeDbAt(*editops::clipById(tl, id), 50), -6.0);
        QVERIFY(lo::offsetVolume(tl, id, 20, 100.0)); // clamped to the maximum
        QCOMPARE(lo::volumeDbAt(*editops::clipById(tl, id), 0), lo::kMaxVolumeDb);
        QVERIFY(!lo::offsetVolume(tl, id, 20, 0.0));
    }

    void keysAndSegments()
    {
        TimelineData tl = timeline();
        const quint64 id = audioClip(tl);
        const auto k1 = lo::addVolumeKey(tl, id, 20);
        QVERIFY(k1);
        QCOMPARE(k1->paramPath, lo::volumePath());
        QVERIFY(lo::addVolumeKey(tl, id, 60));
        QVERIFY(lo::addVolumeKey(tl, id, 80));
        const AnimatedValue& v = editops::clipById(tl, id)->audio.values[keys::VolumeDb];
        QCOMPARE(v.keys.size(), 3);
        QCOMPARE(v.keys[0].value, 0.0); // the line's value at the key
        // Dragging the segment between 20 and 60 moves those two keys only.
        QVERIFY(lo::offsetVolume(tl, id, 40, -10.0));
        const AnimatedValue& w = editops::clipById(tl, id)->audio.values[keys::VolumeDb];
        QCOMPARE(w.keys[0].value, -10.0);
        QCOMPARE(w.keys[1].value, -10.0);
        QCOMPARE(w.keys[2].value, 0.0);
        // After the last key: the last key.
        QVERIFY(lo::offsetVolume(tl, id, 95, 3.0));
        QCOMPARE(editops::clipById(tl, id)->audio.values[keys::VolumeDb].keys[2].value, 3.0);
        // Exactly on a key: only that key.
        QVERIFY(lo::offsetVolume(tl, id, 60, 1.0));
        QCOMPARE(editops::clipById(tl, id)->audio.values[keys::VolumeDb].keys[1].value, -9.0);
        QCOMPARE(editops::clipById(tl, id)->audio.values[keys::VolumeDb].keys[0].value, -10.0);
        // Moving a key in time and value; clamped inside the clip.
        const auto moved = lo::setVolumeKey(tl, *k1, 30, -20.0);
        QVERIFY(moved);
        QCOMPARE(moved->frame, 30);
        const ClipData* c = editops::clipById(tl, id);
        QCOMPARE(c->audio.values[keys::VolumeDb].keys[0].frame, 30);
        QCOMPARE(c->audio.values[keys::VolumeDb].keys[0].value, -20.0);
        const auto clamped = lo::setVolumeKey(tl, *moved, -50, -20.0);
        QCOMPARE(clamped->frame, 0);
        // The mixer's envelope follows: half way between keys 0 (-20 dB) and 60 (-9 dB).
        QVERIFY(std::abs(lo::volumeDbAt(*editops::clipById(tl, id), 30) - (-14.5)) < 1e-9);
    }

    void fades()
    {
        TimelineData tl = timeline();
        const quint64 id = audioClip(tl);
        QVERIFY(lo::setFade(tl, id, true, 30));
        QVERIFY(lo::setFade(tl, id, false, 200)); // limited by the fade-in: 100 - 30
        QCOMPARE(editops::clipById(tl, id)->fadeOutFrames, 70);
        QVERIFY(!lo::setFade(tl, id, false, 70));
        QVERIFY(lo::setFade(tl, id, false, 20));
        // Split: the left part keeps the fade-in, the right part the fade-out.
        const QVector<quint64> right = editops::splitClips(tl, {id}, 60);
        QCOMPARE(right.size(), 1);
        const ClipData* l = editops::clipById(tl, id);
        const ClipData* r = editops::clipById(tl, right.front());
        QCOMPARE(l->fadeInFrames, 30);
        QCOMPARE(l->fadeOutFrames, 0);
        QCOMPARE(r->fadeInFrames, 0);
        QCOMPARE(r->fadeOutFrames, 20);
        // Trimming keeps the fades.
        editops::trimTail(tl, right.front(), -5, false);
        QCOMPARE(editops::clipById(tl, right.front())->fadeOutFrames, 20);
    }

    void serialization()
    {
        ClipData c = mediaClip(0, 50);
        c.fadeInFrames = 12;
        c.fadeOutFrames = 7;
        c.attributes.pixelAspect = Rational {40, 33};
        c.attributes.levels = DataLevels::Full;
        c.attributes.fieldOrder = FieldOrder::LowerFirst;
        c.attributes.deinterlace = true;
        c.attributes.rotation = 270;
        c.attributes.channelFormat = ChannelFormat::Stereo;
        c.attributes.left = 1;
        c.attributes.right = kMutedChannel;
        c.attributes.overrideTimecode = true;
        c.attributes.startTimecodeFrames = 90000;
        const ClipData back = serialization::clipFromJson(serialization::clipToJson(c));
        QCOMPARE(back.fadeInFrames, 12);
        QCOMPARE(back.fadeOutFrames, 7);
        QVERIFY(back.attributes == c.attributes);
        // Defaults write nothing.
        const QJsonObject plain = serialization::clipToJson(mediaClip(0, 50));
        QVERIFY(!plain.contains(QLatin1String("attributes")));
        QVERIFY(!plain.contains(QLatin1String("fadeIn")));
        QVERIFY(serialization::attributesToJson(ClipAttributes {}).isEmpty());
    }

    void attributesMirrorAndUndo()
    {
        Project project;
        MediaItem item;
        item.id = QStringLiteral("media-1");
        item.path = QStringLiteral("/nonexistent/a.mov");
        item.info.hasVideo = item.info.hasAudio = true;
        project.mediaPool()->addItem(item);
        project.beginEdit(QStringLiteral("Add"));
        TimelineData& d = project.timeline()->mutableData();
        ClipData v = mediaClip(0, 50);
        v.id = d.allocateId();
        d.videoTracks[0].clips.push_back(v);
        ClipData a = mediaClip(0, 50);
        a.id = d.allocateId();
        d.audioTracks[0].clips.push_back(a);
        project.endEdit();

        ClipAttributes attrs;
        attrs.rotation = 90;
        attrs.channelFormat = ChannelFormat::Mono;
        project.beginEdit(QStringLiteral("Clip Attributes"));
        project.mediaPool()->setItemAttributes(QStringLiteral("media-1"), attrs);
        project.endEdit();
        const TimelineData& t = project.timeline()->data();
        QVERIFY(t.videoTracks[0].clips[0].attributes == attrs);
        QVERIFY(t.audioTracks[0].clips[0].attributes == attrs);
        // The render snapshot carries them.
        QVERIFY(project.timeline()->snapshot()->videoTracks[0].clips[0].attributes == attrs);
        // One undo step restores item and clips.
        project.undo();
        QVERIFY(project.mediaPool()->item(QStringLiteral("media-1"))->attributes.isDefault());
        QVERIFY(project.timeline()->data().videoTracks[0].clips[0].attributes.isDefault());
        project.redo();
        QVERIFY(project.timeline()->data().audioTracks[0].clips[0].attributes == attrs);

        // Save / load round trip (format 10).
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(project.save(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), serialization::kFormatVersion);
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QVERIFY(loaded.mediaPool()->item(QStringLiteral("media-1"))->attributes == attrs);
        QVERIFY(loaded.timeline()->data().videoTracks[0].clips[0].attributes == attrs);
    }

    void olderFormatLoads()
    {
        // A version-7 file has no fades or attributes: it loads with the defaults.
        Project project;
        project.beginEdit(QStringLiteral("Add"));
        TimelineData& d = project.timeline()->mutableData();
        ClipData c = mediaClip(0, 50);
        c.id = d.allocateId();
        d.videoTracks[0].clips.push_back(c);
        project.endEdit();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.kkproj"));
        QVERIFY(project.save(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        root[QLatin1String("formatVersion")] = 7;
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(back.fadeInFrames, 0);
        QVERIFY(back.attributes.isDefault());
    }
};

QTEST_GUILESS_MAIN(TestLevelOps)
#include "test_levelops.moc"
