// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: marker.add, marker.list, marker.delete (timeline markers).
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/ClipColors.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <algorithm>

namespace kk::commands {

using timelineedit::runEdit;

namespace {

const TimelineData& dataOf(CommandContext& ctx)
{
    return ctx.project->timeline()->data();
}

QJsonObject markerToJson(const Marker& m, Rational fps)
{
    return {{QStringLiteral("frame"), qint64(m.frame)},
        {QStringLiteral("timecode"), timecode::format(m.frame, fps)},
        {QStringLiteral("name"), m.name}, {QStringLiteral("note"), m.note},
        {QStringLiteral("color"), QString::fromLatin1(nearestMarkerColor(m.color).id)},
        {QStringLiteral("duration"), qint64(m.duration)}};
}

CommandResult runAdd(CommandContext& ctx, const CommandArgs& args)
{
    const Rational fps = dataOf(ctx).fps;
    std::optional<int64_t> at;
    std::optional<int64_t> duration;
    if (auto bad = readTime(args, QStringLiteral("at"), fps, at))
        return *bad;
    if (auto bad = readTime(args, QStringLiteral("duration"), fps, duration))
        return *bad;
    if (*at < 0)
        return fail(
            "out_of_range", QStringLiteral("the position is before the start of the timeline"));
    if (duration && *duration < 1)
        return fail("out_of_range", QStringLiteral("the duration must be at least one frame"));
    Marker marker;
    marker.frame = *at;
    marker.name = args.string(QStringLiteral("name"));
    marker.note = args.string(QStringLiteral("note"));
    marker.duration = duration.value_or(1);
    const QString color = args.string(QStringLiteral("color"), QStringLiteral("blue"));
    for (const PaletteColor& c : markerColorPalette()) {
        if (color == QLatin1String(c.id))
            marker.color = QColor(c.rgb);
    }
    if (!marker.color.isValid())
        return fail("bad_value", QStringLiteral("unknown marker colour \"%1\"").arg(color));
    for (const Marker& m : dataOf(ctx).markers) {
        if (m.frame == marker.frame)
            return fail("marker_exists",
                QStringLiteral("there is already a marker at frame %1").arg(marker.frame));
    }
    runEdit(ctx.project, QStringLiteral("Add Marker"), [&](TimelineData& data) {
        // Markers stay sorted by frame.
        const auto it = std::lower_bound(data.markers.begin(), data.markers.end(), marker.frame,
            [](const Marker& m, int64_t f) { return m.frame < f; });
        data.markers.insert(it, marker);
        return true;
    });
    return CommandResult::success({{QStringLiteral("marker"), markerToJson(marker, fps)}},
        QStringLiteral("added marker at %1").arg(timecode::format(marker.frame, fps)));
}

CommandResult runList(CommandContext& ctx, const CommandArgs&)
{
    const TimelineData& d = dataOf(ctx);
    QJsonArray list;
    QString text;
    for (const Marker& m : d.markers) {
        list.append(markerToJson(m, d.fps));
        text += QStringLiteral("%1  %2  %3\n")
                    .arg(timecode::format(m.frame, d.fps),
                        QString::fromLatin1(nearestMarkerColor(m.color).id), m.name);
    }
    return CommandResult::success({{QStringLiteral("markers"), list}},
        text.isEmpty() ? QStringLiteral("no markers") : text.trimmed());
}

CommandResult runDelete(CommandContext& ctx, const CommandArgs& args)
{
    const Rational fps = dataOf(ctx).fps;
    std::optional<int64_t> at;
    if (auto bad = readTime(args, QStringLiteral("at"), fps, at))
        return *bad;
    bool removed = false;
    runEdit(ctx.project, QStringLiteral("Delete Marker"), [&](TimelineData& data) {
        removed = data.markers.removeIf([&](const Marker& m) { return m.frame == *at; }) > 0;
        return removed;
    });
    if (!removed)
        return fail("unknown_marker", QStringLiteral("no marker at frame %1").arg(*at));
    return CommandResult::success({{QStringLiteral("frame"), qint64(*at)}},
        QStringLiteral("deleted the marker at %1").arg(timecode::format(*at, fps)));
}

} // namespace

void registerMarkerCommands(CommandRegistry& registry)
{
    QStringList colors;
    for (const PaletteColor& c : markerColorPalette())
        colors.append(QString::fromLatin1(c.id));
    registry.add(
        {{QStringLiteral("marker.add"),
             QStringLiteral("Add a timeline marker at a time (frames or timecode)."),
             {{QStringLiteral("at"), ParamType::Time, QStringLiteral("position"), true, {}, {}},
                 {QStringLiteral("name"), ParamType::String, QStringLiteral("marker name"), false,
                     {}, {}},
                 {QStringLiteral("color"), ParamType::String, QStringLiteral("marker colour"),
                     false, QStringLiteral("blue"), colors},
                 {QStringLiteral("note"), ParamType::String, QStringLiteral("free text"), false, {},
                     {}},
                 {QStringLiteral("duration"), ParamType::Time,
                     QStringLiteral("length for a ranged marker"), false, QStringLiteral("1"), {}}},
             ProjectUse::Modify, true},
            runAdd});
    registry.add({{QStringLiteral("marker.list"), QStringLiteral("List the timeline markers."), {},
                      ProjectUse::Read, true},
        runList});
    registry.add(
        {{QStringLiteral("marker.delete"), QStringLiteral("Delete the timeline marker at a time."),
             {{QStringLiteral("at"), ParamType::Time, QStringLiteral("position of the marker"),
                 true, {}, {}}},
             ProjectUse::Modify, true},
            runDelete});
}

} // namespace kk::commands
