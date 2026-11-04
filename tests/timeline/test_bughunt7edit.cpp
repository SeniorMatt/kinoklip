// SPDX-License-Identifier: GPL-3.0-or-later
// Seventh bug-hunting round, Edit page timeline: one test per fix, named after the spec row it
// covers.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/ParamKeys.h"
#include "ui/effects/EffectActions.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTest>
#include <QTimer>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

namespace {

/// Opens the context menu of @p w at @p pos and returns the texts of its entries (read and
/// closed from a timer, since QMenu::exec() blocks).
QStringList menuTexts(TimelineWidget& w, QPoint pos)
{
    QStringList texts;
    QTimer::singleShot(0, [&texts] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions())
                if (!a->isSeparator())
                    texts.append(a->text().remove(QLatin1Char('&')));
            menu->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, pos, w.viewport()->mapToGlobal(pos));
    QCoreApplication::sendEvent(w.viewport(), &event);
    return texts;
}

} // namespace

class TestBughunt7Edit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_bughunt7edit"); }

    // Spec 14.5: a new Text title reads "Basic Title" (its clip is named after the text).
    void spec14_5_newTextTitleReadsBasicTitle()
    {
        const auto clip = effectactions::makeGeneratorClip(QStringLiteral("text_title"), {25, 1});
        QVERIFY(clip);
        QCOMPARE(clip->title.staticValue(keys::Text).toString(), QStringLiteral("Basic Title"));
        QCOMPARE(clip->name, QStringLiteral("Basic Title"));
    }

    // Spec 18.18: a new marker is named "Marker N" with N one past the highest number in use,
    // so a name never repeats after a deletion; timeline and clip markers alike.
    void spec18_18_markerNamesDoNotRepeat()
    {
        Ctl t;
        for (int64_t f : {10, 20, 30}) {
            t.tl()->setPlayhead(f);
            t.c.addMarker();
        }
        // Delete "Marker 2", then add another one: it is "Marker 4", not a second "Marker 3".
        t.f.project.beginEdit(QStringLiteral("delete"));
        t.f.data().markers.removeAt(1);
        t.f.project.endEdit();
        t.tl()->setPlayhead(40);
        t.c.addMarker();
        QStringList names;
        for (const Marker& m : t.f.view().markers)
            names << m.name;
        QCOMPARE(names,
            (QStringList {QStringLiteral("Marker 1"), QStringLiteral("Marker 3"),
                QStringLiteral("Marker 4")}));
        // Renamed markers do not count; the helper ignores names of another pattern.
        QVector<Marker> list(2);
        list[0].name = QStringLiteral("Intro");
        list[1].name = QStringLiteral("Marker 7");
        QCOMPARE(TimelineController::newMarkerName(list), QStringLiteral("Marker 8"));
        QCOMPARE(TimelineController::newMarkerName({}), QStringLiteral("Marker 1"));
    }

    // Spec 12.17: a right-click on a clip on a locked track neither selects it nor offers the
    // clip menu; the empty track area's menu shows and the selection stays.
    void spec12_17_rightClickOnLockedClip()
    {
        View v;
        const quint64 a = v.put(kV1, mediaClip(0, 20));
        const quint64 b = v.put(kV2, mediaClip(0, 20));
        v.f.selection.setClips({b});
        v.f.project.beginEdit(QStringLiteral("lock"));
        v.f.data().track(kV1)->locked = true;
        v.f.project.endEdit();
        const QStringList texts = menuTexts(v.w, v.at(10, kV1));
        QVERIFY(!v.f.selection.isSelected(a));
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {b});
        QVERIFY2(
            !texts.contains(QStringLiteral("Change Clip Speed…")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("Add Video Track")), qPrintable(texts.join('|')));
        // On an unlocked track the clip menu still selects the clip.
        const QStringList clipMenu = menuTexts(v.w, v.at(10, kV2));
        QVERIFY(v.f.selection.isSelected(b));
        QVERIFY(!clipMenu.contains(QStringLiteral("Add Video Track")));
    }
};

QTEST_MAIN(TestBughunt7Edit)
#include "test_bughunt7edit.moc"
