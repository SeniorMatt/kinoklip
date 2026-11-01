// SPDX-License-Identifier: GPL-3.0-or-later
// JSON encoding of timelines, clips, parameter sets and the media pool.
#include "core/Serialization.h"

#include "core/EditOps.h"

#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace cw::serialization {

namespace {

QString colorToString(const QColor& c)
{
    return c.name(QColor::HexArgb);
}

QColor colorFromString(const QString& text)
{
    return QColor::fromString(text);
}

QString kindToString(ClipKind kind)
{
    switch (kind) {
    case ClipKind::Media:
        return QStringLiteral("media");
    case ClipKind::Title:
        return QStringLiteral("title");
    case ClipKind::Solid:
        return QStringLiteral("solid");
    }
    return QStringLiteral("media");
}

ClipKind kindFromString(const QString& text)
{
    if (text == QLatin1String("title"))
        return ClipKind::Title;
    if (text == QLatin1String("solid"))
        return ClipKind::Solid;
    return ClipKind::Media;
}

QJsonObject transitionToJson(const TransitionData& t)
{
    QJsonObject o;
    o[QLatin1String("type")] = t.type;
    o[QLatin1String("duration")] = qint64(t.duration);
    if (t.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(t.params);
    return o;
}

TransitionData transitionFromJson(const QJsonObject& o)
{
    TransitionData t;
    t.type = o.value(QLatin1String("type")).toString(t.type);
    t.duration = o.value(QLatin1String("duration")).toInteger(t.duration);
    t.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
    return t;
}

QJsonObject trackToJson(const TrackData& track)
{
    QJsonObject o;
    o[QLatin1String("id")] = qint64(track.id);
    o[QLatin1String("name")] = track.name;
    o[QLatin1String("enabled")] = track.enabled;
    o[QLatin1String("locked")] = track.locked;
    o[QLatin1String("muted")] = track.muted;
    o[QLatin1String("solo")] = track.solo;
    o[QLatin1String("height")] = track.height;
    QJsonArray clips;
    for (const ClipData& c : track.clips)
        clips.append(clipToJson(c));
    o[QLatin1String("clips")] = clips;
    return o;
}

TrackData trackFromJson(const QJsonObject& o, TrackType type)
{
    TrackData t;
    t.type = type;
    t.id = quint64(o.value(QLatin1String("id")).toInteger());
    t.name = o.value(QLatin1String("name")).toString();
    t.enabled = o.value(QLatin1String("enabled")).toBool(true);
    t.locked = o.value(QLatin1String("locked")).toBool(false);
    t.muted = o.value(QLatin1String("muted")).toBool(false);
    t.solo = o.value(QLatin1String("solo")).toBool(false);
    t.height = o.value(QLatin1String("height")).toInt(0);
    for (const QJsonValue& v : o.value(QLatin1String("clips")).toArray())
        t.clips.push_back(clipFromJson(v.toObject()));
    std::stable_sort(t.clips.begin(), t.clips.end(),
        [](const ClipData& a, const ClipData& b) { return a.start < b.start; });
    return t;
}

} // namespace

// ---- Parameters ----

QJsonValue variantToJson(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return value.toLongLong();
    case QMetaType::Double:
    case QMetaType::Float:
        return value.toDouble();
    case QMetaType::QColor:
        return QJsonObject {{QLatin1String("color"), colorToString(value.value<QColor>())}};
    default:
        return value.toString();
    }
}

QVariant variantFromJson(const QJsonValue& json)
{
    if (json.isObject()) {
        const QJsonObject o = json.toObject();
        if (o.contains(QLatin1String("color")))
            return colorFromString(o.value(QLatin1String("color")).toString());
        return {};
    }
    return json.toVariant();
}

QJsonObject paramSetToJson(const ParamSet& params)
{
    QJsonObject values;
    for (auto it = params.values.cbegin(); it != params.values.cend(); ++it) {
        QJsonObject v;
        v[QLatin1String("base")] = it->base;
        if (it->isAnimated()) {
            QJsonArray keys;
            for (const Keyframe& k : it->keys) {
                keys.append(QJsonObject {{QLatin1String("frame"), qint64(k.frame)},
                    {QLatin1String("value"), k.value},
                    {QLatin1String("interp"), interpToString(k.interp)}});
            }
            v[QLatin1String("keys")] = keys;
        }
        values[it.key()] = v;
    }
    QJsonObject statics;
    for (auto it = params.statics.cbegin(); it != params.statics.cend(); ++it)
        statics[it.key()] = variantToJson(it.value());

    QJsonObject o;
    if (!values.isEmpty())
        o[QLatin1String("values")] = values;
    if (!statics.isEmpty())
        o[QLatin1String("statics")] = statics;
    return o;
}

