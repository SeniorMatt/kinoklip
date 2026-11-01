// SPDX-License-Identifier: GPL-3.0-or-later
// Builds the demo timeline, checks it, and optionally writes it out for visual review:
//   KK_DEMO_MEDIA=<dir with interview.mp4, broll.mp4, texture.mp4, music.wav>
//   KK_DEMO_PROJECT=<out.kkproj>   saves the project (for `kinoklip --screenshot edit`)
//   KK_DEMO_PNG=<out.png>          grabs a 1600x520 TimelinePanel offscreen
#include "TimelineTestUtils.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QTest>

using namespace kk;
using namespace kk::test;

class TestTimelineDemo : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinedemo"); }

    void demoIsValidAndRenders()
    {
        TimelineFixture f;
        const int clips = buildDemoTimeline(f.project, qEnvironmentVariable("KK_DEMO_MEDIA"));
        QVERIFY(clips > 10);
        QString why;
        QVERIFY2(editops::checkInvariants(f.view(), &why), qPrintable(why));

        const QString projectPath = qEnvironmentVariable("KK_DEMO_PROJECT");
        if (!projectPath.isEmpty())
            QVERIFY(f.project.save(projectPath));

        TimelinePanel panel(f.ctx);
        panel.resize(1600, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.timelineWidget()->zoomToFit();
        f.selection.selectClip(f.view().videoTracks[1].clips.front().id);
        QTest::qWait(qEnvironmentVariableIsSet("KK_DEMO_PNG") ? 1500 : 10); // thumbnails
        const QPixmap shot = panel.grab();
        QVERIFY(!shot.isNull());
        const QString png = qEnvironmentVariable("KK_DEMO_PNG");
        if (!png.isEmpty())
            QVERIFY(shot.save(png));
    }
};

QTEST_MAIN(TestTimelineDemo)
#include "test_timelinedemo.moc"
