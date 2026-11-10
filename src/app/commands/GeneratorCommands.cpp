// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: generator.add, title.add, title.set. A generator, solid, adjustment or title clip on a
// video track at a time. The clip comes from effectactions::makeGeneratorClip() and is placed by
// effectactions::addGeneratorClip(), the functions behind the Effects library, so a default
// looks the same as when a person double-clicks the entry.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "app/commands/ParamSupport.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/TitleLayers.h"
#include "core/TitlePresets.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

namespace ops = editops;

namespace {

/// The basic text options of a title and the parameter key each one sets.
struct TitleOption
{
    const char* option;
    const QString* key;
    ParamType type;
    const char* description;
};

const TitleOption kTitleOptions[] = {
    {"text", &keys::Text, ParamType::String, "the text; a new line makes a second line"},
    {"font", &keys::Font, ParamType::String, "font family, e.g. \"Sans Serif\""},
    {"size", &keys::Size, ParamType::Number, "font size in pixels at the timeline's picture size"},
    {"color", &keys::Color, ParamType::String, "text colour: #rrggbb or a colour name"},
    {"bold", &keys::Bold, ParamType::Boolean, "bold text"},
    {"italic", &keys::Italic, ParamType::Boolean, "italic text"},
    {"align", &keys::Align, ParamType::String, "alignment of the lines: left, center or right"},
    {"text-x", &keys::TextPosX, ParamType::Number,
        "position of the text block, 0 (left) to 1 (right)"},
    {"text-y", &keys::TextPosY, ParamType::Number,
        "position of the text block, 0 (bottom) to 1 (top)"},
};

QVector<ParamSpec> titleParams()
{
    QVector<ParamSpec> params;
    for (const TitleOption& o : kTitleOptions)
        params.append({QString::fromLatin1(o.option), o.type, QString::fromLatin1(o.description),
            false, {}, {}});
    params.append(setListParam("title"));
    return params;
}

/// "key=value" entries for the title options in @p args, then the `set` entries.
QStringList titleSettings(const CommandArgs& args)
{
    QStringList out;
    for (const TitleOption& o : kTitleOptions) {
        const QString name = QString::fromLatin1(o.option);
        if (!args.has(name))
            continue;
        // The registry keeps a Number as a number and a Boolean as a bool; the checker wants text.
        const QString value = o.type == ParamType::Boolean
            ? (args.boolean(name) ? QStringLiteral("true") : QStringLiteral("false"))
            : o.type == ParamType::Number ? QString::number(args.number(name), 'g', 15)
                                          : args.string(name);
        out.append(*o.key + QLatin1Char('=') + value);
    }
    out.append(args.strings(QStringLiteral("set")));
    return out;
}

/// Where the parameters of @p clip live: the solid colour of a Solid clip, else ClipData::title.
ParamRef baseRef(const ClipData& clip)
{
    return {
        clip.id, clip.kind == ClipKind::Solid ? ParamScope::SolidColor : ParamScope::Title, 0, {}};
}

/// The video track named @p name ("V2") as an index, -1 for none given; failure when it is not a
/// video track of the timeline.
std::optional<CommandResult> videoTrackIndex(
    const TimelineData& d, const CommandArgs& args, int& index)
{
    index = -1;
    if (!args.has(QStringLiteral("track")))
        return std::nullopt;
    const auto ref = parseTrack(d, args.string(QStringLiteral("track")));
    if (!ref || ref->type != TrackType::Video)
        return fail("unknown_track",
            QStringLiteral("\"%1\" is not a video track of this timeline (V1 ... V%2)")
                .arg(args.string(QStringLiteral("track")))
                .arg(d.videoTracks.size()));
    index = ref->index;
    return std::nullopt;
}

CommandResult place(CommandContext& ctx, const CommandArgs& args, const EffectDescriptor& d,
    const QStringList& entries)
{
    Project& project = *ctx.project;
    const TimelineData& data = project.timeline()->data();
    std::optional<int64_t> at;
    std::optional<int64_t> duration;
    if (auto bad = readTime(args, QStringLiteral("at"), data.fps, at))
        return *bad;
    if (auto bad = readTime(args, QStringLiteral("duration"), data.fps, duration))
        return *bad;
    if (at && *at < 0)
        return fail("out_of_range", QStringLiteral("the start cannot be negative"));
    if (duration && *duration < 1)
        return fail("out_of_range", QStringLiteral("the duration must be at least one frame"));
    int track = -1;
    if (auto bad = videoTrackIndex(data, args, track))
        return *bad;
    if (track >= 0 && data.videoTracks[size_t(track)].locked)
        return fail("track_locked",
            QStringLiteral("track %1 is locked").arg(trackName({TrackType::Video, track})));

    // Check the parameters on the clip the library would make, before anything is edited.
    const std::optional<ClipData> probe
        = effectactions::makeGeneratorClip(d.id, data.fps, QSizeF(data.width, data.height));
    if (!probe)
        return fail("unknown_effect", QStringLiteral("%1 cannot be placed").arg(d.id));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(d, entries, settings))
        return *bad;