ParamSet paramSetFromJson(const QJsonObject& json)
{
    ParamSet params;
    const QJsonObject values = json.value(QLatin1String("values")).toObject();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const QJsonObject v = it.value().toObject();
        AnimatedValue value(v.value(QLatin1String("base")).toDouble());
        for (const QJsonValue& kv : v.value(QLatin1String("keys")).toArray()) {
            const QJsonObject k = kv.toObject();
            value.setKey(k.value(QLatin1String("frame")).toInteger(),
                k.value(QLatin1String("value")).toDouble(),
                interpFromString(k.value(QLatin1String("interp")).toString()));
        }
        params.values.insert(it.key(), value);
    }
    const QJsonObject statics = json.value(QLatin1String("statics")).toObject();
    for (auto it = statics.constBegin(); it != statics.constEnd(); ++it) {
        const QVariant v = variantFromJson(it.value());
        if (v.isValid())
            params.statics.insert(it.key(), v);
    }
    return params;
}

// ---- Clips ----

QJsonObject clipToJson(const ClipData& clip)
{
    QJsonObject o;
    o[QLatin1String("id")] = qint64(clip.id);
    o[QLatin1String("kind")] = kindToString(clip.kind);
    if (!clip.mediaId.isEmpty())
        o[QLatin1String("mediaId")] = clip.mediaId;
    o[QLatin1String("name")] = clip.name;
    if (clip.label.isValid())
        o[QLatin1String("label")] = colorToString(clip.label);
    o[QLatin1String("start")] = qint64(clip.start);
    o[QLatin1String("duration")] = qint64(clip.duration);
    o[QLatin1String("sourceInUs")] = qint64(clip.sourceInUs);
    o[QLatin1String("sourceDurationUs")] = qint64(clip.sourceDurationUs);
    o[QLatin1String("speed")] = clip.speed;
    o[QLatin1String("enabled")] = clip.enabled;
    if (clip.linkGroup != 0)
        o[QLatin1String("linkGroup")] = qint64(clip.linkGroup);
    if (clip.transform != ParamSet {})
        o[QLatin1String("transform")] = paramSetToJson(clip.transform);
    if (clip.audio != ParamSet {})
        o[QLatin1String("audio")] = paramSetToJson(clip.audio);
    if (clip.title != ParamSet {})
        o[QLatin1String("title")] = paramSetToJson(clip.title);
    if (clip.solidColor.isValid())
        o[QLatin1String("solidColor")] = colorToString(clip.solidColor);
    if (!clip.effects.isEmpty()) {
        QJsonArray effects;
        for (const EffectInstance& fx : clip.effects) {
            effects.append(QJsonObject {{QLatin1String("id"), qint64(fx.id)},
                {QLatin1String("type"), fx.type}, {QLatin1String("enabled"), fx.enabled},
                {QLatin1String("params"), paramSetToJson(fx.params)}});
        }
        o[QLatin1String("effects")] = effects;
    }
    if (clip.transitionIn)
        o[QLatin1String("transitionIn")] = transitionToJson(*clip.transitionIn);
    if (clip.transitionOut)
        o[QLatin1String("transitionOut")] = transitionToJson(*clip.transitionOut);
    return o;
}

ClipData clipFromJson(const QJsonObject& o)
{
    ClipData c;
    c.id = quint64(o.value(QLatin1String("id")).toInteger());
    c.kind = kindFromString(o.value(QLatin1String("kind")).toString());
    c.mediaId = o.value(QLatin1String("mediaId")).toString();
    c.name = o.value(QLatin1String("name")).toString();
    if (o.contains(QLatin1String("label")))
        c.label = colorFromString(o.value(QLatin1String("label")).toString());
    c.start = o.value(QLatin1String("start")).toInteger();
    c.duration = o.value(QLatin1String("duration")).toInteger();
    c.sourceInUs = o.value(QLatin1String("sourceInUs")).toInteger();
    c.sourceDurationUs = o.value(QLatin1String("sourceDurationUs")).toInteger();
    c.speed = o.value(QLatin1String("speed")).toDouble(1.0);
    c.enabled = o.value(QLatin1String("enabled")).toBool(true);
    c.linkGroup = quint64(o.value(QLatin1String("linkGroup")).toInteger());
    c.transform = paramSetFromJson(o.value(QLatin1String("transform")).toObject());
    c.audio = paramSetFromJson(o.value(QLatin1String("audio")).toObject());
    c.title = paramSetFromJson(o.value(QLatin1String("title")).toObject());
    if (o.contains(QLatin1String("solidColor")))
        c.solidColor = colorFromString(o.value(QLatin1String("solidColor")).toString());
    for (const QJsonValue& v : o.value(QLatin1String("effects")).toArray()) {
        const QJsonObject fo = v.toObject();
        EffectInstance fx;
        fx.id = quint64(fo.value(QLatin1String("id")).toInteger());
        fx.type = fo.value(QLatin1String("type")).toString();
        fx.enabled = fo.value(QLatin1String("enabled")).toBool(true);
        fx.params = paramSetFromJson(fo.value(QLatin1String("params")).toObject());
        c.effects.append(fx);
    }
    if (o.contains(QLatin1String("transitionIn")))
        c.transitionIn = transitionFromJson(o.value(QLatin1String("transitionIn")).toObject());
    if (o.contains(QLatin1String("transitionOut")))
        c.transitionOut = transitionFromJson(o.value(QLatin1String("transitionOut")).toObject());
    return c;
}

