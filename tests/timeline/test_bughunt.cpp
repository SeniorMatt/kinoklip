// SPDX-License-Identifier: GPL-3.0-or-later
// Bug-hunting round on 1.0.0-beta.1 (Edit page first): one test per fix, named after the spec
// row it covers.
#include "ParityTestUtils.h"
#include "ui/inspector/InspectorPanel.h"

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
};

QTEST_MAIN(TestBughunt)
#include "test_bughunt.moc"