    const quint64 id = effectactions::addGeneratorClip(
        project, d.id, at.value_or(0), duration.value_or(-1), track, [&](ClipData& clip) {
            applySettings(clip, baseRef(clip), settings);
            if (clip.kind == ClipKind::Title && args.has(QStringLiteral("text"))
                && d.id == titles::Text)
                clip.name = args.string(QStringLiteral("text")).section(QLatin1Char('\n'), 0, 0);
        });
    if (id == 0)
        return fail(
            "edit_refused", QStringLiteral("the clip was not placed (track missing or locked)"));
    const TimelineData& after = project.timeline()->data();
    const auto loc = ops::findClip(after, id);
    return CommandResult::success(
        {{QStringLiteral("clip"), clipToJson(after, loc->track, *ops::clipById(after, id))}},
        QStringLiteral("added %1 as clip %2 on %3").arg(d.name).arg(id).arg(trackName(loc->track)));
}

CommandResult runGeneratorAdd(CommandContext& ctx, const CommandArgs& args)
{
    const QString id = args.string(QStringLiteral("generator"));
    const EffectDescriptor* d = EffectRegistry::instance().find(id);
    if (d && d->category == EffectCategory::Title)
        return fail("bad_value", QStringLiteral("%1 is a title: use title.add").arg(id));
    if (!d || (d->category != EffectCategory::Generator && d->category != EffectCategory::Effects))
        return fail("unknown_effect",
            QStringLiteral("\"%1\" is not a generator (see effect.list --category generator)")
                .arg(id));
    return place(ctx, args, *d, args.strings(QStringLiteral("set")));
}

CommandResult runTitleAdd(CommandContext& ctx, const CommandArgs& args)
{
    const QString id = args.string(QStringLiteral("preset"), titles::Text);
    const EffectDescriptor* d = EffectRegistry::instance().find(id);
    if (!d || d->category != EffectCategory::Title)
        return fail("unknown_effect",
            QStringLiteral("\"%1\" is not a title (see effect.list --category title)").arg(id));
    return place(ctx, args, *d, titleSettings(args));
}

