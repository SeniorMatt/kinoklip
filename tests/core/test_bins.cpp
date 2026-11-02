// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for media pool organisation in core: nested bins (move / re-parent / delete with
// contents, undo), clip colours, flags, metadata, usage counting (nested timelines, compound
// clips), power bins persistence, serialization round trip and the format 11 migration.
#include "CoreTestUtils.h"
#include "core/ClipColors.h"
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "core/MediaUsage.h"
#include "core/PowerBins.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

MediaItem fileItem(const QString& id, const QString& path, const QString& bin = {})
{
    MediaItem m;
    m.id = id;
    m.path = path;
    m.binId = bin;
    m.info.valid = true;
    m.info.hasVideo = true;
    m.info.width = 1920;
    m.info.height = 1080;
    m.info.frameRate = {25, 1};
    m.info.durationUs = 10'000'000;
    m.info.path = path;
    return m;
}

QStringList names(const MediaPool& pool, const QStringList& binIds)
{
    QStringList out;
    for (const QString& id : binIds)
        out.append(pool.bin(id)->name);
    return out;
}

ClipData clipOf(const QString& mediaId, int64_t start)
{
    ClipData c = mediaClip(start, 10, 0, 100, mediaId);
    c.mediaId = mediaId;
    return c;
}

} // namespace

class TestBins : public QObject
{
    Q_OBJECT
private slots:
    void moveBinReordersAndReparents()
    {
        MediaPool pool;
        const QString a = pool.addBin(QStringLiteral("A"));
        const QString b = pool.addBin(QStringLiteral("B"));
        const QString c = pool.addBin(QStringLiteral("C"));
        const QString a1 = pool.addBin(QStringLiteral("A1"), a);
        QCOMPARE(names(pool, pool.childBins({})), (QStringList {"A", "B", "C"}));

        // Reorder among siblings: C first, then B after A.
        QVERIFY(pool.moveBin(c, {}, 0));
        QCOMPARE(names(pool, pool.childBins({})), (QStringList {"C", "A", "B"}));
        QVERIFY(pool.moveBin(b, {}, 1));
        QCOMPARE(names(pool, pool.childBins({})), (QStringList {"C", "B", "A"}));
        // Re-parent B into A (last), then before A1.
        QVERIFY(pool.moveBin(b, a));
        QCOMPARE(names(pool, pool.childBins(a)), (QStringList {"A1", "B"}));
        QVERIFY(pool.moveBin(b, a, 0));
        QCOMPARE(names(pool, pool.childBins(a)), (QStringList {"B", "A1"}));
        QVERIFY(pool.binIsInside(b, a));
        QVERIFY(pool.binIsInside(a1, QString()));

        // A bin can never move into itself or its descendants.
        QVERIFY(!pool.moveBin(a, a));
        QVERIFY(!pool.moveBin(a, a1));
        QVERIFY(!pool.moveBin(a, QStringLiteral("unknown")));
        QVERIFY(!pool.moveBin(QStringLiteral("unknown"), {}));
        QCOMPARE(pool.bin(a)->parentId, QString());
    }

    void deleteBinRemovesContents()
    {
        MediaPool pool;
        const QString footage = pool.addBin(QStringLiteral("Footage"));
        const QString day = pool.addBin(QStringLiteral("Day 1"), footage);
        const QString keep = pool.addBin(QStringLiteral("Keep"));
        pool.addItem(fileItem(QStringLiteral("f1"), QStringLiteral("/f1.mp4"), footage));
        pool.addItem(fileItem(QStringLiteral("f2"), QStringLiteral("/f2.mp4"), day));
        pool.addItem(fileItem(QStringLiteral("k"), QStringLiteral("/k.mp4"), keep));
        MediaItem tl;
        tl.id = QStringLiteral("tl");
        tl.kind = MediaItemKind::Timeline;
        tl.timelineUid = 7;
        tl.binId = day;
        pool.addItem(tl);
        QSignalSpy removed(&pool, &MediaPool::itemRemoved);

        const QStringList gone = pool.removeBinWithContents(footage);
        QCOMPARE(gone, (QStringList {"f1", "f2"}));
        QCOMPARE(removed.count(), 2);
        QVERIFY(!pool.bin(footage));
        QVERIFY(!pool.bin(day));
        QVERIFY(pool.bin(keep));
        QVERIFY(pool.item(QStringLiteral("k")));
        // Timelines are never deleted with a bin: they move to Master.
        QCOMPARE(pool.item(QStringLiteral("tl"))->binId, QString());
        QVERIFY(pool.removeBinWithContents(QStringLiteral("unknown")).isEmpty());
    }

