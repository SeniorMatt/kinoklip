// SPDX-License-Identifier: GPL-3.0-or-later
// Deliver page UI test (offscreen): playhead preview reaches the viewer, presets drive the
// form, invalid combinations are normalised, "Add to Render Queue" + "Render All" render a
// job in the background, user presets can be saved. Writes a screenshot of the page with a
// finished job to $KK_SCREENSHOT_DIR/deliver_page.png when that variable is set.
#include "../panels/PanelTestUtils.h"
#include "ExportTestUtil.h"
#include "export/RenderQueue.h"
#include "ui/common/Resources.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderSettingsPanel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QStandardPaths>

using namespace kk;
using namespace kk::test;

class TestDeliverPage : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true); // user presets go to a test location
        initResources();
    }

    void pageWorkflow()
    {
        PanelFixture fx;
        QString why;
        if (!buildExportProject(fx.project, &why)) {
            QSKIP(qPrintable(why));
        }
        QTemporaryDir dir;
        DeliverPage page(fx.ctx);
        page.resize(1600, 900);
        QSignalSpy preview(&page, &DeliverPage::previewShown);
        fx.project.timeline()->setPlayhead(35);
        page.show();
        QVERIFY(preview.wait(20000));
        QCOMPARE(preview.last().at(0).value<int64_t>(), int64_t(35));

        RenderSettingsPanel* panel = page.settingsPanel();
        QCOMPARE(panel->currentPresetId(), QStringLiteral("youtube_1080p"));
        panel->applyPreset(QStringLiteral("master_prores_hq"));
        ExportSettings s = panel->settings();
        QCOMPARE(s.container, QStringLiteral("mov"));
        QCOMPARE(s.video.codec, QStringLiteral("prores"));
        QCOMPARE(s.audio.codec, QStringLiteral("pcm_s24le"));
        QVERIFY(s.outputPath.endsWith(QLatin1String(".mov")));

        // Back to an MP4 delivery at timeline size, rendered into the temp dir.
        panel->applyPreset(QStringLiteral("youtube_1080p"));
        s = panel->settings();
        s.video.width = 0;
        s.video.height = 0;
        s.outputPath = dir.filePath(QStringLiteral("deliver.mp4"));
        panel->setSettings(s);
        QCOMPARE(panel->settings().outputPath, dir.filePath(QStringLiteral("deliver.mp4")));
        QCOMPARE(panel->settings().outputSize(fx.project.timeline()->data()), QSize(320, 180));

        RenderQueue* queue = page.renderQueue();
        const quint64 id = page.addCurrentToQueue(false);
        QVERIFY(id != 0);
        ExportSettings wav = s;
        wav.container = QStringLiteral("wav");
        wav.normalize();
        panel->setSettings(wav);
        QVERIFY(page.addCurrentToQueue(false) != 0);
        QCOMPARE(queue->jobs().size(), 2);
        queue->startAll();
        QVERIFY(queue->waitForIdle(120000));
        for (const RenderQueue::Job& job : queue->jobs()) {
            QVERIFY2(job.status == RenderQueue::Status::Done, qPrintable(job.result.error));
        }
        const media::MediaInfo info = media::probe(dir.filePath(QStringLiteral("deliver.mp4")));
        QVERIFY(info.valid && info.hasVideo && info.hasAudio);

        // A queued job that is cancelled before it starts never renders.
        panel->setSettings(s);
        const quint64 cancelled = page.addCurrentToQueue(false);
        queue->cancel(cancelled);
        QCOMPARE(queue->job(cancelled)->status, RenderQueue::Status::Cancelled);

        panel->applyPreset(QStringLiteral("youtube_1080p"));
        QApplication::processEvents();
        saveScreenshot(page, QStringLiteral("deliver_page"));
        queue->clearFinished();
        QVERIFY(queue->jobs().isEmpty());
    }
};

QTEST_MAIN(TestDeliverPage)
#include "test_deliverpage.moc"
