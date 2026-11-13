// SPDX-License-Identifier: GPL-3.0-or-later
// Deliver page UI test (offscreen): playhead preview reaches the viewer, presets drive the
// form, invalid combinations are normalised, "Add to Render Queue" + "Render All" render a
// job in the background, user presets can be saved. Writes a screenshot of the page with a
// finished job to $KK_SCREENSHOT_DIR/deliver_page.png when that variable is set.
#include "../panels/PanelTestUtils.h"
#include "ExportTestUtil.h"
#include "export/ExportPresets.h"
#include "export/IndividualClips.h"
#include "export/RenderQueue.h"
#include "ui/common/Resources.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderQueuePanel.h"
#include "ui/deliver/RenderSettingsPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
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

        // Individual clips: naming and handles appear only with the radio button (spec 94).
        auto* individual = panel->findChild<QRadioButton*>(QStringLiteral("RenderIndividualClips"));
        QVERIFY(individual->isEnabled());
        auto* handles = panel->findChild<QSpinBox*>(QStringLiteral("RenderClipHandles"));
        QVERIFY(handles->isHidden() || !panel->settings().individualClips);
        individual->setChecked(true);
        handles->setValue(4);
        s = panel->settings();
        QVERIFY(s.individualClips);
        QCOMPARE(s.clipHandles, 4);
        QVERIFY(s.toJson().contains(QStringLiteral("individualClips")));
        individual->parentWidget()->findChildren<QRadioButton*>().first()->setChecked(true); // Single clip
        QVERIFY(!panel->settings().individualClips);
        QVERIFY(!panel->settings().toJson().contains(QStringLiteral("individualClips")));
        QVERIFY(!panel->settings().toJson().contains(QStringLiteral("clipHandles")));

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

    void renderRangeFollowsInOutMarks()
    {
        // Owner report on 0.4.0: with In / Out set the render still covered the whole
        // timeline; a Resolve user expects In/Out Range to be picked from the marks.
        PanelFixture fx;
        Timeline* timeline = fx.project.timeline();
        timeline->mutableData().markIn = 10;
        timeline->mutableData().markOut = 40;
        timeline->notifyChanged();
        DeliverPage page(fx.ctx);
        RenderSettingsPanel* panel = page.settingsPanel();
        QCOMPARE(panel->settings().range, RangeMode::InOut);
        // A manual choice stays while the marks do not change ...
        ExportSettings s = panel->settings();
        s.range = RangeMode::Entire;
        panel->setSettings(s);
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::Entire);
        // ... moving a mark picks the range again; clearing them goes back to the timeline.
        timeline->mutableData().markOut = 30;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::InOut);
        timeline->mutableData().markIn = -1;
        timeline->mutableData().markOut = -1;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::Entire);
    }

    /// Owner report on 1.1.0: with only an Out mark (In at the timeline start, as the viewer
    /// shows it) Deliver kept "Entire Timeline". As in Resolve, one mark is a range too: Out
    /// alone renders from the start to Out, In alone from In to the end (spec 45.4).
    void renderRangeFollowsASingleMark()
    {
        PanelFixture fx;
        Timeline* timeline = fx.project.timeline();
        DeliverPage page(fx.ctx);
        RenderSettingsPanel* panel = page.settingsPanel();
        QCOMPARE(panel->settings().range, RangeMode::Entire);

        timeline->mutableData().markOut = 40;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::InOut);
        auto [first, end] = panel->settings().frameRange(timeline->data());
        QCOMPARE(first, int64_t(0));
        QCOMPARE(end, int64_t(40));

        // A second range after the first: clear the Out, set only an In.
        timeline->mutableData().markOut = -1;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::Entire);
        timeline->mutableData().markIn = 20;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::InOut);

        // The owner's sequence: In/Out Range chosen by hand with only an Out mark, then the Out
        // mark moved. The old rule saw "no range" and switched to Entire Timeline.
        timeline->mutableData().markIn = -1;
        timeline->mutableData().markOut = 50;
        timeline->notifyChanged();
        ExportSettings s = panel->settings();
        s.range = RangeMode::InOut;
        panel->setSettings(s);
        timeline->mutableData().markOut = 60;
        timeline->notifyChanged();
        QCOMPARE(panel->settings().range, RangeMode::InOut);
    }

    /// Individual clips: three clips give three files of the right lengths, handles included.
    void individualClipsRenderOneFilePerClip()
    {
        PanelFixture fx;
        {
            fx.project.beginEdit(QStringLiteral("Clips"));
            TimelineData& tl = fx.project.timeline()->mutableData();
            tl.width = 320;
            tl.height = 180;
            tl.fps = {25, 1};
            const QColor colors[3] = {Qt::red, Qt::green, Qt::blue};
            const int64_t starts[3] = {0, 20, 50}, lengths[3] = {20, 30, 10};
            const QString names[3] = {QStringLiteral("Intro"), QStringLiteral("Main/Part"),
                QStringLiteral("Main/Part")};
            for (int i = 0; i < 3; ++i) {
                ClipData c = solidClip(tl, colors[i], starts[i], lengths[i]);
                c.name = names[i];
                addClip(tl, TrackType::Video, 0, c);
            }
            fx.project.endEdit();
        }
        QTemporaryDir dir;
        DeliverPage page(fx.ctx);
        RenderSettingsPanel* panel = page.settingsPanel();
        ExportSettings s = panel->settings();
        s.video.width = 0;
        s.video.height = 0;
        s.audio.enabled = false;
        s.outputPath = dir.filePath(QStringLiteral("ignored.mp4"));
        s.individualClips = true;
        s.clipHandles = 5;
        panel->setSettings(s);
        QVERIFY(panel->settings().individualClips);
        QCOMPARE(panel->settings().clipHandles, 5);

        // The expansion: ranges with handles clamped to the timeline, names made safe and unique.
        const auto jobs = individualClipJobs(
            fx.project.timeline()->data(), panel->settings(), MediaPathMap());
        QCOMPARE(jobs.size(), 3);
        const std::pair<int64_t, int64_t> expected[3] = {{0, 25}, {15, 55}, {45, 60}};
        const QString files[3] = {QStringLiteral("Intro.mp4"), QStringLiteral("Main_Part.mp4"),
            QStringLiteral("Main_Part_2.mp4")};
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(jobs[i].settings.rangeIn, expected[i].first);
            QCOMPARE(jobs[i].settings.rangeOut, expected[i].second);
            QCOMPARE(jobs[i].settings.range, RangeMode::Custom);
            QCOMPARE(QFileInfo(jobs[i].settings.outputPath).fileName(), files[i]);
        }
        ExportSettings numbered = panel->settings();
        numbered.clipNaming = ClipNaming::NumberedClipName;
        QCOMPARE(QFileInfo(individualClipJobs(fx.project.timeline()->data(), numbered,
                               MediaPathMap())[1].settings.outputPath).fileName(),
            QStringLiteral("002_Main_Part.mp4"));

        // Through the page: one queued job per clip, each rendered to its own file.
        RenderQueue* queue = page.renderQueue();
        QVERIFY(page.addCurrentToQueue(false) != 0);
        QCOMPARE(queue->jobs().size(), 3);
        queue->startAll();
        QVERIFY(queue->waitForIdle(120000));
        const int64_t lengthsWithHandles[3] = {25, 40, 15};
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(queue->jobs()[i].status == RenderQueue::Status::Done,
                qPrintable(queue->jobs()[i].result.error));
            const media::MediaInfo info = media::probe(dir.filePath(files[i]));
            QVERIFY(info.valid && info.hasVideo);
            QVERIFY2(std::abs(info.durationUs - lengthsWithHandles[i] * 40'000) <= 40'000,
                qPrintable(files[i] + QString::number(info.durationUs)));
        }
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("ignored.mp4"))));
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
        // The page opens as Custom Export at the timeline's own size (Match timeline).
        QCOMPARE(panel->currentPresetId(), QString());
        QCOMPARE(panel->settings().video.width, 0);
        QCOMPARE(panel->settings().video.height, 0);
        QCOMPARE(panel->settings().video.codec, QStringLiteral("h264"));
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
