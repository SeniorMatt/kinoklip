// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: frame.export (a timeline frame to a PNG file) and frame.view (the same picture handed
// back to the caller, for a language model that looks at its work). Both render through the same
// compositor as --render-frame, the viewer and the export.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/RenderFrameCommand.h"
#include "ui/common/TimecodeFormat.h"

#include <QFileInfo>

#include <cmath>

namespace kk::commands {

namespace {

constexpr int kMinWidth = 16; ///< smallest picture asked for
constexpr int kMaxWidth = 8192; ///< largest picture asked for
constexpr int kViewWidth = 1024; ///< frame.view default: keeps the picture small for a model

const ParamSpec kAtParam {QStringLiteral("at"), ParamType::Time,
    QStringLiteral("timeline frame to render, counted from the start of the timeline"), false,
    QStringLiteral("0"), {}};

/// Renders the requested frame; @p width 0 = timeline size. Sets @p image or returns a failure.
std::optional<CommandResult> renderRequested(CommandContext& ctx, const CommandArgs& args,
    int width, bool neverUpscale, QImage& image, int64_t& frame)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    if (d.duration() == 0)
        return fail("empty_timeline", QStringLiteral("the timeline has no clips to show"));
    std::optional<int64_t> at;
    if (auto bad = readTime(args, QStringLiteral("at"), d.fps, at))
        return bad;
    frame = at.value_or(0);
    if (frame < 0 || frame >= d.duration())
        return fail("out_of_range",
            QStringLiteral("frame %1 is outside the timeline (0 to %2)")
                .arg(frame)
                .arg(d.duration() - 1));
    QSize size; // empty = the timeline's own size
    if (width > 0 && d.width > 0 && d.height > 0 && !(neverUpscale && width >= d.width))
        size = QSize(width, std::max(1, int(std::lround(double(width) * d.height / d.width))));
    QString error;
    image = renderFrameImage(project, frame, size, &error);
    if (image.isNull())
        return fail("render_failed", error);
    return std::nullopt;
}

std::optional<CommandResult> checkWidth(const CommandArgs& args, int* width)
{
    if (!args.has(QStringLiteral("width")))
        return std::nullopt;
    *width = int(args.integer(QStringLiteral("width")));
    if (*width < kMinWidth || *width > kMaxWidth)
        return fail("out_of_range",
            QStringLiteral("width must be between %1 and %2").arg(kMinWidth).arg(kMaxWidth));
    return std::nullopt;
}

QJsonObject frameInfo(const TimelineData& d, int64_t frame, const QImage& image)
{
    return {{QStringLiteral("frame"), qint64(frame)},
        {QStringLiteral("timecode"), timecode::format(frame, d.fps)},
        {QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()}};
}

CommandResult runExport(CommandContext& ctx, const CommandArgs& args)
{
    int width = 0;
    if (auto bad = checkWidth(args, &width))
        return *bad;
    const QString path = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    if (!path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
        return fail("bad_value", QStringLiteral("the file name must end in .png"));
    QImage image;
    int64_t frame = 0;
    if (auto bad = renderRequested(ctx, args, width, false, image, frame))
        return *bad;
    if (!image.convertedTo(QImage::Format_ARGB32).save(path, "PNG"))
        return fail("write_failed", QStringLiteral("cannot write %1").arg(path));
    QJsonObject data = frameInfo(ctx.project->timeline()->data(), frame, image);
    data.insert(QStringLiteral("file"), path);
    return CommandResult::success(std::move(data),
        QStringLiteral("wrote frame %1 as %2 (%3x%4)")
            .arg(frame)
            .arg(path)
            .arg(image.width())
            .arg(image.height()));
}

CommandResult runView(CommandContext& ctx, const CommandArgs& args)
{
    int width = kViewWidth;
    if (auto bad = checkWidth(args, &width))
        return *bad;
    QImage image;
    int64_t frame = 0;
    if (auto bad = renderRequested(ctx, args, width, true, image, frame))
        return *bad;
    CommandResult result
        = CommandResult::success(frameInfo(ctx.project->timeline()->data(), frame, image),
            QStringLiteral("frame %1 (%2x%3)").arg(frame).arg(image.width()).arg(image.height()));
    result.image = image.convertedTo(QImage::Format_ARGB32);
    return result;
}

} // namespace

void registerFrameCommands(CommandRegistry& registry)
{
    registry.add(
        {{QStringLiteral("frame.export"),
             QStringLiteral("Render one frame of the timeline to a PNG file, as the viewer and "
                            "the export show it. The project is not changed."),
             {kAtParam,
                 {QStringLiteral("file"), ParamType::Path, QStringLiteral("PNG file to write"),
                     true, {}, {}},
                 {QStringLiteral("width"), ParamType::Integer,
                     QStringLiteral("picture width in pixels, 16 to 8192; the height keeps the "
                                    "aspect ratio"),
                     false, QStringLiteral("the timeline's width"), {}}},
             ProjectUse::Read, true},
            runExport});
    registry.add({{QStringLiteral("frame.view"),
                      QStringLiteral(
                          "Look at one frame of the timeline: returns the picture itself "
                          "(MCP clients get it as an image; the command line only prints its "
                          "size, use frame export to get a file). Never wider than the timeline."),
                      {kAtParam,
                          {QStringLiteral("width"), ParamType::Integer,
                              QStringLiteral("picture width in pixels, 16 to 8192"), false,
                              QString::number(kViewWidth), {}}},
                      ProjectUse::Read, true},
        runView});
}

} // namespace kk::commands