// ---- Timeline ----

QJsonObject timelineToJson(const TimelineData& data)
{
    QJsonObject o;
    o[QLatin1String("name")] = data.name;
    o[QLatin1String("fps")]
        = QJsonObject {{QLatin1String("num"), data.fps.num}, {QLatin1String("den"), data.fps.den}};
    o[QLatin1String("width")] = data.width;
    o[QLatin1String("height")] = data.height;
    o[QLatin1String("markIn")] = qint64(data.markIn);
    o[QLatin1String("markOut")] = qint64(data.markOut);
    o[QLatin1String("nextId")] = qint64(data.nextId);
    QJsonArray markers;
    for (const Marker& m : data.markers) {
        markers.append(QJsonObject {{QLatin1String("frame"), qint64(m.frame)},
            {QLatin1String("name"), m.name}, {QLatin1String("color"), colorToString(m.color)}});
    }
    o[QLatin1String("markers")] = markers;
    QJsonArray video;
    for (const TrackData& t : data.videoTracks)
        video.append(trackToJson(t));
    o[QLatin1String("videoTracks")] = video;
    QJsonArray audio;
    for (const TrackData& t : data.audioTracks)
        audio.append(trackToJson(t));
    o[QLatin1String("audioTracks")] = audio;
    return o;
}