CommandResult runTitleSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& data = project.timeline()->data();
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(data, id, found))
        return *bad;
    if (found.clip->kind != ClipKind::Title)
        return fail("bad_value", QStringLiteral("clip %1 is not a title clip").arg(id));
    if (data.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    const QStringList entries = titleSettings(args);
    if (entries.isEmpty())
        return fail(
            "missing_parameter", QStringLiteral("nothing to set: give at least one option"));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*titles::descriptorOf(*found.clip), entries, settings))
        return *bad;
    timelineedit::runEdit(&project, QStringLiteral("Set Title"), [&](TimelineData& d) {
        ClipData* clip = ops::clipById(d, id);
        applySettings(*clip, baseRef(*clip), settings);
        if (args.has(QStringLiteral("text")) && clip->generator.isEmpty())
            clip->name = args.string(QStringLiteral("text")).section(QLatin1Char('\n'), 0, 0);
        return true;
    });
    const TimelineData& after = project.timeline()->data();
    return CommandResult::success(
        {{QStringLiteral("clip"), clipToJson(after, found.track, *ops::clipById(after, id))}},
        QStringLiteral("updated title clip %1").arg(id));
}

// ---- Title layers (spec section 68) ----

namespace tl = titlelayers;

QJsonObject layerJson(const ClipData& clip, quint64 id, int index)
{
    const bool shape = tl::kind(clip, id) == TitleLayerKind::Shape;
    QJsonObject o {{QStringLiteral("layer"), qint64(id)}, {QStringLiteral("index"), index},
        {QStringLiteral("kind"), shape ? QStringLiteral("shape") : QStringLiteral("text")},
        {QStringLiteral("name"), tl::displayName(clip, id)},
        {QStringLiteral("visible"), tl::visible(clip, id)}};
    if (const ParamSet* p = tl::params(clip, id)) {
        if (shape)
            o.insert(QStringLiteral("shape"),
                p->staticValue(keys::ShapeType, QStringLiteral("rect")).toString());
        else
            o.insert(QStringLiteral("text"), p->staticValue(keys::Text).toString());
    }
    return o;
}

QJsonArray layersJson(const ClipData& clip)
{
    QJsonArray list;
    int index = 0;
    for (const quint64 id : tl::order(clip))
        list.append(layerJson(clip, id, index++));
    return list;
}

