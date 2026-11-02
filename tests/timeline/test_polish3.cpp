// SPDX-License-Identifier: GPL-3.0-or-later
// Third polish round of the Edit page (docs/polish-v3.md, Round 3): source timecode on clips
// (spec 12.14) and the speed-change nudges (29.17).
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "ui/common/SourceTimecode.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSettings>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

/// A 25 fps media item with start timecode 01:00:00:00 (the path is the test binary, so it is
/// not offline; nothing decodes it).
QString addMedia(Project& project)
{
    MediaItem item;
    item.path = QCoreApplication::applicationFilePath();
    item.name = QStringLiteral("A001.mov");
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.frameRate = {25, 1};
    item.info.durationUs = 100'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    item.info.startTimecode = QStringLiteral("01:00:00:00");
    return project.mediaPool()->addItem(item);
}

ClipData mediaClip(const QString& media, int64_t start, int64_t duration, int64_t sourceInUs)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = media;
    c.name = QStringLiteral("A001");
    c.start = start;
    c.duration = duration;
    c.sourceInUs = sourceInUs;
    c.sourceDurationUs = 100'000'000;
    return c;
}

quint64 put(TimelineFixture& f, TrackRef ref, const ClipData& clip)
{
    f.project.beginEdit(QStringLiteral("setup"));
    const quint64 id = ops::overwriteClip(f.data(), ref, clip);
    f.project.endEdit();
    return id;
}

/// Texts of the Timeline View Options menu of @p panel (filled on show).
QMenu* viewOptionsMenu(TimelinePanel& panel)
{
    auto* button = panel.findChild<QToolButton*>(QStringLiteral("TimelineViewOptions"));
    if (!button || !button->menu())
        return nullptr;
    QMetaObject::invokeMethod(button->menu(), "aboutToShow");
    return button->menu();
}

QAction* findAction(QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions())
        if (a->text() == text)
            return a;
    return nullptr;
}

} // namespace

class TestPolish3 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { initTimelineTestCase("test_polish3"); }

    /// Source timecode of a clip: the media's start timecode plus the source in.
    void sourceTimecodeOfAClip()
    {
        TimelineFixture f;
        const QString media = addMedia(f.project);
        ClipData c = mediaClip(media, 0, 50, 4'000'000); // 4 s into the file
        QCOMPARE(sourceTimecode(c, c.sourceInUs, f.project.mediaPool(), {25, 1}),
            QStringLiteral("01:00:04:00"));
        // Clip Attributes > Timecode overrides the embedded one.
        ClipAttributes attrs;
        attrs.overrideTimecode = true;
        attrs.startTimecodeFrames = 25 * 60 * 10; // 00:10:00:00
        f.project.beginEdit(QStringLiteral("Clip Attributes"));
        QVERIFY(f.project.mediaPool()->setItemAttributes(media, attrs));
        f.project.endEdit();
        QCOMPARE(sourceTimecode(c, c.sourceInUs, f.project.mediaPool(), {25, 1}),
            QStringLiteral("00:10:04:00"));
        // Generated clips count from zero at the timeline rate.
        ClipData title = titleClip(0, 50, QStringLiteral("T"));
        QCOMPARE(sourceTimecode(title, 0, f.project.mediaPool(), {25, 1}),
            QStringLiteral("00:00:00:00"));
    }

    /// Timeline View Options > Show Source Timecode: off by default (Resolve shows names
    /// only), stored in the settings, and the clip band draws the timecode when on.
    void spec12_14_sourceTimecodeOption()
    {
        QSettings().remove(QLatin1String(prefs::kTimelineSourceTimecode));
        TimelineFixture f;
        const QString media = addMedia(f.project);
        put(f, {TrackType::Video, 0}, mediaClip(media, 0, 100, 2'000'000));
        TimelinePanel panel(f.ctx);
        panel.resize(1400, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        TimelineWidget* w = panel.timelineWidget();
        w->setThumbnailProvider({});
        w->setWaveformProvider({});
        w->setPixelsPerFrame(10.0, kHeaderWidth);
        w->scrollToFrame(0);
        QVERIFY(!w->showSourceTimecode());
        QMenu* menu = viewOptionsMenu(panel);
        QVERIFY(menu);
        QAction* option = findAction(menu, QStringLiteral("Show Source Timecode"));
        QVERIFY(option);
        QVERIFY(option->isCheckable() && !option->isChecked());

        const TimelineLayout& L = w->layout();
        const QRect band = L.rowRect(L.rowOf({TrackType::Video, 0}))
                               .adjusted(kHeaderWidth, 0, 0, 0)
                               .intersected(w->viewport()->rect());
        auto bandImage = [&] {
            w->viewport()->repaint();
            return w->viewport()->grab(band).toImage();
        };
        const QImage off = bandImage();
        option->trigger();
        QVERIFY(w->showSourceTimecode());
        QVERIFY(QSettings().value(QLatin1String(prefs::kTimelineSourceTimecode)).toBool());
        const QImage on = bandImage();
        QVERIFY(on != off); // the timecode text was drawn
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty())
            panel.grab().save(dir + QStringLiteral("/clip-source-timecode.png"));
        // A new panel restores the choice.
        TimelinePanel second(f.ctx);
        QVERIFY(second.timelineWidget()->showSourceTimecode());
        option->trigger();
        QVERIFY(!w->showSourceTimecode());
    }

    /// Increase / Decrease Clip Speed by 10%: percentage points on the selection, durations
    /// follow (later clips ripple), one named undo step; reverse clips are left alone.
    void spec29_17_speedNudges()
    {
        TimelineFixture f;
        TimelineController c(f.ctx);
        c.connectActions();
        const QString media = addMedia(f.project);
        const quint64 a = put(f, {TrackType::Video, 0}, mediaClip(media, 0, 110, 0));
        const quint64 b = put(f, {TrackType::Video, 0}, mediaClip(media, 110, 50, 0));
        f.selection.selectClip(a);
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->count();
        f.actions.action(QStringLiteral("clip.speed_increase"))->trigger();
        const ClipData* clip = ops::clipById(f.view(), a);
        QCOMPARE(clip->speed, 1.1);
        QCOMPARE(clip->duration, int64_t(100));
        QCOMPARE(ops::clipById(f.view(), b)->start, int64_t(100)); // rippled
        QCOMPARE(undo->count(), before + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Increase Clip Speed"));
        f.actions.action(QStringLiteral("clip.speed_increase"))->trigger();
        QCOMPARE(ops::clipById(f.view(), a)->speed, 1.2); // no drift
        for (int i = 0; i < 3; ++i)
            f.actions.action(QStringLiteral("clip.speed_decrease"))->trigger();
        QCOMPARE(ops::clipById(f.view(), a)->speed, 0.9);
        QCOMPARE(undo->undoText(), QStringLiteral("Decrease Clip Speed"));
        // Never below 10 %.
        for (int i = 0; i < 12; ++i)
            c.nudgeSelectionSpeed(-10);
        QCOMPARE(ops::clipById(f.view(), a)->speed, 0.1);
        const int floor = undo->count();
        QVERIFY(!c.nudgeSelectionSpeed(-10));
        QCOMPARE(undo->count(), floor);
        // A reverse clip is not nudged.
        f.project.beginEdit(QStringLiteral("setup"));
        ops::clipById(f.data(), b)->speed = -1.0;
        f.project.endEdit();
        f.selection.selectClip(b);
        QVERIFY(!c.nudgeSelectionSpeed(10));
        QCOMPARE(ops::clipById(f.view(), b)->speed, -1.0);
    }
};

QTEST_MAIN(TestPolish3)
#include "test_polish3.moc"
