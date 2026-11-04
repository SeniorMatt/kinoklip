// SPDX-License-Identifier: GPL-3.0-or-later
// Bug-hunting round on 1.0.0-beta.1 (Edit page first): one test per fix, named after the spec
// row it covers.
#include "ParityTestUtils.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ValueField.h"

#include <QSettings>
#include <QTabWidget>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

class TestBughunt : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_bughunt"); }
    void init() { QSettings().clear(); }

    // Spec 24.15: a command that selects video + audio (a source edit, a paste, Select All,
    // a drop) keeps the Inspector on the Video tab; only clicking the audio part shows Audio.
    void spec24_15_editResultKeepsVideoTab()
    {
        Ctl t;
        InspectorPanel panel(t.f.ctx);
        const QString media = addMedia(t.f.project);
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::VideoTab));

        const QVector<quint64> ids
            = t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Overwrite);
        QCOMPARE(ids.size(), 2);
        const ClipData* current = ops::clipById(t.f.view(), t.f.selection.currentClip());
        QVERIFY(current);
        QCOMPARE(ops::findClip(t.f.view(), current->id)->track.type, TrackType::Video);
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::VideoTab));

        // Copy + paste and Select All: the same.
        t.c.copy();
        t.f.selection.clear();
        t.tl()->setPlayhead(100);
        t.c.paste();
        QCOMPARE(t.f.selection.clips().size(), 2);
        QCOMPARE(
            ops::findClip(t.f.view(), t.f.selection.currentClip())->track.type, TrackType::Video);
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::VideoTab));
        t.f.selection.clear();
        t.c.selectAll();
        QCOMPARE(
            ops::findClip(t.f.view(), t.f.selection.currentClip())->track.type, TrackType::Video);
        QCOMPARE(panel.tabs()->currentIndex(), int(InspectorPanel::VideoTab));
    }

    // Spec 24.16: the Inspector shows Transform Position Y (and Anchor Y) positive up, as
    // Resolve does; the stored value (and so every saved project) keeps its +down meaning.
    void spec24_16_positionYIsPositiveUp()
    {
        Ctl t;
        InspectorPanel panel(t.f.ctx);
        const QString media = addMedia(t.f.project);
        const QVector<quint64> ids
            = t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Overwrite);
        QVERIFY(!ids.isEmpty());
        const quint64 video = t.f.selection.currentClip();
        QCoreApplication::processEvents();
        auto* field = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::PosY);
        QVERIFY(field);

        // Typed +100: the picture moves up, i.e. 100 px towards the top (stored -100).
        emit field->valueCommitted(100.0);
        QCoreApplication::processEvents();
        const ClipData* clip = ops::clipById(t.f.view(), video);
        QCOMPARE(clip->transform.value(keys::PosY, 0), -100.0);
        QCOMPARE(field->value(), 100.0);

        // A stored offset (an older project, the on-screen control) shows negated.
        t.f.project.beginEdit(QStringLiteral("setup"));
        ops::clipById(t.tl()->mutableData(), video)->transform.setBase(keys::PosY, 40.0);
        t.f.project.endEdit();
        QCoreApplication::processEvents();
        QCOMPARE(field->value(), -40.0);
        auto* anchor = panel.findChild<ValueField*>(QStringLiteral("value:") + keys::AnchorY);
        QVERIFY(anchor);
        emit anchor->valueCommitted(10.0);
        QCOMPARE(ops::clipById(t.f.view(), video)->transform.value(keys::AnchorY, 0), -10.0);

        // The curve editors draw it negated too.
        t.f.project.beginEdit(QStringLiteral("keys"));
        ClipData* c = ops::clipById(t.tl()->mutableData(), video);
        c->transform.values[keys::PosY].setKey(0, 0.0);
        c->transform.values[keys::PosY].setKey(10, 50.0);
        t.f.project.endEdit();
        bool found = false;
        for (const keyframeops::AnimatedParam& p :
            keyframeops::animatedParams(*ops::clipById(t.f.view(), video))) {
            if (p.path.endsWith(keys::PosY)) {
                found = true;
                QVERIFY(p.displayNegated);
            }
        }
        QVERIFY(found);
    }
};

QTEST_MAIN(TestBughunt)
#include "test_bughunt.moc"
