// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for MediaPool (items, bins, signals), SelectionModel and the EffectRegistry catalogue.
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/SelectionModel.h"

#include <QSignalSpy>
#include <QTest>

using namespace kk;

namespace {
MediaItem item(const QString& path)
{
    MediaItem m;
    m.path = path;
    m.info.valid = true;
    m.info.path = path;
    return m;
}
} // namespace

class TestMediaPool : public QObject
{
    Q_OBJECT
private slots:
    void addRenameRemove()
    {
        MediaPool pool;
        QSignalSpy added(&pool, &MediaPool::itemAdded);
        QSignalSpy changed(&pool, &MediaPool::changed);
        const QString id = pool.addItem(item(QStringLiteral("/x/Beach.mp4")));
        QCOMPARE(added.count(), 1);
        QCOMPARE(pool.item(id)->name, QStringLiteral("Beach.mp4"));
        QVERIFY(pool.renameItem(id, QStringLiteral("Beach Day")));
        QCOMPARE(pool.item(id)->name, QStringLiteral("Beach Day"));
        QVERIFY(!pool.renameItem(QStringLiteral("nope"), QStringLiteral("x")));
        QVERIFY(pool.removeItem(id));
        QVERIFY(!pool.item(id));
        QCOMPARE(changed.count(), 3);
    }

    void duplicateIdsAreReplaced()
    {
        MediaPool pool;
        MediaItem a = item(QStringLiteral("/a.mp4"));
        a.id = QStringLiteral("same");
        MediaItem b = item(QStringLiteral("/b.mp4"));
        b.id = QStringLiteral("same");
        QCOMPARE(pool.addItem(a), QStringLiteral("same"));
        QVERIFY(pool.addItem(b) != QStringLiteral("same"));
    }

    void bins()
    {
        MediaPool pool;
        const QString footage = pool.addBin(QStringLiteral("Footage"));
        const QString day1 = pool.addBin(QStringLiteral("Day 1"), footage);
        const QString id = pool.addItem(item(QStringLiteral("/a.mp4")));
        QVERIFY(pool.moveItemToBin(id, day1));
        QCOMPARE(pool.itemIdsInBin(day1), QStringList {id});
        QVERIFY(!pool.moveItemToBin(id, QStringLiteral("missing")));
        QVERIFY(pool.renameBin(day1, QStringLiteral("Monday")));
        QCOMPARE(pool.bin(day1)->name, QStringLiteral("Monday"));
        // Removing a bin moves its contents to the parent.
        QVERIFY(pool.removeBin(day1));
        QCOMPARE(pool.item(id)->binId, footage);
        QVERIFY(pool.removeBin(footage));
        QCOMPARE(pool.item(id)->binId, QString());
        QVERIFY(pool.bins().isEmpty());
    }

    void importReportsInvalidFiles()
    {
        MediaPool pool;
        const ImportResult r = pool.importFiles({QStringLiteral("/does/not/exist.mp4")});
        QVERIFY(r.importedIds.isEmpty());
        QCOMPARE(r.warnings.size(), 1);
        QVERIFY(pool.items().isEmpty());
    }

    void setStateEmitsReset()
    {
        MediaPool pool;
        QSignalSpy reset(&pool, &MediaPool::reset);
        MediaPoolState s;
        s.items.append(item(QStringLiteral("/a.mp4")));
        s.items[0].id = QStringLiteral("a");
        pool.setState(s);
        QCOMPARE(reset.count(), 1);
        QCOMPARE(pool.state(), s);
    }

    void selection()
    {
        SelectionModel sel;
        QSignalSpy spy(&sel, &SelectionModel::changed);
        sel.setClips({3, 1, 3, 0});
        QCOMPARE(sel.clips(), (QVector<quint64> {3, 1}));
        QCOMPARE(sel.currentClip(), quint64(1));
        sel.toggleClip(3);
        QCOMPARE(sel.clips(), QVector<quint64> {1});
        sel.addClips({1, 7});
        sel.prune({7});
        QCOMPARE(sel.clips(), QVector<quint64> {7});
        sel.setKeyframes({KeyframeRef {7, QStringLiteral("transform/posX"), 4}});
        QVERIFY(!sel.isEmpty());
        sel.clear();
        QVERIFY(sel.isEmpty());
        QCOMPARE(spy.count(), 6);
    }

    void effectCatalogueIsComplete()
    {
        const EffectRegistry& reg = EffectRegistry::instance();
        const QStringList expected {QStringLiteral("gaussian_blur"),
            QStringLiteral("brightness_contrast"), QStringLiteral("saturation"),
            QStringLiteral("sharpen"), QStringLiteral("mirror"), QStringLiteral("vignette"),
            QStringLiteral("mosaic"), QStringLiteral("drop_shadow"),
            QStringLiteral("cross_dissolve"), QStringLiteral("dip_to_color"),
            QStringLiteral("wipe"), QStringLiteral("push"), QStringLiteral("solid_color"),
            QStringLiteral("text_title"), QLatin1String(groups::Transform),
            QLatin1String(groups::Cropping), QLatin1String(groups::Audio)};
        for (const QString& id : expected)
            QVERIFY2(reg.find(id), qPrintable(id));
        // The 4 first transitions + 57 of version 1.2 (tests/render/test_transitions.cpp).
        QCOMPARE(reg.byCategory(EffectCategory::Transition).size(), 61);
        // 8 v0.1 effects + 24 of the v4 batch 1 (tests/render/test_videoeffects.cpp) + 30 of
        // batch 2 (tests/render/test_videoeffects2.cpp) + 11 of version 1.2
        // (tests/render/test_effectsgap.cpp) + 17 of its second part
        // (tests/render/test_effectsb.cpp) + 9 of the multi-frame effects
        // (tests/render/test_effects5.cpp).
        QCOMPARE(reg.byCategory(EffectCategory::VideoEffect).size(), 99);

        // Every descriptor is well-formed.
        for (const EffectDescriptor& d : reg.all()) {
            QVERIFY(!d.name.isEmpty());
            for (const ParamDescriptor& p : d.params) {
                QVERIFY2(!p.key.isEmpty() && !p.label.isEmpty(), qPrintable(d.id));
                if (p.isScalar())
                    QVERIFY2(p.min <= p.def && p.def <= p.max, qPrintable(d.id + "." + p.key));
                else
                    QVERIFY2(p.defStatic.isValid(), qPrintable(d.id + "." + p.key));
                if (p.type == ParamType::Choice)
                    QCOMPARE(p.choices.size(), p.choiceLabels.size());
            }
        }
    }

    void registryDefaultsMatchParamKeys()
    {
        // The transform/audio/title descriptors and keys::defaultValue() must agree.
        const EffectRegistry& reg = EffectRegistry::instance();
        for (const char* id : {groups::Transform, groups::Cropping, groups::Audio, "text_title"}) {
            for (const ParamDescriptor& p : reg.find(QLatin1String(id))->params) {
                if (p.isScalar())
                    QCOMPARE(p.def, keys::defaultValue(p.key));
            }
        }
        const EffectInstance fx = reg.createEffect(QStringLiteral("gaussian_blur"), 9);
        QCOMPARE(fx.id, quint64(9));
        QCOMPARE(fx.params.value(QStringLiteral("radius"), 0, -1), 5.0);
        const TransitionData t = reg.createTransition(QStringLiteral("wipe"), 10);
        QCOMPARE(
            t.params.staticValue(QStringLiteral("direction")).toString(), QStringLiteral("left"));
    }
};

QTEST_GUILESS_MAIN(TestMediaPool)
#include "test_mediapool.moc"
