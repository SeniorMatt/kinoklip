// SPDX-License-Identifier: GPL-3.0-or-later
// Mixer model tests (Audio page, spec 37): bus and routing ops, automation writing, project
// format 13 round trip, older formats (1 - 12) loading with a neutral mixer, damaged data
// repaired, undo of mixer edits.
#include "core/MixerJson.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtTest>

using namespace kk;

namespace {

/// A mixer with everything set away from the defaults.
void customize(TimelineData& d)
{
    TrackData& a1 = d.audioTracks[0];
    a1.mix.mono = true;
    a1.mix.strip.volumeDb = -4.5;
    a1.mix.strip.pan = 0.25;
    a1.mix.strip.eq.enabled = true;
    a1.mix.strip.eq.bands[2] = EqBand {EqBandType::Notch, true, 3150.0, 0.0, 6.0};
    a1.mix.strip.dynamics.compressorEnabled = true;
    a1.mix.strip.dynamics.ratio = 3.0;
    a1.mix.strip.dynamics.limiterEnabled = true;
    a1.mix.automation = AutomationMode::Touch;
    (void)mixerops::writeAutomation(d, a1.id, mixkeys::kVolumeDb, 10, -6.0, 10);
    (void)mixerops::writeAutomation(d, a1.id, mixkeys::kPan, 20, -0.5, 20);
    const int bus2 = mixerops::addBus(d, true, QStringLiteral("Dialogue"));
    (void)mixerops::setTrackRoute(d, a1.id, bus2, true);
    mixerops::bus(d, bus2)->strip.muted = true;
    mixerops::bus(d, kMainBusId)->strip.eq.enabled = true;
    (void)mixerops::setOutputBus(d, bus2);
}

QJsonObject saveToJson(Project& project, const QString& path)
{
    if (!project.save(path))
        return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeJson(const QString& path, const QJsonObject& root)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

} // namespace

class TestMixerData : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreNeutral()
    {
        const TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1});
        QCOMPARE(int(d.mixer.buses.size()), 1);
        QCOMPARE(d.mixer.buses[0].id, kMainBusId);
        QCOMPARE(d.mixer.outputBus, kMainBusId);
        for (const TrackData& t : d.audioTracks) {
            QCOMPARE(t.mix.buses, QVector<int> {kMainBusId});
            QVERIFY(!t.mix.strip.eq.enabled);
            QCOMPARE(t.mix.strip.volumeDb, 0.0);
        }
        QCOMPARE(busDisplayName(d.mixer.buses[0]), QStringLiteral("Bus 1"));
    }

    void busOps()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1});
        const int b2 = mixerops::addBus(d, false);
        const int b3 = mixerops::addBus(d, true, QStringLiteral("Music"));
        QCOMPARE(b2, 2);
        QCOMPARE(b3, 3);
        QVERIFY(mixerops::renameBus(d, b2, QStringLiteral("FX")));
        QVERIFY(!mixerops::renameBus(d, b2, QStringLiteral("FX")));
        QVERIFY(mixerops::setBusMono(d, b2, true));
        QVERIFY(mixerops::setTrackRoute(d, d.audioTracks[0].id, b3, true));
        QVERIFY(!mixerops::setTrackRoute(d, d.audioTracks[0].id, 99, true)); // unknown bus
        QCOMPARE(d.audioTracks[0].mix.buses, (QVector<int> {kMainBusId, b3}));
        QVERIFY(mixerops::setOutputBus(d, b3));
        QVERIFY(mixerops::removeBus(d, b3));
        QCOMPARE(d.mixer.outputBus, kMainBusId);
        QCOMPARE(d.audioTracks[0].mix.buses, QVector<int> {kMainBusId});
        // Ids are never reused.
        QCOMPARE(mixerops::addBus(d, false), 4);
    }

    void automationValuesAndModes()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1});
        TrackData& t = d.audioTracks[0];
        t.mix.strip.volumeDb = -3.0;
        QCOMPARE(mixerops::faderDbAt(t, 5.0), -3.0); // no curve: the static fader
        QVERIFY(mixerops::writeAutomation(d, t.id, mixkeys::kVolumeDb, 0, 0.0, 0));
        QVERIFY(mixerops::writeAutomation(d, t.id, mixkeys::kVolumeDb, 10, -10.0, 10));
        QVERIFY(std::abs(mixerops::faderDbAt(t, 5.0) + 5.0) < 1e-9);
        t.mix.automation = AutomationMode::Off;
        QCOMPARE(mixerops::faderDbAt(t, 5.0), -3.0);
        t.mix.automation = AutomationMode::Write;
        QVERIFY(mixerops::readsAutomation(t.mix, mixkeys::kVolumeDb));
        // Values are clamped to the fader range.
        QVERIFY(mixerops::writeAutomation(d, t.id, mixkeys::kVolumeDb, 20, 40.0, 20));
        QCOMPARE(mixerops::faderDbAt(t, 30.0), kFaderMaxDb);
        QVERIFY(mixerops::clearAutomation(d, t.id, mixkeys::kVolumeDb));
        QCOMPARE(mixerops::faderDbAt(t, 5.0), -3.0);
    }

    void jsonRoundTrip()
    {
        Project project;
        project.beginEdit(QStringLiteral("Mix"));
        customize(project.timeline()->mutableData());
        project.endEdit();
        const TimelineData before = project.timeline()->data();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("mix.kkproj"));
        const QJsonObject root = saveToJson(project, path);
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), 13);
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const TimelineData& after = loaded.timeline()->data();
        QVERIFY(after.mixer == before.mixer);
        QCOMPARE(after.audioTracks.size(), before.audioTracks.size());
        for (size_t i = 0; i < after.audioTracks.size(); ++i)
            QVERIFY2(after.audioTracks[i].mix == before.audioTracks[i].mix,
                qPrintable(QString::number(i)));
    }

    void defaultMixerWritesNothing()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1});
        const QJsonObject o = serialization::timelineToJson(d);
        QVERIFY(!o.contains(QLatin1String("mixer")));
        for (const QJsonValue& t : o.value(QLatin1String("audioTracks")).toArray())
            QVERIFY(!t.toObject().contains(QLatin1String("mix")));
    }

    void olderFormatsLoadWithANeutralMixer()
    {
        // A project saved without mixer data, labelled with every older format version
        // (1 - 12; 12 is the colour page's), loads and gets the default mixer.
        Project project;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.kkproj"));
        QJsonObject root = saveToJson(project, path);
        QVERIFY(!root.isEmpty());
        for (int version = 3; version <= 12; ++version) {
            root[QLatin1String("formatVersion")] = version;
            writeJson(path, root);
            Project loaded;
            QString error;
            QVERIFY2(loaded.load(path, &error), qPrintable(error));
            const TimelineData& d = loaded.timeline()->data();
            QVERIFY(d.mixer == MixerData {});
            for (const TrackData& t : d.audioTracks)
                QVERIFY(t.mix == TrackMix {});
        }
        // Versions 1 and 2 store a single "timeline" object (converted by Project).
        QJsonObject v2 = root;
        const QJsonArray timelines = root.value(QLatin1String("timelines")).toArray();
        QVERIFY(!timelines.isEmpty());
        v2.remove(QLatin1String("timelines"));
        v2[QLatin1String("timeline")] = timelines.first();
        for (int version : {1, 2}) {
            v2[QLatin1String("formatVersion")] = version;
            writeJson(path, v2);
            Project loaded;
            QString error;
            QVERIFY2(loaded.load(path, &error), qPrintable(error));
            QVERIFY(loaded.timeline()->data().mixer == MixerData {});
        }
        // A newer format is refused.
        root[QLatin1String("formatVersion")] = serialization::kFormatVersion + 1;
        writeJson(path, root);
        Project newer;
        QVERIFY(!newer.load(path));
    }

    void damagedMixerDataIsRepaired()
    {
        QJsonObject mixer {{QStringLiteral("buses"),
                               QJsonArray {QJsonObject {{QStringLiteral("id"), 3}},
                                   QJsonObject {{QStringLiteral("id"), 3}},
                                   QJsonObject {{QStringLiteral("id"), -1}}}},
            {QStringLiteral("output"), 7}, {QStringLiteral("nextBusId"), 1}};
        const MixerData m = serialization::mixerFromJson(mixer);
        QCOMPARE(int(m.buses.size()), 2); // main bus added, duplicate and invalid dropped
        QCOMPARE(m.buses[0].id, kMainBusId);
        QCOMPARE(m.outputBus, kMainBusId);
        QCOMPARE(m.nextBusId, 4);
        // Out-of-range values are clamped.
        const ChannelStrip s = serialization::stripFromJson(
            QJsonObject {{QStringLiteral("volume"), 300.0}, {QStringLiteral("pan"), -9.0},
                {QStringLiteral("eq"),
                    QJsonObject {{QStringLiteral("on"), true},
                        {QStringLiteral("bands"),
                            QJsonArray {QJsonObject {
                                {QStringLiteral("freq"), 1e9}, {QStringLiteral("q"), 0.0}}}}}}});
        QCOMPARE(s.volumeDb, kFaderMaxDb);
        QCOMPARE(s.pan, -1.0);
        QCOMPARE(s.eq.bands[0].frequencyHz, kEqMaxFrequencyHz);
        QCOMPARE(s.eq.bands[0].q, kEqMinQ);
        // Routes to missing buses are dropped on load.
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1});
        d.audioTracks[0].mix.buses = {kMainBusId, 5};
        TimelineData back;
        QString error;
        QVERIFY(serialization::timelineFromJson(serialization::timelineToJson(d), back, &error));
        QCOMPARE(back.audioTracks[0].mix.buses, QVector<int> {kMainBusId});
    }

    void mixerEditsAreOneUndoStep()
    {
        Project project;
        const int before = project.undoStack()->count();
        project.beginEdit(QStringLiteral("Add Bus"));
        mixerops::addBus(project.timeline()->mutableData(), false);
        project.endEdit();
        QCOMPARE(project.undoStack()->count(), before + 1);
        QCOMPARE(int(project.timeline()->data().mixer.buses.size()), 2);
        project.undo();
        QCOMPARE(int(project.timeline()->data().mixer.buses.size()), 1);
        project.redo();
        QCOMPARE(int(project.timeline()->data().mixer.buses.size()), 2);
    }
};

QTEST_GUILESS_MAIN(TestMixerData)
#include "test_mixerdata.moc"
