// SPDX-License-Identifier: GPL-3.0-or-later
// Deliver page UI test (offscreen): playhead preview reaches the viewer, presets drive the
// form, invalid combinations are normalised, "Add to Render Queue" + "Render All" render a
// job in the background, user presets can be saved. Writes a screenshot of the page with a
// finished job to $KK_SCREENSHOT_DIR/deliver_page.png when that variable is set.
#include "../panels/PanelTestUtils.h"
#include "ExportTestUtil.h"
#include "export/ExportPresets.h"
#include "export/RenderQueue.h"
#include "ui/common/Resources.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderQueuePanel.h"
#include "ui/deliver/RenderSettingsPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>

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

    /// The Resolve-style form: every option reaches ExportSettings, file naming options
    /// persist, unsupported options are disabled with a reason.
    void renderSettingsForm()
    {
        PanelFixture fx;
        QSettings().remove(QStringLiteral("deliver"));
        ExportPresets presets;
        auto* panel = new RenderSettingsPanel(fx.ctx, &presets);
        std::unique_ptr<QWidget> owner(panel);
        panel->applyPreset(QStringLiteral("youtube_1080p"));

        // Quality: the preset's CRF 18 is the "High" level; Best writes the codec's value.
        auto* level = panel->findChild<QComboBox*>(QStringLiteral("RenderQualityLevel"));
        QCOMPARE(level->currentData().toInt(), int(RenderSettingsPanel::QualityLevel::High));
        level->setCurrentIndex(level->findData(int(RenderSettingsPanel::QualityLevel::Best)));
        QCOMPARE(panel->settings().video.quality,
            RenderSettingsPanel::qualityValue(
                QStringLiteral("h264"), RenderSettingsPanel::QualityLevel::Best));
        QCOMPARE(panel->currentPresetId(), QString()); // a manual change leaves the preset
        panel->findChild<QRadioButton*>(QStringLiteral("RenderQualityRestrict"))->setChecked(true);
        QCOMPARE(panel->settings().video.rateControl, RateControl::Bitrate);
        panel->findChild<QSpinBox*>(QStringLiteral("RenderBitrate"))->setValue(12000);
        QCOMPARE(panel->settings().video.bitrateKbps, 12000);

        // Key frames and frame reordering.
        panel->findChild<QRadioButton*>(QStringLiteral("RenderKeyframesEvery"))->setChecked(true);
        panel->findChild<QSpinBox*>(QStringLiteral("RenderKeyframeInterval"))->setValue(12);
        panel->findChild<QCheckBox*>(QStringLiteral("RenderFrameReordering"))->setChecked(false);
        ExportSettings s = panel->settings();
        QCOMPARE(s.video.keyframeInterval, 12);
        QVERIFY(!s.video.frameReordering);

        // Resolution "Match timeline" = 0x0 (timeline size).
        auto* resolution = panel->findChild<QComboBox*>(QStringLiteral("RenderResolution"));
        resolution->setCurrentIndex(0);
        QCOMPARE(panel->settings().video.width, 0);

        // Unsupported options are disabled with a tooltip, not faked.
        auto* individual = panel->findChild<QRadioButton*>(QStringLiteral("RenderIndividualClips"));
        QVERIFY(!individual->isEnabled());
        QVERIFY(!individual->toolTip().isEmpty());

        // File tab: subfolder and unique names build the output path and persist.
        QTemporaryDir dir;
        s = panel->settings();
        s.outputPath = dir.filePath(QStringLiteral("cut.mp4"));
        panel->setSettings(s);
        panel->findChild<QCheckBox*>(QStringLiteral("RenderUseSubfolder"))->setChecked(true);
        panel->findChild<QLineEdit*>(QStringLiteral("RenderSubfolder"))
            ->setText(QStringLiteral("web"));
        QCOMPARE(panel->settings().outputPath, dir.filePath(QStringLiteral("web/cut.mp4")));
        QDir(dir.path()).mkpath(QStringLiteral("web"));
        QFile existing(dir.filePath(QStringLiteral("web/cut.mp4")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        panel->findChild<QCheckBox*>(QStringLiteral("RenderUniqueNames"))->setChecked(true);
        QCOMPARE(panel->settings().outputPath, dir.filePath(QStringLiteral("web/cut_1.mp4")));
        QVERIFY(QSettings().value(QStringLiteral("deliver/useSubfolder")).toBool());
        QCOMPARE(QSettings().value(QStringLiteral("deliver/subfolder")).toString(),
            QStringLiteral("web"));

        // A new panel (next start) restores the file options.
        RenderSettingsPanel again(fx.ctx, &presets);
        QVERIFY(again.findChild<QCheckBox*>(QStringLiteral("RenderUseSubfolder"))->isChecked());
        QVERIFY(again.findChild<QCheckBox*>(QStringLiteral("RenderUniqueNames"))->isChecked());
        QSettings().remove(QStringLiteral("deliver"));
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
        // Polish round 2: the queue panel says the render is complete.
        auto* queuePanel = page.findChild<RenderQueuePanel*>();
        QVERIFY(queuePanel);
        QTRY_COMPARE(queuePanel->summaryText(), QStringLiteral("Render complete: 2 jobs rendered"));
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
