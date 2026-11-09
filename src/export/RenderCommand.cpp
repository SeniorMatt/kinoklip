// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCommand — implementation of the --render command-line mode.
#include "export/RenderCommand.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "export/ExportJob.h"
#include "export/ExportPresets.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>

#include <cstdio>

namespace kk {

namespace {

/// Default preset id per output extension.
QString presetForExtension(const QString& suffix, bool pattern)
{
    if (pattern) {
        if (suffix == QLatin1String("png")) {
            return QStringLiteral("png_sequence");
        }
        if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
            return QStringLiteral("jpeg_sequence");
        }
        if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff")) {
            return QStringLiteral("tiff_sequence");
        }
        if (suffix == QLatin1String("exr")) {
            return QStringLiteral("exr_sequence");
        }
        return {};
    }
    static const QHash<QString, QString> table {
        {QStringLiteral("mov"), QStringLiteral("master_prores_hq")},
        {QStringLiteral("mkv"), QStringLiteral("mkv_h264_flac")},
        {QStringLiteral("webm"), QStringLiteral("web_webm")},
        {QStringLiteral("wav"), QStringLiteral("audio_wav")},
        {QStringLiteral("flac"), QStringLiteral("audio_flac")},
        {QStringLiteral("mp3"), QStringLiteral("audio_mp3")},
        {QStringLiteral("m4a"), QStringLiteral("audio_aac")},
        {QStringLiteral("opus"), QStringLiteral("audio_opus")},
        {QStringLiteral("gif"), QStringLiteral("gif")},
        {QStringLiteral("png"), QStringLiteral("frame_png")},
        {QStringLiteral("jpg"), QStringLiteral("frame_jpeg")},
        {QStringLiteral("jpeg"), QStringLiteral("frame_jpeg")},
    };
    return table.value(suffix);
}

QString formatEta(double seconds)
{
    if (seconds < 0) {
        return QStringLiteral("--:--");
    }
    const int s = int(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

bool settingsForOutputFile(const QString& outPath, ExportSettings& settings, QString* error)
{
    const QString suffix = QFileInfo(outPath).suffix().toLower();
    settings = ExportSettings {};
    if (suffix == QLatin1String("mp4") || suffix == QLatin1String("m4v")) {
        settings.container
            = QStringLiteral("mp4"); // defaults: H.264 CRF 20, AAC 320k, timeline size
    } else {
        const QString preset = presetForExtension(suffix, outPath.contains(QLatin1Char('%')));
        ExportPresets presets;
        if (preset.isEmpty() || !presets.apply(preset, settings)) {
            if (error) {
                *error = QStringLiteral("unknown output format \".%1\" (use --preset)").arg(suffix);
            }
            return false;
        }
    }
    settings.outputPath = outPath;
    return true;
}

RenderOutcome renderProject(Project& project, const QString& outPath, const QString& presetId,
    const ExportJob::ProgressCallback& progress)
{
    RenderOutcome outcome;
    ExportSettings settings;
    QString error;
    if (presetId.isEmpty()) {
        if (!settingsForOutputFile(outPath, settings, &error)) {
            outcome.error = error;
            return outcome;
        }
    } else {
        ExportPresets presets;
        if (!presets.apply(presetId, settings)) {
            QStringList ids;
            for (const ExportPreset& p : presets.all())
                ids.append(p.id);
            outcome.error = QStringLiteral("unknown preset \"%1\". Available: %2")
                                .arg(presetId, ids.join(QLatin1Char(' ')));
            return outcome;
        }
        settings.outputPath = outPath; // exactly what was asked for, even with another suffix
    }
    const TimelineData& tl = project.timeline()->data();
    settings.range
        = (tl.markIn >= 0 && tl.markOut > tl.markIn) ? RangeMode::InOut : RangeMode::Entire;

    std::unique_ptr<Compositor> compositor;
    if (settings.hasVideo()) {
        compositor = std::make_unique<Compositor>();
    }
    ExportJob job(
        settings, project.timeline()->snapshot(), makeMediaPathMap(project.mediaPool()->state()));
    job.setCompositor(compositor.get());
    outcome.result = job.run(progress);
    outcome.ok = outcome.result.ok;
    outcome.error = outcome.result.error;
    return outcome;
}

int runRender(const QString& projectPath, const QString& outPath, const QString& presetId)
{
    Project project;
    QString error;
    if (!project.load(projectPath, &error)) {
        std::fprintf(stderr, "render: %s\n", qPrintable(error));
        return 1;
    }
    QElapsedTimer lastPrint;
    lastPrint.start();
    const RenderOutcome outcome = renderProject(
        project, outPath, presetId, [&](const ExportProgress& p) {
            if (lastPrint.elapsed() < 1000 && p.framesDone < p.totalFrames) {
                return;
            }
            lastPrint.restart();
            std::printf("render: %3d%% %lld/%lld frames, %.1f fps, ETA %s\n",
                int(p.fraction() * 100), static_cast<long long>(p.framesDone),
                static_cast<long long>(p.totalFrames), p.fps, qPrintable(formatEta(p.etaSeconds)));
            std::fflush(stdout);
        });
    if (!outcome.ok) {
        std::fprintf(stderr, "render: %s\n", qPrintable(outcome.error));
        return 1;
    }
    const ExportResult& result = outcome.result;
    std::printf("render: %lld frames in %.2f s (%.1f fps), video encoder %s -> %s\n",
        static_cast<long long>(result.frames), result.seconds, result.averageFps(),
        result.videoEncoder.isEmpty() ? "none" : qPrintable(result.videoEncoder),
        qPrintable(result.outputPath));
    return 0;
}

} // namespace kk