bool timelineFromJson(const QJsonObject& o, TimelineData& out, QString* error)
{
    TimelineData data;
    data.name = o.value(QLatin1String("name")).toString();
    const QJsonObject fps = o.value(QLatin1String("fps")).toObject();
    data.fps = Rational {
        fps.value(QLatin1String("num")).toInt(25), fps.value(QLatin1String("den")).toInt(1)};
    data.width = o.value(QLatin1String("width")).toInt(1920);
    data.height = o.value(QLatin1String("height")).toInt(1080);
    data.markIn = o.value(QLatin1String("markIn")).toInteger(-1);
    data.markOut = o.value(QLatin1String("markOut")).toInteger(-1);
    data.nextId = quint64(o.value(QLatin1String("nextId")).toInteger(1));
    for (const QJsonValue& v : o.value(QLatin1String("markers")).toArray()) {
        const QJsonObject m = v.toObject();
        Marker marker;
        marker.frame = m.value(QLatin1String("frame")).toInteger();
        marker.name = m.value(QLatin1String("name")).toString();
        if (m.contains(QLatin1String("color")))
            marker.color = colorFromString(m.value(QLatin1String("color")).toString());
        data.markers.append(marker);
    }
    std::stable_sort(data.markers.begin(), data.markers.end(),
        [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
    for (const QJsonValue& v : o.value(QLatin1String("videoTracks")).toArray())
        data.videoTracks.push_back(trackFromJson(v.toObject(), TrackType::Video));
    for (const QJsonValue& v : o.value(QLatin1String("audioTracks")).toArray())
        data.audioTracks.push_back(trackFromJson(v.toObject(), TrackType::Audio));

    editops::normalizeIds(data);
    QString why;
    if (!editops::checkInvariants(data, &why)) {
        if (error)
            *error = QStringLiteral("invalid timeline: %1").arg(why);
        return false;
    }
    out = std::move(data);
    return true;
}

// ---- Media ----

QJsonObject mediaInfoToJson(const media::MediaInfo& info)
{
    return QJsonObject {
        {QLatin1String("valid"), info.valid},
        {QLatin1String("error"), info.error},
        {QLatin1String("hasVideo"), info.hasVideo},
        {QLatin1String("hasAudio"), info.hasAudio},
        {QLatin1String("isStillImage"), info.isStillImage},
        {QLatin1String("width"), info.width},
        {QLatin1String("height"), info.height},
        {QLatin1String("rotation"), info.rotation},
        {QLatin1String("frameRate"),
            QJsonObject {{QLatin1String("num"), info.frameRate.num},
                {QLatin1String("den"), info.frameRate.den}}},
        {QLatin1String("durationUs"), qint64(info.durationUs)},
        {QLatin1String("audioChannels"), info.audioChannels},
        {QLatin1String("audioSampleRate"), info.audioSampleRate},
        {QLatin1String("videoCodec"), info.videoCodec},
        {QLatin1String("audioCodec"), info.audioCodec},
        {QLatin1String("container"), info.container},
    };
}

media::MediaInfo mediaInfoFromJson(const QJsonObject& o)
{
    media::MediaInfo info;
    info.valid = o.value(QLatin1String("valid")).toBool();
    info.error = o.value(QLatin1String("error")).toString();
    info.hasVideo = o.value(QLatin1String("hasVideo")).toBool();
    info.hasAudio = o.value(QLatin1String("hasAudio")).toBool();
    info.isStillImage = o.value(QLatin1String("isStillImage")).toBool();
    info.width = o.value(QLatin1String("width")).toInt();
    info.height = o.value(QLatin1String("height")).toInt();
    info.rotation = o.value(QLatin1String("rotation")).toInt();
    const QJsonObject fr = o.value(QLatin1String("frameRate")).toObject();
    info.frameRate = Rational {
        fr.value(QLatin1String("num")).toInt(0), fr.value(QLatin1String("den")).toInt(1)};
    info.durationUs = o.value(QLatin1String("durationUs")).toInteger();
    info.audioChannels = o.value(QLatin1String("audioChannels")).toInt();
    info.audioSampleRate = o.value(QLatin1String("audioSampleRate")).toInt();
    info.videoCodec = o.value(QLatin1String("videoCodec")).toString();
    info.audioCodec = o.value(QLatin1String("audioCodec")).toString();
    info.container = o.value(QLatin1String("container")).toString();
    return info;
}

QJsonObject mediaPoolToJson(const MediaPoolState& state, const QDir& projectDir)
{
    QJsonArray bins;
    for (const Bin& b : state.bins) {
        bins.append(QJsonObject {{QLatin1String("id"), b.id}, {QLatin1String("name"), b.name},
            {QLatin1String("parentId"), b.parentId}});
    }
    QJsonArray items;
    for (const MediaItem& m : state.items) {
        items.append(QJsonObject {{QLatin1String("id"), m.id}, {QLatin1String("path"), m.path},
            {QLatin1String("relativePath"), projectDir.relativeFilePath(m.path)},
            {QLatin1String("name"), m.name}, {QLatin1String("binId"), m.binId},
            {QLatin1String("info"), mediaInfoToJson(m.info)}});
    }
    return QJsonObject {{QLatin1String("bins"), bins}, {QLatin1String("items"), items}};
}

MediaPoolState mediaPoolFromJson(const QJsonObject& json, const QDir& projectDir)
{
    MediaPoolState state;
    for (const QJsonValue& v : json.value(QLatin1String("bins")).toArray()) {
        const QJsonObject b = v.toObject();
        state.bins.append(
            Bin {b.value(QLatin1String("id")).toString(), b.value(QLatin1String("name")).toString(),
                b.value(QLatin1String("parentId")).toString()});
    }
    for (const QJsonValue& v : json.value(QLatin1String("items")).toArray()) {
        const QJsonObject o = v.toObject();
        MediaItem m;
        m.id = o.value(QLatin1String("id")).toString();
        m.path = o.value(QLatin1String("path")).toString();
        const QString relative = o.value(QLatin1String("relativePath")).toString();
        if (!QFileInfo::exists(m.path) && !relative.isEmpty()) {
            const QString candidate = projectDir.absoluteFilePath(relative);
            if (QFileInfo::exists(candidate))
                m.path = QFileInfo(candidate).absoluteFilePath();
        }
        m.name = o.value(QLatin1String("name")).toString();
        m.binId = o.value(QLatin1String("binId")).toString();
        m.info = mediaInfoFromJson(o.value(QLatin1String("info")).toObject());
        m.info.path = m.path;
        state.items.append(m);
    }
    return state;
}

} // namespace cw::serialization