/// The title clip `clip` of the command, unlocked; failure otherwise.
std::optional<CommandResult> titleClipOrFail(
    const TimelineData& data, const CommandArgs& args, FoundClip& found, bool editing)
{
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    if (auto bad = findClipOrFail(data, id, found))
        return bad;
    if (found.clip->kind != ClipKind::Title)
        return fail("bad_value", QStringLiteral("clip %1 is not a title clip").arg(id));
    if (editing && data.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    return std::nullopt;
}

std::optional<CommandResult> layerOrFail(
    const ClipData& clip, const CommandArgs& args, const char* name, quint64& id)
{
    id = quint64(args.integer(QString::fromLatin1(name)));
    if (!tl::exists(clip, id))
        return fail("unknown_layer",
            QStringLiteral("clip %1 has no title layer %2 (see title.layer_list)")
                .arg(clip.id)
                .arg(id));
    return std::nullopt;
}

const ParamSpec kLayerClip {QStringLiteral("clip"), ParamType::Integer,
    QStringLiteral("title clip id (see clip.list)"), true, {}, {}};
const ParamSpec kLayerId {QStringLiteral("layer"), ParamType::Integer,
    QStringLiteral("layer id (see title.layer_list); 0 is the first layer"), true, {}, {}};

CommandResult layerDone(
    Project& project, quint64 clipId, const QString& message, const QJsonObject& extra = {})
{
    const TimelineData& after = project.timeline()->data();
    const auto loc = ops::findClip(after, clipId);
    QJsonObject data = extra;
    data.insert(QStringLiteral("layers"), layersJson(*ops::clipById(after, clipId)));
    data.insert(
        QStringLiteral("clip"), clipToJson(after, loc->track, *ops::clipById(after, clipId)));
    return CommandResult::success(data, message);
}

CommandResult runLayerList(CommandContext& ctx, const CommandArgs& args)
{
    FoundClip found;
    if (auto bad = titleClipOrFail(ctx.project->timeline()->data(), args, found, false))
        return *bad;
    const QJsonArray list = layersJson(*found.clip);
    return CommandResult::success({{QStringLiteral("layers"), list}},
        QStringLiteral("%1 layer(s) on clip %2, bottom to top")
            .arg(list.size())
            .arg(found.clip->id));
}

CommandResult runLayerAdd(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    if (auto bad = titleClipOrFail(project.timeline()->data(), args, found, true))
        return *bad;
    const quint64 clipId = found.clip->id;
    const QString kind = args.string(QStringLiteral("kind"), QStringLiteral("text"));
    const QString shape = args.string(QStringLiteral("shape"));
    quint64 copyOf = 0;
    const bool duplicate = args.has(QStringLiteral("duplicate"));
    if (duplicate) {
        if (auto bad = layerOrFail(*found.clip, args, "duplicate", copyOf))
            return *bad;
    }
    // Check the settings on the layer the new one will be, before anything is edited.
    const bool isShape = duplicate ? tl::kind(*found.clip, copyOf) == TitleLayerKind::Shape
                                   : kind == QLatin1String("shape");
    const EffectDescriptor* d = isShape
        ? EffectRegistry::instance().find(QLatin1String(tl::kShapeDescriptorId))
        : titles::descriptorOf(*found.clip);
    QStringList entries = args.strings(QStringLiteral("set"));
    if (args.has(QStringLiteral("text")))
        entries.prepend(keys::Text + QLatin1Char('=') + args.string(QStringLiteral("text")));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*d, entries, settings))
        return *bad;
    quint64 created = 0;
    timelineedit::runEdit(&project, QStringLiteral("Add Title Layer"), [&](TimelineData& data) {
        ClipData* clip = ops::clipById(data, clipId);
        created = duplicate
            ? tl::duplicateLayer(*clip, copyOf)
            : tl::addLayer(*clip, isShape ? TitleLayerKind::Shape : TitleLayerKind::Text, shape);
        if (created != 0 && !settings.isEmpty())
            applySettings(*clip, {clipId, ParamScope::TitleLayer, created, {}}, settings);
        if (created != 0 && args.has(QStringLiteral("name")))
            tl::rename(*clip, created, args.string(QStringLiteral("name")));
        return created != 0;
    });
    if (created == 0)
        return fail("edit_refused", QStringLiteral("the layer was not added"));
    return layerDone(project, clipId,
        QStringLiteral("added layer %1 to clip %2").arg(created).arg(clipId),
        {{QStringLiteral("layer"), qint64(created)}});
}

CommandResult runLayerRemove(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    if (auto bad = titleClipOrFail(project.timeline()->data(), args, found, true))
        return *bad;
    quint64 id = 0;
    if (auto bad = layerOrFail(*found.clip, args, "layer", id))
        return *bad;
    if (id == tl::kBaseId)
        return fail("bad_value",
            QStringLiteral("layer 0 is the title's first layer and cannot be removed; hide it "
                           "with title.layer_set --visible false"));
    const quint64 clipId = found.clip->id;
    timelineedit::runEdit(&project, QStringLiteral("Delete Title Layer"),
        [&](TimelineData& data) { return tl::removeLayer(*ops::clipById(data, clipId), id); });
    return layerDone(
        project, clipId, QStringLiteral("removed layer %1 of clip %2").arg(id).arg(clipId));
}

CommandResult runLayerMove(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    if (auto bad = titleClipOrFail(project.timeline()->data(), args, found, true))
        return *bad;
    quint64 id = 0;
    if (auto bad = layerOrFail(*found.clip, args, "layer", id))
        return *bad;
    const int by = int(args.integer(QStringLiteral("by")));
    if (by == 0)
        return fail("out_of_range", QStringLiteral("\"by\" cannot be 0 (positive moves up)"));
    const quint64 clipId = found.clip->id;
    bool moved = false;
    timelineedit::runEdit(&project, QStringLiteral("Move Title Layer"), [&](TimelineData& data) {
        moved = tl::moveLayer(*ops::clipById(data, clipId), id, by);
        return moved;
    });
    if (!moved)
        return fail(
            "out_of_range", QStringLiteral("layer %1 is already at the end of the stack").arg(id));
    return layerDone(
        project, clipId, QStringLiteral("moved layer %1 of clip %2").arg(id).arg(clipId));
}

