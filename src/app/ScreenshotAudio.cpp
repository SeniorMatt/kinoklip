// SPDX-License-Identifier: GPL-3.0-or-later
// ScreenshotAudio — demo mix, offline meter reading and composited grab.
#include "app/ScreenshotAudio.h"

#include "app/MainWindow.h"
#include "app/pages/AudioPage.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/AudioMixer.h"
#include "render/LoudnessMeter.h"
#include "render/RenderTypes.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqCurveView.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QCoreApplication>
#include <QPainter>
#include <QTimer>
#include <QUndoStack>

#include <vector>

namespace kk {

namespace {
constexpr int64_t kDemoLengthUs = 12'000'000; ///< at most this much of each file
constexpr int kMeterSeconds = 3; ///< offline reading: the mix before the playhead
} // namespace

QWidget* prepareAudioScreenshot(MainWindow& window, const QString& section, QString* error)
{
    AppContext& ctx = window.context();
    window.setCurrentPage(QStringLiteral("audio"));
    AudioPage* page = window.audioPage();
    const QStringList importPaths
        = qEnvironmentVariable("KK_SCREENSHOT_IMPORT").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (!importPaths.isEmpty())
        ctx.project->importMedia(importPaths);
    QVector<const MediaItem*> sounds;
    for (const MediaItem& m : ctx.project->mediaPool()->items()) {
        if (m.kind == MediaItemKind::File && m.info.hasAudio)
            sounds.append(&m);
    }
    Timeline* timeline = ctx.project->timeline();
    if (timeline->data().duration() == 0) {
        if (sounds.isEmpty()) {
            *error = QStringLiteral("audio: needs media with sound (KK_SCREENSHOT_IMPORT=a.mp4)");
            return nullptr;
        }
        TimelineController* tc = window.editPage()->timeline()->controller();
        for (int track = 0; track < 2; ++track) {
            const MediaItem* item = sounds[std::min<qsizetype>(track, sounds.size() - 1)];
            timeline->setPlayhead(0);
            tc->setTargetTrack(TrackType::Audio, track);
            tc->setTargetTrack(TrackType::Video, track);
            tc->insertSource(item->id, 0, std::min(item->info.durationUs, kDemoLengthUs),
                SourceEditMode::Overwrite);
        }
        tc->setTargetTrack(TrackType::Audio, 0);
        tc->setTargetTrack(TrackType::Video, 0);
        ctx.selection->clear();
    }
    // A demo mix (one undo step; the project is marked clean afterwards).
    ctx.project->beginEdit(QStringLiteral("Demo Mix"));
    TimelineData& d = timeline->mutableData();
    if (d.audioTracks.size() >= 2) {
        TrackData& a1 = d.audioTracks[0];
        a1.mix.strip.eq.enabled = true;
        a1.mix.strip.eq.bands[0].enabled = true; // 80 Hz high-pass
        a1.mix.strip.eq.bands[2].gainDb = 4.5;
        a1.mix.strip.eq.bands[2].frequencyHz = 350.0;
        a1.mix.strip.eq.bands[3].gainDb = -6.0;
        a1.mix.strip.eq.bands[3].frequencyHz = 2500.0;
        a1.mix.strip.eq.bands[3].q = 2.0;
        a1.mix.strip.eq.bands[4].gainDb = 3.0;
        a1.mix.strip.dynamics.compressorEnabled = true;
        a1.mix.strip.dynamics.thresholdDb = -24.0;
        a1.mix.strip.dynamics.ratio = 3.0;
        const int64_t length = std::max<int64_t>(a1.end(), 50);
        (void)mixerops::writeAutomation(d, a1.id, mixkeys::kVolumeDb, 0, -2.0, 0);
        (void)mixerops::writeAutomation(d, a1.id, mixkeys::kVolumeDb, length / 3, -8.0, length / 3);
        (void)mixerops::writeAutomation(
            d, a1.id, mixkeys::kVolumeDb, length * 2 / 3, -3.0, length * 2 / 3);
        TrackData& a2 = d.audioTracks[1];
        a2.mix.strip.pan = 0.35;
        a2.mix.strip.volumeDb = -6.0;
        const int dialogue
            = mixerops::addBus(d, true, QCoreApplication::translate("kk::Screenshot", "Dialogue"));
        (void)mixerops::setTrackRoute(d, a2.id, dialogue, true);
        mixerops::bus(d, kMainBusId)->strip.dynamics.limiterEnabled = true;
    }
    ctx.project->endEdit();
    ctx.project->undoStack()->setClean(); // demo edits: closing must not ask
    timeline->setPlayhead(std::max<int64_t>(0, timeline->data().duration() / 2));

    // Offline meter reading of the mix over the seconds before the playhead.
    {
        const TimelineData& data = timeline->data();
        AudioMixer mixer;
        mixer.setMediaPaths(makeMediaPathMap(ctx.project->mediaPool()->state()));
        MixLevels levels;
        mixer.setLevels(&levels);
        dsp::LoudnessMeter loudness;
        const int64_t end = framesToSamples48k(timeline->playhead(), data.fps);
        const int64_t start
            = std::max<int64_t>(0, end - int64_t(kMeterSeconds) * AudioMixer::kSampleRate);
        constexpr int kBlock = 4800;
        std::vector<float> buffer(size_t(kBlock) * 2);
        MeterFrame frame;
        for (int64_t s = start; s < end; s += kBlock) {
            const int n = int(std::min<int64_t>(kBlock, end - s));
            mixer.mix(data, s, n, buffer.data());
            loudness.process(buffer.data(), n);
            if (end - s <= 2 * kBlock) // the last 200 ms: the reading on screen
                frame.levels.merge(levels);
        }
        frame.sample = start;
        frame.momentaryLufs = loudness.momentary();
        frame.shortTermLufs = loudness.shortTerm();
        frame.integratedLufs = loudness.integrated();
        frame.truePeakDb = loudness.truePeakRecent();
        frame.truePeakMaxDb = loudness.truePeakMax();
        page->meterModel()->setFrame(frame);
    }
    TimelineWidget* w = page->timeline();
    QTimer::singleShot(0, w, [w] { w->zoomToFit(); });

    if (timeline->data().audioTracks.empty())
        return nullptr;
    MixerStrip* strip = page->mixer()->strip(MixTarget::track(timeline->data().audioTracks[0].id));
    if (!strip)
        return nullptr;
    QWidget* tool = nullptr;
    if (section == QLatin1String("eq")) {
        EqWindow* eq = strip->openEq();
        eq->curve()->setSelectedBand(3);
        tool = eq;
    } else if (section == QLatin1String("dynamics")) {
        tool = strip->openDynamics();
    }
    if (tool) {
        // Over the timeline, left of the mixer.
        const QPoint at = window.mapToGlobal(QPoint(window.width() / 5, window.height() / 3));
        tool->move(at);
    }
    return tool;
}

QPixmap grabWithToolWindow(QWidget& window, QWidget* tool)
{
    QPixmap shot = window.grab();
    if (!tool || !tool->isVisible())
        return shot;
    QPainter p(&shot);
    const QPoint at = window.mapFromGlobal(tool->mapToGlobal(QPoint(0, 0)));
    p.drawPixmap(at, tool->grab());
    return shot;
}

} // namespace kk