    void binOperationsUndo()
    {
        Project p;
        MediaPool* pool = p.mediaPool();
        p.beginEdit(QStringLiteral("Setup"));
        const QString a = pool->addBin(QStringLiteral("A"));
        const QString b = pool->addBin(QStringLiteral("B"));
        pool->addItem(fileItem(QStringLiteral("x"), QStringLiteral("/x.mp4"), b));
        p.endEdit();
        const MediaPoolState setup = pool->state();

        p.beginEdit(QStringLiteral("Move Bin"));
        QVERIFY(pool->moveBin(b, a));
        p.endEdit();
        p.beginEdit(QStringLiteral("Delete Bin"));
        pool->removeBinWithContents(a);
        p.endEdit();
        QVERIFY(!pool->item(QStringLiteral("x")));
        p.undo();
        QCOMPARE(pool->bin(b)->parentId, a);
        QVERIFY(pool->item(QStringLiteral("x"))); // index rebuilt after the undo
        p.undo();
        QCOMPARE(pool->state(), setup);
        p.redo();
        p.redo();
        QVERIFY(!pool->bin(a));
    }

    void colorsFlagsMetadataUndo()
    {
        Project p;
        MediaPool* pool = p.mediaPool();
        p.beginEdit(QStringLiteral("Setup"));
        pool->addItem(fileItem(QStringLiteral("x"), QStringLiteral("/x.mp4")));
        p.endEdit();

        p.beginEdit(QStringLiteral("Clip Color"));
        QVERIFY(pool->setClipColor(QStringLiteral("x"), QStringLiteral("teal")));
        p.endEdit();
        p.beginEdit(QStringLiteral("Flags"));
        QVERIFY(pool->setFlags(
            QStringLiteral("x"), flagsFromIds({QStringLiteral("red"), QStringLiteral("blue")})));
        p.endEdit();
        MediaMetadata md;
        md.description = QStringLiteral("Wide shot");
        md.keywords = {QStringLiteral(" beach "), QStringLiteral("Beach"), QStringLiteral(""),
            QStringLiteral("sunset")};
        md.rating = 9;
        p.beginEdit(QStringLiteral("Metadata"));
        QVERIFY(pool->setMetadata(QStringLiteral("x"), md));
        p.endEdit();

        const MediaItem* x = pool->item(QStringLiteral("x"));
        QCOMPARE(x->clipColor, QStringLiteral("teal"));
        QCOMPARE(flagIds(x->flags), (QStringList {"blue", "red"}));
        QCOMPARE(x->metadata.keywords, (QStringList {"beach", "sunset"})); // trimmed, unique
        QCOMPARE(x->metadata.rating, 5); // clamped
        QVERIFY(!pool->setClipColor(QStringLiteral("nope"), QStringLiteral("teal")));

        p.undo();
        QVERIFY(pool->item(QStringLiteral("x"))->metadata.isEmpty());
        p.undo();
        QCOMPARE(pool->item(QStringLiteral("x"))->flags, quint32(0));
        p.undo();
        QVERIFY(pool->item(QStringLiteral("x"))->clipColor.isEmpty());
    }

    void paletteLookups()
    {
        QCOMPARE(int(clipColorPalette().size()), 16);
        QCOMPARE(int(flagPalette().size()), 16);
        for (const PaletteColor& c : clipColorPalette()) {
            QCOMPARE(clipColorIdOf(clipColorValue(QLatin1String(c.id))), QLatin1String(c.id));
            QVERIFY(!clipColorName(QLatin1String(c.id)).isEmpty());
        }
        QVERIFY(clipColorIdOf(QColor(1, 2, 3)).isEmpty());
        QVERIFY(!clipColorValue(QStringLiteral("unknown")).isValid());
        QCOMPARE(flagCount(flagsFromIds({QStringLiteral("cyan"), QStringLiteral("mint")})), 2);
        QCOMPARE(flagsFromIds({QStringLiteral("bogus")}), quint32(0));
    }