CommandResult runLayerSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    if (auto bad = titleClipOrFail(project.timeline()->data(), args, found, true))
        return *bad;
    quint64 id = 0;
    if (auto bad = layerOrFail(*found.clip, args, "layer", id))
        return *bad;
    QStringList entries = args.strings(QStringLiteral("set"));
    for (const char* option : {"text", "font", "size", "color"}) {
        const QString name = QString::fromLatin1(option);
        if (!args.has(name))
            continue;
        const QString key = name == QLatin1String("text") ? keys::Text
            : name == QLatin1String("font")               ? keys::Font
            : name == QLatin1String("size")               ? keys::Size
                                                          : keys::Color;
        const QString value = name == QLatin1String("size")
            ? QString::number(args.number(name), 'g', 15)
            : args.string(name);
        entries.append(key + QLatin1Char('=') + value);
    }
    const bool hasVisible = args.has(QStringLiteral("visible"));
    const bool hasName = args.has(QStringLiteral("name"));
    if (entries.isEmpty() && !hasVisible && !hasName)
        return fail("missing_parameter",
            QStringLiteral("nothing to set: give name, visible, text, font, size, color or set"));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*layerDescriptor(*found.clip, id), entries, settings))
        return *bad;
    const quint64 clipId = found.clip->id;
    const ParamScope scope = id == tl::kBaseId ? ParamScope::Title : ParamScope::TitleLayer;
    timelineedit::runEdit(&project, QStringLiteral("Set Title Layer"), [&](TimelineData& data) {
        ClipData* clip = ops::clipById(data, clipId);
        if (hasVisible)
            tl::setVisible(*clip, id, args.boolean(QStringLiteral("visible")));
        if (hasName)
            tl::rename(*clip, id, args.string(QStringLiteral("name")));
        applySettings(*clip, {clipId, scope, id, {}}, settings);
        return true;
    });
    return layerDone(
        project, clipId, QStringLiteral("updated layer %1 of clip %2").arg(id).arg(clipId));
}

QVector<ParamSpec> placementParams()
{
    return {{QStringLiteral("at"), ParamType::Time, QStringLiteral("first frame on the timeline"),
                false, QStringLiteral("0"), {}},
        {QStringLiteral("duration"), ParamType::Time,
            QStringLiteral("length in frames or timecode"), false,
            QStringLiteral("Preferences > Editing > generator duration"), {}},
        {QStringLiteral("track"), ParamType::String,
            QStringLiteral("video track (V1, V2, ...); anything under the clip is overwritten. "
                           "Default: the lowest track above everything in that time range"),
            false, {}, {}}};
}

} // namespace