    void usageCountsNestedAndCompound()
    {
        Project p;
        MediaPool* pool = p.mediaPool();
        p.beginEdit(QStringLiteral("Setup"));
        for (const char* id : {"m1", "m2", "m3", "m4"})
            pool->addItem(fileItem(QLatin1String(id), QStringLiteral("/%1.mp4").arg(id)));
        TimelineData& d = p.timeline()->mutableData();
        // m1: a linked V+A pair (one use) and a second video clip (two uses in total).
        const quint64 v = ops::overwriteClip(d, kV1, clipOf(QStringLiteral("m1"), 0));
        const quint64 a = ops::overwriteClip(d, kA1, clipOf(QStringLiteral("m1"), 0));
        ops::linkClips(d, {v, a});
        ops::overwriteClip(d, kV1, clipOf(QStringLiteral("m1"), 20));
        // m2: only inside a compound clip.
        const quint64 inner = ops::overwriteClip(d, kV2, clipOf(QStringLiteral("m2"), 40));
        p.endEdit();
        QString error;
        const quint64 cc = p.createCompoundClip({inner}, QStringLiteral("CC"), {}, &error);
        QVERIFY2(cc != 0, qPrintable(error));
        // m3: only in a second timeline, which is nested into the first one.
        const quint64 second = p.createTimeline(QStringLiteral("Second"));
        const quint64 first = p.activeTimelineUid();
        QVERIFY(p.openTab(second));
        p.beginEdit(QStringLiteral("m3"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, clipOf(QStringLiteral("m3"), 0));
        p.endEdit();
        QVERIFY(p.openTab(first));
        p.beginEdit(QStringLiteral("Nest"));
        ClipData nested
            = compound::makeNestedClip(*p.timelineData(second), p.timelineItemId(second));
        nested.start = 100;
        ops::overwriteClip(p.timeline()->mutableData(), kV1, nested);
        p.endEdit();

        const usage::Counts counts = usage::compute(p);
        QCOMPARE(counts.value(QStringLiteral("m1")), 2);
        QCOMPARE(counts.value(QStringLiteral("m2")), 1);
        QCOMPARE(counts.value(QStringLiteral("m3")), 1);
        QCOMPARE(counts.value(QStringLiteral("m4")), 0);
        QCOMPARE(counts.value(p.timelineItemId(cc)), 1); // the compound clip instance
        QCOMPARE(counts.value(p.timelineItemId(second)), 1); // the nested timeline
        QCOMPARE(usage::unusedFileItems(pool->state(), counts), (QStringList {"m4"}));

        const QVector<quint64> clips
            = usage::clipsUsing(p.timeline()->data(), {QStringLiteral("m1")}, pool->state());
        QCOMPARE(clips.size(), 3);
        const QVector<quint64> nestedClips
            = usage::clipsUsing(p.timeline()->data(), {p.timelineItemId(second)}, pool->state());
        QCOMPARE(nestedClips.size(), 1);
        const ClipData* n = ops::clipById(p.timeline()->data(), nestedClips.front());
        QCOMPARE(usage::itemIdOf(*n, pool->state()), p.timelineItemId(second));
    }

    void powerBinsPersistAcrossProjects()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("power-bins.json"));
        {
            PowerBins power(path);
            QVERIFY(power.pool()->items().isEmpty());
            const QString logos = power.pool()->addBin(QStringLiteral("Logos"));
            power.pool()->addBin(QStringLiteral("Dark"), logos);
            MediaItem logo
                = fileItem(QStringLiteral("logo"), QStringLiteral("/assets/logo.png"), logos);
            logo.clipColor = QStringLiteral("blue");
            power.pool()->addItem(logo);
            QVERIFY(QFile::exists(path)); // saved on change, no explicit save
        }
        // "Another project": a fresh store reading the same file.
        PowerBins again(path);
        QCOMPARE(again.pool()->bins().size(), 2);
        const MediaItem* logo = again.pool()->item(QStringLiteral("logo"));
        QVERIFY(logo);
        QCOMPARE(logo->path, QStringLiteral("/assets/logo.png")); // absolute path kept
        QCOMPARE(logo->clipColor, QStringLiteral("blue"));
        QCOMPARE(again.pool()->bin(logo->binId)->name, QStringLiteral("Logos"));
        // Projects do not see or change power bins.
        Project p;
        QVERIFY(!p.mediaPool()->item(QStringLiteral("logo")));

        // A damaged file loads empty and is kept as a backup.
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("not json");
        f.close();
        QString error;
        QVERIFY(!again.load(&error));
        QVERIFY(again.pool()->items().isEmpty());
        QVERIFY(QFile::exists(path + QStringLiteral(".bak")));
    }

    void serializationRoundTrip()
    {
        MediaPoolState s;
        s.bins = {Bin {QStringLiteral("b1"), QStringLiteral("Footage"), {}},
            Bin {QStringLiteral("b2"), QStringLiteral("Day 1"), QStringLiteral("b1")}};
        MediaItem m = fileItem(QStringLiteral("m"), QStringLiteral("/m.mov"), QStringLiteral("b2"));
        m.name = QStringLiteral("Interview");
        m.clipColor = QStringLiteral("olive");
        m.flags = flagsFromIds({QStringLiteral("green"), QStringLiteral("cream")});
        m.metadata.description = QStringLiteral("d");
        m.metadata.comments = QStringLiteral("c");
        m.metadata.keywords = {QStringLiteral("k1"), QStringLiteral("k2")};
        m.metadata.scene = QStringLiteral("12");
        m.metadata.shot = QStringLiteral("A");
        m.metadata.take = QStringLiteral("3");
        m.metadata.camera = QStringLiteral("Cam B");
        m.metadata.angle = QStringLiteral("Wide");
        m.metadata.goodTake = true;
        m.metadata.rating = 4;
        m.addedMs = 1'700'000'000'000;
        m.fileModifiedMs = 1'600'000'000'000;
        s.items = {m};
        MediaItem t;
        t.id = QStringLiteral("t");
        t.kind = MediaItemKind::Timeline;
        t.timelineUid = 3;
        t.name = QStringLiteral("Edit");
        t.clipColor = QStringLiteral("pink");
        s.items.append(t);
        SmartBin sb;
        sb.id = QStringLiteral("s1");
        sb.name = QStringLiteral("Good takes");
        sb.matchAll = false;
        sb.rules = {SmartRule {SmartField::Keywords, SmartOp::Has, QStringLiteral("k1")},
            SmartRule {SmartField::DateAdded, SmartOp::InLast, QStringLiteral("7")}};
        s.smartBins = {sb};

        const QJsonObject json = serialization::mediaPoolToJson(s, QDir(QStringLiteral("/")));
        const MediaPoolState back
            = serialization::mediaPoolFromJson(json, QDir(QStringLiteral("/")));
        QCOMPARE(back, s);

        // Defaults are not written (older readers and diffs stay small).
        const QJsonObject plain = serialization::mediaPoolToJson(
            MediaPoolState {{}, {fileItem(QStringLiteral("p"), QStringLiteral("/p.mp4"))}, {}},
            QDir(QStringLiteral("/")));
        const QJsonObject item = plain.value(QStringLiteral("items")).toArray()[0].toObject();
        QVERIFY(!item.contains(QStringLiteral("clipColor")));
        QVERIFY(!item.contains(QStringLiteral("flags")));
        QVERIFY(!item.contains(QStringLiteral("metadata")));
        QVERIFY(!plain.contains(QStringLiteral("smartBins")));

        // Unknown colours / flags / rule fields from a newer version are dropped, not kept.
        QJsonObject odd = json;
        QJsonArray items = odd.value(QStringLiteral("items")).toArray();
        QJsonObject first = items[0].toObject();
        first[QStringLiteral("clipColor")] = QStringLiteral("ultraviolet");
        first[QStringLiteral("flags")] = QJsonArray {QStringLiteral("red"), QStringLiteral("x")};
        items[0] = first;
        odd[QStringLiteral("items")] = items;
        QJsonArray smart = odd.value(QStringLiteral("smartBins")).toArray();
        QJsonObject bin = smart[0].toObject();
        QJsonArray rules = bin.value(QStringLiteral("rules")).toArray();
        rules.append(QJsonObject {{QStringLiteral("field"), QStringLiteral("mood")},
            {QStringLiteral("op"), QStringLiteral("is")}, {QStringLiteral("value"), {}}});
        bin[QStringLiteral("rules")] = rules;
        smart[0] = bin;
        odd[QStringLiteral("smartBins")] = smart;
        const MediaPoolState oddBack = serialization::mediaPoolFromJson(odd, QDir());
        QVERIFY(oddBack.items[0].clipColor.isEmpty());
        QCOMPARE(flagIds(oddBack.items[0].flags), QStringList {QStringLiteral("red")});
        QCOMPARE(oddBack.smartBins[0].rules.size(), 2);
    }

    void projectSaveLoadAndBinViews()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("bins.kkproj"));
        Project p;
        p.beginEdit(QStringLiteral("Setup"));
        const QString bin = p.mediaPool()->addBin(QStringLiteral("B"));
        MediaItem m = fileItem(QStringLiteral("m"), dir.filePath(QStringLiteral("m.mp4")), bin);
        m.flags = 1;
        p.mediaPool()->addItem(m);
        p.mediaPool()->addSmartBin(SmartBin {QStringLiteral("s"), QStringLiteral("S"), true,
            {SmartRule {SmartField::Flags, SmartOp::Has, QStringLiteral("any")}}});
        p.endEdit();
        p.setBinViewState(bin, BinViewState {QStringLiteral("list"), QStringLiteral("name"), true});
        p.setBinViewState(
            QStringLiteral("smart:s"), BinViewState {QStringLiteral("grid"), {}, false});
        QVERIFY(p.save(path));

        Project q;
        QString error;
        QVERIFY2(q.load(path, &error), qPrintable(error));
        QCOMPARE(q.mediaPool()->state(), p.mediaPool()->state());
        QCOMPARE(q.binViewState(bin).sortKey, QStringLiteral("name"));
        QVERIFY(q.binViewState(bin).descending);
        QCOMPARE(q.binViewState(QStringLiteral("smart:s")).mode, QStringLiteral("grid"));
        QCOMPARE(q.binViewState(QStringLiteral("other")), BinViewState {});
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(
            root.value(QStringLiteral("formatVersion")).toInt(), serialization::kFormatVersion);
        QVERIFY(serialization::kFormatVersion >= 11); // later branches bump it
        q.newProject();
        QCOMPARE(q.binViewState(bin), BinViewState {});
    }

    void migratesOlderProjects_data()
    {
        QTest::addColumn<int>("version");
        QTest::newRow("6 (before multicam)") << 6;
        QTest::newRow("10 (attrs, saved without bins data)") << 10;
    }

    void migratesOlderProjects()
    {
        // An older project: no colours, flags, metadata, smart bins or file times.
        QFETCH(int, version);
        QTemporaryDir dir;
        const QString media = dir.filePath(QStringLiteral("clip.mp4"));
        QFile mf(media);
        QVERIFY(mf.open(QIODevice::WriteOnly));
        mf.write("x");
        mf.close();
        Project p;
        p.beginEdit(QStringLiteral("Setup"));
        p.mediaPool()->addBin(QStringLiteral("Old Bin"));
        p.mediaPool()->addItem(fileItem(QStringLiteral("m"), media));
        p.endEdit();
        const QString path = dir.filePath(QStringLiteral("old.kkproj"));
        QVERIFY(p.save(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        root[QStringLiteral("formatVersion")] = version;
        QJsonObject pool = root.value(QStringLiteral("mediaPool")).toObject();
        QJsonArray items = pool.value(QStringLiteral("items")).toArray();
        for (int i = 0; i < items.size(); ++i) {
            QJsonObject o = items[i].toObject();
            o.remove(QStringLiteral("added"));
            o.remove(QStringLiteral("modified"));
            items[i] = o;
        }
        pool[QStringLiteral("items")] = items;
        root[QStringLiteral("mediaPool")] = pool;
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();

        Project q;
        QString error;
        QVERIFY2(q.load(path, &error), qPrintable(error));
        const MediaItem* m = q.mediaPool()->item(QStringLiteral("m"));
        QVERIFY(m);
        QVERIFY(m->clipColor.isEmpty());
        QCOMPARE(m->flags, quint32(0));
        QVERIFY(m->metadata.isEmpty());
        QCOMPARE(m->fileModifiedMs, QFileInfo(media).lastModified().toMSecsSinceEpoch());
        QCOMPARE(m->addedMs, m->fileModifiedMs);
        QCOMPARE(q.mediaPool()->bins().size(), 1);
        QVERIFY(q.mediaPool()->smartBins().isEmpty());
        QVERIFY(!q.isDirty());

        // Newer files than this build are refused.
        root[QStringLiteral("formatVersion")] = serialization::kFormatVersion + 1;
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();
        QVERIFY(!q.load(path, &error));
    }
};

QTEST_GUILESS_MAIN(TestBins)
#include "test_bins.moc"