void registerGeneratorCommands(CommandRegistry& registry)
{
    QVector<ParamSpec> generator {{QStringLiteral("generator"), ParamType::String,
        QStringLiteral("generator id (see effect.list --category generator), e.g. solid_color; "
                       "adjustment_clip makes an adjustment clip"),
        true, {}, {}}};
    generator += placementParams();
    generator.append(setListParam("generator"));
    registry.add({{QStringLiteral("generator.add"),
                      QStringLiteral("Add a generated clip (solid colour, gradient, adjustment "
                                     "clip, ...) to a video track at a time, with parameters."),
                      generator, ProjectUse::Modify, true},
        runGeneratorAdd});

    QVector<ParamSpec> title {{QStringLiteral("preset"), ParamType::String,
        QStringLiteral("title id (see effect.list --category title)"), false,
        QStringLiteral("text_title"), {}}};
    title += placementParams();
    title += titleParams();
    registry.add({{QStringLiteral("title.add"),
                      QStringLiteral("Add a text title clip to a video track at a time, with its "
                                     "text and basic text settings."),
                      title, ProjectUse::Modify, true},
        runTitleAdd});

    QVector<ParamSpec> set {{QStringLiteral("clip"), ParamType::Integer,
        QStringLiteral("title clip id (see clip.list)"), true, {}, {}}};
    set += titleParams();
    registry.add({{QStringLiteral("title.set"),
                      QStringLiteral("Change the text and basic text settings of a title clip."),
                      set, ProjectUse::Modify, true},
        runTitleSet});

    registry.add({{QStringLiteral("title.layer_list"),
                      QStringLiteral("List the layers of a title clip, bottom to top."),
                      {kLayerClip}, ProjectUse::Read, true},
        runLayerList});
    registry.add(
        {{QStringLiteral("title.layer_add"),
             QStringLiteral("Add a layer on top of a title clip: a text layer, a shape "
                            "layer, or a copy of a layer (duplicate)."),
             {kLayerClip,
                 {QStringLiteral("kind"), ParamType::String, QStringLiteral("layer kind"), false,
                     QStringLiteral("text"), {QStringLiteral("text"), QStringLiteral("shape")}},
                 {QStringLiteral("shape"), ParamType::String,
                     QStringLiteral("shape layers: rect, rounded, ellipse or line"), false,
                     QStringLiteral("rect"),
                     {QStringLiteral("rect"), QStringLiteral("rounded"), QStringLiteral("ellipse"),
                         QStringLiteral("line")}},
                 {QStringLiteral("duplicate"), ParamType::Integer,
                     QStringLiteral("copy this layer (id) instead of adding a new one; "
                                    "the copy sits directly above it"),
                     false, {}, {}},
                 {QStringLiteral("name"), ParamType::String,
                     QStringLiteral("name in the layer list"), false, {}, {}},
                 {QStringLiteral("text"), ParamType::String,
                     QStringLiteral("text layers: the text"), false, {}, {}},
                 setListParam("layer")},
             ProjectUse::Modify, true},
            runLayerAdd});
    registry.add({{QStringLiteral("title.layer_remove"),
                      QStringLiteral("Delete a layer of a title clip (not the first one)."),
                      {kLayerClip, kLayerId}, ProjectUse::Modify, true},
        runLayerRemove});
    registry.add({{QStringLiteral("title.layer_move"),
                      QStringLiteral("Move a layer up (positive) or down (negative) the stack."),
                      {kLayerClip, kLayerId,
                          {QStringLiteral("by"), ParamType::Integer,
                              QStringLiteral("places to move: 1 up, -1 down"), true, {}, {}}},
                      ProjectUse::Modify, true},
        runLayerMove});
    registry.add({{QStringLiteral("title.layer_set"),
                      QStringLiteral("Change a layer of a title clip: its name, whether it is "
                                     "shown, and its parameters."),
                      {kLayerClip, kLayerId,
                          {QStringLiteral("name"), ParamType::String,
                              QStringLiteral("name in the layer list"), false, {}, {}},
                          {QStringLiteral("visible"), ParamType::Boolean,
                              QStringLiteral("show or hide the layer"), false, {}, {}},
                          {QStringLiteral("text"), ParamType::String, QStringLiteral("the text"),
                              false, {}, {}},
                          {QStringLiteral("font"), ParamType::String, QStringLiteral("font family"),
                              false, {}, {}},
                          {QStringLiteral("size"), ParamType::Number,
                              QStringLiteral("font size in pixels"), false, {}, {}},
                          {QStringLiteral("color"), ParamType::String,
                              QStringLiteral("text or fill colour: #rrggbb or a colour name"),
                              false, {}, {}},
                          setListParam("layer")},
                      ProjectUse::Modify, true},
        runLayerSet});
}

} // namespace kk::commands
