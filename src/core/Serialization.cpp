// SPDX-License-Identifier: GPL-3.0-or-later
// JSON encoding of timelines, clips, parameter sets and the media pool.
#include "core/Serialization.h"

#include "core/ClipColors.h"
#include "core/EditOps.h"
#include "core/FileParams.h"
#include "core/MixerJson.h"
#include "core/Retime.h"
#include "core/SmartBins.h"
#include "core/ViewerGuides.h"
#include "media/MediaSource.h"

#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <optional>

namespace kk::serialization {

namespace {

// ---- Load-time limits ----
// A damaged or hand-edited file must not drive the renderer into absurd allocations or
// integer overflow (frames -> microseconds -> samples multiply by up to 48000), so loading
// rejects values far outside anything an edit can produce.
constexpr int kMaxDimension = 16384; ///< px; largest texture size GL 3.3 hardware supports
constexpr double kMinFps = 1.0;
constexpr double kMaxFps = 1000.0;
constexpr int64_t kMaxFrames = int64_t(1) << 32; ///< ~4.5 years at 30 fps
constexpr int64_t kMaxSourceUs = int64_t(1) << 50; ///< ~35 years
constexpr double kMaxSpeed = 1.0e6; ///< fit-to-fill of hours into a frame stays below this

/// Why @p c is unusable, or empty.
QString clipLimitError(const ClipData& c)
{
    if (c.start < 0 || c.start > kMaxFrames || c.duration > kMaxFrames)
        return QStringLiteral("clip %1 position out of range").arg(c.id);
    if (!std::isfinite(c.speed) || std::abs(c.speed) > kMaxSpeed)
        return QStringLiteral("clip %1 speed out of range").arg(c.id);
    if (c.sourceInUs > kMaxSourceUs || c.sourceDurationUs < 0 || c.sourceDurationUs > kMaxSourceUs)
        return QStringLiteral("clip %1 source range out of range").arg(c.id);
    for (const AnimatedValue& v : c.retime.values) {
        for (const Keyframe& k : v.keys) {
            if (!std::isfinite(k.value) || std::abs(k.value) > double(kMaxSourceUs))
                return QStringLiteral("clip %1 retime curve out of range").arg(c.id);
        }
    }
    return {};
}

/// Transition lengths are clamped to [1, clip duration] (they are drawn and rendered
/// within the clip; anything else is meaningless).
void clampTransition(std::optional<TransitionData>& t, int64_t clipDuration)
{
    if (t)
        t->duration = std::clamp<int64_t>(t->duration, 1, std::max<int64_t>(1, clipDuration));
}

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
    case ClipKind::Nested:
        return QStringLiteral("nested");
    case ClipKind::Adjustment:
        return QStringLiteral("adjustment");
    case ClipKind::Generator:
        return QStringLiteral("generator");
    case ClipKind::Subtitle:
        return QStringLiteral("subtitle");
    }
    return QStringLiteral("media");
}

ClipKind kindFromString(const QString& text)
{
    if (text == QLatin1String("title"))
        return ClipKind::Title;
    if (text == QLatin1String("solid"))
        return ClipKind::Solid;
    if (text == QLatin1String("nested"))
        return ClipKind::Nested;
    if (text == QLatin1String("adjustment"))
        return ClipKind::Adjustment;
    if (text == QLatin1String("generator"))
        return ClipKind::Generator;
    if (text == QLatin1String("subtitle"))
        return ClipKind::Subtitle;
    return ClipKind::Media;
}

QJsonObject transitionToJson(const TransitionData& t)
{
    QJsonObject o;
    o[QLatin1String("type")] = t.type;
    o[QLatin1String("duration")] = qint64(t.duration);
    // Centre (the default) is omitted, so files of earlier versions stay identical.
    if (t.alignment == TransitionAlignment::StartOnCut)
        o[QLatin1String("alignment")] = QStringLiteral("start");
    else if (t.alignment == TransitionAlignment::EndOnCut)
        o[QLatin1String("alignment")] = QStringLiteral("end");
    if (t.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(t.params);
    return o;
}

TransitionData transitionFromJson(const QJsonObject& o)
{
    TransitionData t;
    t.type = o.value(QLatin1String("type")).toString(t.type);
    t.duration = o.value(QLatin1String("duration")).toInteger(t.duration);
    const QString alignment = o.value(QLatin1String("alignment")).toString();
    if (alignment == QLatin1String("start"))
        t.alignment = TransitionAlignment::StartOnCut;
    else if (alignment == QLatin1String("end"))
        t.alignment = TransitionAlignment::EndOnCut;
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
    o[QLatin1String("autoSelect")] = track.autoSelect;
    o[QLatin1String("height")] = track.height;
    // Format 13: the mixer channel of audio tracks, omitted while it has the defaults.
    if (track.type == TrackType::Audio && track.mix != TrackMix {})
        o[QLatin1String("mix")] = trackMixToJson(track.mix);
    if (track.type == TrackType::Subtitle) {
        o[QLatin1String("subtitleStyle")] = paramSetToJson(track.subtitleStyle);
        if (!track.language.isEmpty())
            o[QLatin1String("language")] = track.language;
    }
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
    t.autoSelect = o.value(QLatin1String("autoSelect")).toBool(true);
    t.height = o.value(QLatin1String("height")).toInt(0);
    if (type == TrackType::Audio) // formats 1 - 11 have no "mix": the defaults
        t.mix = trackMixFromJson(o.value(QLatin1String("mix")).toObject());
    if (type == TrackType::Subtitle) {
        t.subtitleStyle = paramSetFromJson(o.value(QLatin1String("subtitleStyle")).toObject());
        t.language = o.value(QLatin1String("language")).toString();
    }
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
    case QMetaType::QVariantMap: // a File parameter in its stored form (core/FileParams.h)
        return QJsonObject::fromVariantMap(value.toMap());
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
        if (o.contains(QLatin1String(fileparams::kFileKey)))
            return o.toVariantMap(); // decoded by fileparams::decodeAfterLoad()
        return {};
    }
    return json.toVariant();
}

QJsonValue keyHandleToJson(const KeyHandle& h)
{
    if (h.mode != KeyHandle::Mode::Free)
        return handleModeToString(h.mode);
    return QJsonObject {{QLatin1String("mode"), handleModeToString(h.mode)},
        {QLatin1String("dt"), h.dtFrames}, {QLatin1String("dv"), h.dv}};
}

KeyHandle keyHandleFromJson(const QJsonValue& json)
{
    KeyHandle h;
    if (json.isString()) {
        h.mode = handleModeFromString(json.toString());
    } else if (json.isObject()) {
        const QJsonObject o = json.toObject();
        h.mode = handleModeFromString(o.value(QLatin1String("mode")).toString());
        h.dtFrames = o.value(QLatin1String("dt")).toDouble();
        h.dv = o.value(QLatin1String("dv")).toDouble();
    }
    return h;
}

QJsonObject keyframeToJson(const Keyframe& k)
{
    // Defaults (linear handles, no hold) are omitted to keep files small and diffable.
    QJsonObject o {{QLatin1String("frame"), qint64(k.frame)}, {QLatin1String("value"), k.value}};
    if (k.in != KeyHandle {})
        o[QLatin1String("in")] = keyHandleToJson(k.in);
    if (k.out != KeyHandle {})
        o[QLatin1String("out")] = keyHandleToJson(k.out);
    if (k.hold)
        o[QLatin1String("hold")] = true;
    return o;
}

Keyframe keyframeFromJson(const QJsonObject& o)
{
    Keyframe k;
    k.frame = o.value(QLatin1String("frame")).toInteger();
    k.value = o.value(QLatin1String("value")).toDouble();
    k.in = keyHandleFromJson(o.value(QLatin1String("in")));
    k.out = keyHandleFromJson(o.value(QLatin1String("out")));
    k.hold = o.value(QLatin1String("hold")).toBool();
    return k;
}

QJsonObject paramSetToJson(const ParamSet& params)
{
    QJsonObject values;
    for (auto it = params.values.cbegin(); it != params.values.cend(); ++it) {
        QJsonObject v;
        v[QLatin1String("base")] = it->base;
        if (it->isAnimated()) {
            QJsonArray keys;
            for (const Keyframe& k : it->keys)
                keys.append(keyframeToJson(k));
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
        const QJsonArray keys = v.value(QLatin1String("keys")).toArray();
        // Format version 1 stored one "interp" per key (the curve of the segment starting
        // there); it is detected per key and converted once all keys are known, because a v1
        // easeOut changes the *next* key's handle.
        QVector<Interp> legacy;
        bool hasLegacy = false;
        for (const QJsonValue& kv : keys) {
            const QJsonObject k = kv.toObject();
            value.putKey(keyframeFromJson(k));
            hasLegacy = hasLegacy || k.contains(QLatin1String("interp"));
        }
        if (hasLegacy) {
            // putKey() sorted the keys; map the legacy modes by frame.
            QMap<qint64, Interp> byFrame;
            for (const QJsonValue& kv : keys) {
                const QJsonObject k = kv.toObject();
                byFrame.insert(k.value(QLatin1String("frame")).toInteger(),
                    interpFromString(k.value(QLatin1String("interp")).toString()));
            }
            legacy.reserve(value.keys.size());
            for (const Keyframe& key : value.keys)
                legacy.append(byFrame.value(key.frame, Interp::Linear));
            migrateLegacyInterp(value, legacy);
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

namespace {

QJsonArray markersToJson(const QVector<Marker>& list)
{
    QJsonArray markers;
    for (const Marker& m : list) {
        QJsonObject o {{QLatin1String("frame"), qint64(m.frame)}, {QLatin1String("name"), m.name},
            {QLatin1String("color"), colorToString(m.color)}};
        if (!m.note.isEmpty())
            o[QLatin1String("note")] = m.note;
        if (m.duration > 1) // single-frame markers stay as format 12 wrote them
            o[QLatin1String("duration")] = qint64(m.duration);
        markers.append(o);
    }
    return markers;
}

/// Markers sorted by frame (files edited by hand may list them in any order).
QVector<Marker> markersFromJson(const QJsonArray& array)
{
    QVector<Marker> list;
    for (const QJsonValue& v : array) {
        const QJsonObject m = v.toObject();
        Marker marker;
        marker.frame = m.value(QLatin1String("frame")).toInteger();
        marker.name = m.value(QLatin1String("name")).toString();
        marker.note = m.value(QLatin1String("note")).toString();
        marker.duration = std::max<int64_t>(1, m.value(QLatin1String("duration")).toInteger(1));
        if (m.contains(QLatin1String("color")))
            marker.color = colorFromString(m.value(QLatin1String("color")).toString());
        list.append(marker);
    }
    std::stable_sort(list.begin(), list.end(),
        [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
    return list;
}

QJsonArray maskPathToJson(const MaskPath& path)
{
    // Compact: one array per point, [x, y, inX, inY, outX, outY] plus 1 for a smooth point.
    QJsonArray points;
    for (const MaskPoint& p : path) {
        QJsonArray a {p.x, p.y, p.inX, p.inY, p.outX, p.outY};
        if (p.smooth)
            a.append(1);
        points.append(a);
    }
    return points;
}

MaskPath maskPathFromJson(const QJsonArray& array)
{
    MaskPath path;
    path.reserve(array.size());
    for (const QJsonValue& v : array) {
        const QJsonArray a = v.toArray();
        if (a.size() < 2)
            continue;
        MaskPoint p;
        p.x = a.at(0).toDouble();
        p.y = a.at(1).toDouble();
        p.inX = a.at(2).toDouble();
        p.inY = a.at(3).toDouble();
        p.outX = a.at(4).toDouble();
        p.outY = a.at(5).toDouble();
        p.smooth = a.at(6).toInt() != 0;
        path.append(p);
    }
    return path;
}

QJsonObject maskToJson(const MaskData& m)
{
    QJsonObject o {{QLatin1String("id"), qint64(m.id)},
        {QLatin1String("shape"), maskShapeToString(m.shape)},
        {QLatin1String("mode"), maskModeToString(m.mode)}};
    if (!m.name.isEmpty())
        o[QLatin1String("name")] = m.name;
    if (!m.enabled)
        o[QLatin1String("enabled")] = false;
    if (m.invert)
        o[QLatin1String("invert")] = true;
    if (!m.closed)
        o[QLatin1String("closed")] = false;
    if (m.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(m.params);
    if (m.isPathShape())
        animatedPathToJson(m.path, o);
    return o;
}

MaskData maskFromJson(const QJsonObject& o)
{
    MaskData m;
    m.id = quint64(o.value(QLatin1String("id")).toInteger());
    m.name = o.value(QLatin1String("name")).toString();
    m.shape = maskShapeFromString(o.value(QLatin1String("shape")).toString());
    m.mode = maskModeFromString(o.value(QLatin1String("mode")).toString());
    m.enabled = o.value(QLatin1String("enabled")).toBool(true);
    m.invert = o.value(QLatin1String("invert")).toBool(false);
    m.closed = o.value(QLatin1String("closed")).toBool(true);
    m.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
    animatedPathFromJson(o, m.path);
    return m;
}

} // namespace

void animatedPathToJson(const AnimatedPath& path, QJsonObject& o)
{
    if (!path.isAnimated()) {
        o[QLatin1String("points")] = maskPathToJson(path.shapes.value(0));
        return;
    }
    // Shape keys: the keyframe (frame + easing) with the points of that key.
    QJsonArray keys;
    for (qsizetype i = 0; i < path.timing.keys.size(); ++i) {
        QJsonObject k = keyframeToJson(path.timing.keys[i]);
        k.remove(QLatin1String("value")); // the index; implied by the order
        k[QLatin1String("points")] = maskPathToJson(path.shapes.value(i));
        keys.append(k);
    }
    o[QLatin1String("shapeKeys")] = keys;
}

void animatedPathFromJson(const QJsonObject& o, AnimatedPath& path)
{
    const QJsonArray keys = o.value(QLatin1String("shapeKeys")).toArray();
    if (keys.isEmpty()) {
        path.shapes = {maskPathFromJson(o.value(QLatin1String("points")).toArray())};
    } else {
        path.shapes.clear();
        for (const QJsonValue& v : keys) {
            const QJsonObject ko = v.toObject();
            Keyframe k = keyframeFromJson(ko);
            if (path.timing.hasKeyAt(k.frame))
                continue; // duplicate frame in a hand-edited file: first one wins
            k.value = double(path.shapes.size());
            path.shapes.append(maskPathFromJson(ko.value(QLatin1String("points")).toArray()));
            path.timing.putKey(k);
        }
        path.normalize(); // sorts the shapes into frame order
    }
}

QJsonObject attributesToJson(const ClipAttributes& a)
{
    // Only what differs from the default is written (most items have no attributes).
    const ClipAttributes d;
    QJsonObject o;
    if (a.pixelAspect.isValid()) {
        o[QLatin1String("pixelAspect")] = QJsonArray {a.pixelAspect.num, a.pixelAspect.den};
    }
    if (a.levels != d.levels)
        o[QLatin1String("levels")] = dataLevelsToString(a.levels);
    if (a.fieldOrder != d.fieldOrder)
        o[QLatin1String("fieldOrder")] = fieldOrderToString(a.fieldOrder);
    if (a.deinterlace)
        o[QLatin1String("deinterlace")] = true;
    if (a.rotation != 0)
        o[QLatin1String("rotation")] = a.rotation;
    if (a.channelFormat != d.channelFormat) {
        o[QLatin1String("channelFormat")] = channelFormatToString(a.channelFormat);
        o[QLatin1String("left")] = a.left;
        o[QLatin1String("right")] = a.right;
    }
    if (a.overrideTimecode)
        o[QLatin1String("startTimecode")] = qint64(a.startTimecodeFrames);
    return o;
}

ClipAttributes attributesFromJson(const QJsonObject& o)
{
    ClipAttributes a;
    const QJsonArray par = o.value(QLatin1String("pixelAspect")).toArray();
    if (par.size() == 2) {
        const Rational r {par[0].toInt(), par[1].toInt()};
        if (r.isValid())
            a.pixelAspect = r;
    }
    a.levels = dataLevelsFromString(o.value(QLatin1String("levels")).toString());
    a.fieldOrder = fieldOrderFromString(o.value(QLatin1String("fieldOrder")).toString());
    a.deinterlace = o.value(QLatin1String("deinterlace")).toBool(false);
    a.rotation = normalizedRotation(o.value(QLatin1String("rotation")).toInt(0));
    a.channelFormat = channelFormatFromString(o.value(QLatin1String("channelFormat")).toString());
    a.left = o.value(QLatin1String("left")).toInt(0);
    a.right = o.value(QLatin1String("right")).toInt(1);
    if (o.contains(QLatin1String("startTimecode"))) {
        a.overrideTimecode = true;
        a.startTimecodeFrames
            = std::max<int64_t>(0, o.value(QLatin1String("startTimecode")).toInteger());
    }
    return a;
}

QJsonObject clipToJson(const ClipData& clip)
{
    QJsonObject o;
    o[QLatin1String("id")] = qint64(clip.id);
    o[QLatin1String("kind")] = kindToString(clip.kind);
    if (!clip.mediaId.isEmpty())
        o[QLatin1String("mediaId")] = clip.mediaId;
    if (clip.nestedUid != 0)
        o[QLatin1String("nestedUid")] = qint64(clip.nestedUid);
    if (clip.multicamAngle >= 0)
        o[QLatin1String("multicamAngle")] = clip.multicamAngle;
    o[QLatin1String("name")] = clip.name;
    if (clip.label.isValid())
        o[QLatin1String("label")] = colorToString(clip.label);
    o[QLatin1String("start")] = qint64(clip.start);
    o[QLatin1String("duration")] = qint64(clip.duration);
    o[QLatin1String("sourceInUs")] = qint64(clip.sourceInUs);
    o[QLatin1String("sourceDurationUs")] = qint64(clip.sourceDurationUs);
    if (retime::isRetimed(clip) || clip.retime != ParamSet {}
        || clip.retimeProcess != RetimeProcess::ProjectDefault || !clip.pitchCorrection) {
        QJsonObject r;
        r[QLatin1String("speed")] = clip.speed;
        if (clip.retime != ParamSet {})
            r[QLatin1String("curve")] = paramSetToJson(clip.retime);
        if (clip.retimeProcess != RetimeProcess::ProjectDefault)
            r[QLatin1String("process")] = retime::processToString(clip.retimeProcess);
        r[QLatin1String("pitchCorrection")] = clip.pitchCorrection;
        o[QLatin1String("retime")] = r;
    }
    o[QLatin1String("enabled")] = clip.enabled;
    if (clip.linkGroup != 0)
        o[QLatin1String("linkGroup")] = qint64(clip.linkGroup);
    if (clip.cacheOutput)
        o[QLatin1String("cacheOutput")] = true;
    if (clip.fadeInFrames > 0)
        o[QLatin1String("fadeIn")] = qint64(clip.fadeInFrames);
    if (clip.fadeOutFrames > 0)
        o[QLatin1String("fadeOut")] = qint64(clip.fadeOutFrames);
    if (!clip.attributes.isDefault())
        o[QLatin1String("attributes")] = attributesToJson(clip.attributes);
    if (clip.transform != ParamSet {})
        o[QLatin1String("transform")] = paramSetToJson(clip.transform);
    if (clip.audio != ParamSet {})
        o[QLatin1String("audio")] = paramSetToJson(clip.audio);
    if (clip.title != ParamSet {})
        o[QLatin1String("title")] = paramSetToJson(clip.title);
    if (clip.solidColor.isValid())
        o[QLatin1String("solidColor")] = colorToString(clip.solidColor);
    if (!clip.generator.isEmpty())
        o[QLatin1String("generator")] = clip.generator;
    if (!clip.effects.isEmpty()) {
        QJsonArray effects;
        for (const EffectInstance& fx : clip.effects) {
            effects.append(QJsonObject {{QLatin1String("id"), qint64(fx.id)},
                {QLatin1String("type"), fx.type}, {QLatin1String("enabled"), fx.enabled},
                {QLatin1String("params"), paramSetToJson(fx.params)}});
        }
        o[QLatin1String("effects")] = effects;
    }
    if (!clip.masks.isEmpty()) {
        QJsonArray masks;
        for (const MaskData& m : clip.masks)
            masks.append(maskToJson(m));
        o[QLatin1String("masks")] = masks;
    }
    if (clip.transitionIn)
        o[QLatin1String("transitionIn")] = transitionToJson(*clip.transitionIn);
    if (clip.transitionOut)
        o[QLatin1String("transitionOut")] = transitionToJson(*clip.transitionOut);
    if (!clip.markers.isEmpty())
        o[QLatin1String("markers")] = markersToJson(clip.markers);
    if (clip.composition)
        o[QLatin1String("composition")] = compositionToJson(*clip.composition);
    if (!clip.grade.isEmpty()) // format 16; omitted when ungraded (older files identical)
        o[QLatin1String("grade")] = gradeToJson(clip.grade);
    if (!clip.stabilization.isDefault()) // spec 58; omitted when never used (older files identical)
        o[QLatin1String("stabilization")] = stabilize::toJson(clip.stabilization);
    return o;
}

ClipData clipFromJson(const QJsonObject& o)
{
    ClipData c;
    c.id = quint64(o.value(QLatin1String("id")).toInteger());
    c.kind = kindFromString(o.value(QLatin1String("kind")).toString());
    c.mediaId = o.value(QLatin1String("mediaId")).toString();
    c.nestedUid = quint64(o.value(QLatin1String("nestedUid")).toInteger());
    c.multicamAngle = std::max(-1, o.value(QLatin1String("multicamAngle")).toInt(-1));
    c.name = o.value(QLatin1String("name")).toString();
    if (o.contains(QLatin1String("label")))
        c.label = colorFromString(o.value(QLatin1String("label")).toString());
    c.start = o.value(QLatin1String("start")).toInteger();
    c.duration = o.value(QLatin1String("duration")).toInteger();
    c.sourceInUs = o.value(QLatin1String("sourceInUs")).toInteger();
    c.sourceDurationUs = o.value(QLatin1String("sourceDurationUs")).toInteger();
    if (o.contains(QLatin1String("retime"))) {
        const QJsonObject r = o.value(QLatin1String("retime")).toObject();
        c.speed = r.value(QLatin1String("speed")).toDouble(1.0);
        c.retime = paramSetFromJson(r.value(QLatin1String("curve")).toObject());
        c.retimeProcess = retime::processFromString(r.value(QLatin1String("process")).toString());
        c.pitchCorrection = r.value(QLatin1String("pitchCorrection")).toBool(true);
    } else {
        // Format <= 3: one constant speed (= one segment). Those versions resampled the audio
        // of retimed clips (pitch followed the speed): keep that sound.
        c.speed = o.value(QLatin1String("speed")).toDouble(1.0);
        c.pitchCorrection = c.speed == 1.0;
    }
    c.enabled = o.value(QLatin1String("enabled")).toBool(true);
    c.linkGroup = quint64(o.value(QLatin1String("linkGroup")).toInteger());
    c.cacheOutput = o.value(QLatin1String("cacheOutput")).toBool(false);
    c.fadeInFrames = std::max<int64_t>(0, o.value(QLatin1String("fadeIn")).toInteger());
    c.fadeOutFrames = std::max<int64_t>(0, o.value(QLatin1String("fadeOut")).toInteger());
    if (o.contains(QLatin1String("attributes")))
        c.attributes = attributesFromJson(o.value(QLatin1String("attributes")).toObject());
    c.transform = paramSetFromJson(o.value(QLatin1String("transform")).toObject());
    c.audio = paramSetFromJson(o.value(QLatin1String("audio")).toObject());
    c.title = paramSetFromJson(o.value(QLatin1String("title")).toObject());
    if (o.contains(QLatin1String("solidColor")))
        c.solidColor = colorFromString(o.value(QLatin1String("solidColor")).toString());
    c.generator = o.value(QLatin1String("generator")).toString();
    for (const QJsonValue& v : o.value(QLatin1String("effects")).toArray()) {
        const QJsonObject fo = v.toObject();
        EffectInstance fx;
        fx.id = quint64(fo.value(QLatin1String("id")).toInteger());
        fx.type = fo.value(QLatin1String("type")).toString();
        fx.enabled = fo.value(QLatin1String("enabled")).toBool(true);
        fx.params = paramSetFromJson(fo.value(QLatin1String("params")).toObject());
        c.effects.append(fx);
    }
    for (const QJsonValue& v : o.value(QLatin1String("masks")).toArray())
        c.masks.append(maskFromJson(v.toObject()));
    if (o.contains(QLatin1String("transitionIn")))
        c.transitionIn = transitionFromJson(o.value(QLatin1String("transitionIn")).toObject());
    if (o.contains(QLatin1String("transitionOut")))
        c.transitionOut = transitionFromJson(o.value(QLatin1String("transitionOut")).toObject());
    c.markers = markersFromJson(o.value(QLatin1String("markers")).toArray());
    if (o.contains(QLatin1String("composition")))
        c.composition = compositionFromJson(o.value(QLatin1String("composition")).toObject());
    // Format 16: Color page grade (absent = ungraded, what every older clip is).
    if (o.contains(QLatin1String("grade")))
        c.grade = gradeFromJson(o.value(QLatin1String("grade")).toObject());
    if (o.contains(QLatin1String("stabilization")))
        c.stabilization = stabilize::fromJson(o.value(QLatin1String("stabilization")).toObject());
    return c;
}

// ---- Timeline ----

QJsonObject timelineToJson(const TimelineData& data)
{
    QJsonObject o;
    if (data.uid != 0)
        o[QLatin1String("uid")] = qint64(data.uid);
    if (data.compound)
        o[QLatin1String("compound")] = true;
    if (data.multicam)
        o[QLatin1String("multicam")] = true;
    if (!data.useProjectSettings)
        o[QLatin1String("useProjectSettings")] = false;
    o[QLatin1String("name")] = data.name;
    o[QLatin1String("fps")]
        = QJsonObject {{QLatin1String("num"), data.fps.num}, {QLatin1String("den"), data.fps.den}};
    if (data.startTimecode != 0) // format 17; omitted at 00:00:00:00
        o[QLatin1String("startTimecode")] = qint64(data.startTimecode);
    o[QLatin1String("width")] = data.width;
    o[QLatin1String("height")] = data.height;
    if (data.retimeProcess != RetimeProcess::Nearest)
        o[QLatin1String("retimeProcess")] = retime::processToString(data.retimeProcess);
    o[QLatin1String("markIn")] = qint64(data.markIn);
    o[QLatin1String("markOut")] = qint64(data.markOut);
    o[QLatin1String("nextId")] = qint64(data.nextId);
    o[QLatin1String("markers")] = markersToJson(data.markers);
    QJsonArray video;
    for (const TrackData& t : data.videoTracks)
        video.append(trackToJson(t));
    o[QLatin1String("videoTracks")] = video;
    QJsonArray audio;
    for (const TrackData& t : data.audioTracks)
        audio.append(trackToJson(t));
    o[QLatin1String("audioTracks")] = audio;
    if (!data.subtitleTracks.empty()) { // format 7; omitted when empty (older files identical)
        QJsonArray subtitle;
        for (const TrackData& t : data.subtitleTracks)
            subtitle.append(trackToJson(t));
        o[QLatin1String("subtitleTracks")] = subtitle;
    }
    if (data.mixer != MixerData {}) // format 12; omitted when default (older files identical)
        o[QLatin1String("mixer")] = mixerToJson(data.mixer);
    if (!data.guides.empty()) // format 14; omitted when none (older files identical)
        o[QLatin1String("guides")] = guides::toJson(data.guides);
    if (!data.grade.isEmpty()) // format 16; omitted when ungraded
        o[QLatin1String("grade")] = gradeToJson(data.grade);
    return o;
}

bool timelineFromJson(const QJsonObject& o, TimelineData& out, QString* error)
{
    TimelineData data;
    data.uid = quint64(o.value(QLatin1String("uid")).toInteger());
    data.compound = o.value(QLatin1String("compound")).toBool(false);
    // A multicam timeline is always a compound one too (never counted as a regular timeline).
    data.multicam = o.value(QLatin1String("multicam")).toBool(false);
    data.compound = data.compound || data.multicam;
    data.useProjectSettings = o.value(QLatin1String("useProjectSettings")).toBool(true);
    data.name = o.value(QLatin1String("name")).toString();
    const QJsonObject fps = o.value(QLatin1String("fps")).toObject();
    data.fps = Rational {
        fps.value(QLatin1String("num")).toInt(25), fps.value(QLatin1String("den")).toInt(1)};
    data.startTimecode
        = std::clamp<int64_t>(o.value(QLatin1String("startTimecode")).toInteger(0), 0, kMaxFrames);
    data.width = o.value(QLatin1String("width")).toInt(1920);
    data.height = o.value(QLatin1String("height")).toInt(1080);
    data.retimeProcess = retime::processFromString(
        o.value(QLatin1String("retimeProcess")).toString(), RetimeProcess::Nearest);
    if (data.retimeProcess == RetimeProcess::ProjectDefault)
        data.retimeProcess = RetimeProcess::Nearest;
    auto fail = [error](const QString& why) {
        if (error)
            *error = QStringLiteral("invalid timeline: %1").arg(why);
        return false;
    };
    if (data.width > kMaxDimension || data.height > kMaxDimension)
        return fail(
            QStringLiteral("resolution %1x%2 is too large").arg(data.width).arg(data.height));
    if (data.fps.isValid() && (data.fps.toDouble() < kMinFps || data.fps.toDouble() > kMaxFps))
        return fail(QStringLiteral("frame rate %1 is out of range").arg(data.fps.toDouble()));
    data.markIn = o.value(QLatin1String("markIn")).toInteger(-1);
    data.markOut = o.value(QLatin1String("markOut")).toInteger(-1);
    data.nextId = quint64(o.value(QLatin1String("nextId")).toInteger(1));
    data.markers = markersFromJson(o.value(QLatin1String("markers")).toArray());
    for (const QJsonValue& v : o.value(QLatin1String("videoTracks")).toArray())
        data.videoTracks.push_back(trackFromJson(v.toObject(), TrackType::Video));
    for (const QJsonValue& v : o.value(QLatin1String("audioTracks")).toArray())
        data.audioTracks.push_back(trackFromJson(v.toObject(), TrackType::Audio));
    for (const QJsonValue& v : o.value(QLatin1String("subtitleTracks")).toArray())
        data.subtitleTracks.push_back(trackFromJson(v.toObject(), TrackType::Subtitle));
    // Format 13: buses and the output bus (absent = the main bus only); routes to buses that
    // do not exist are dropped.
    data.mixer = mixerFromJson(o.value(QLatin1String("mixer")).toObject());
    repairRoutes(data.mixer, data.audioTracks);
    // Format 14: viewer guides (absent = none, what every older timeline has).
    data.guides = guides::fromJson(o.value(QLatin1String("guides")).toArray());
    // Format 16: the timeline grade (absent = ungraded).
    if (o.contains(QLatin1String("grade")))
        data.grade = gradeFromJson(o.value(QLatin1String("grade")).toObject());

    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (TrackData& track : data.tracks(type)) {
            for (ClipData& c : track.clips) {
                if (const QString why = clipLimitError(c); !why.isEmpty())
                    return fail(why);
                clampTransition(c.transitionIn, c.duration);
                clampTransition(c.transitionOut, c.duration);
            }
        }
    }

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
    QJsonObject o {
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
    if (!info.startTimecode.isEmpty())
        o[QLatin1String("timecode")] = info.startTimecode;
    return o;
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
    info.startTimecode = o.value(QLatin1String("timecode")).toString();
    return info;
}

namespace {

QJsonObject metadataToJson(const MediaMetadata& m)
{
    QJsonObject o;
    auto text = [&o](const char* key, const QString& value) {
        if (!value.isEmpty())
            o[QLatin1String(key)] = value;
    };
    text("description", m.description);
    text("comments", m.comments);
    if (!m.keywords.isEmpty())
        o[QLatin1String("keywords")] = QJsonArray::fromStringList(m.keywords);
    text("scene", m.scene);
    text("shot", m.shot);
    text("take", m.take);
    text("camera", m.camera);
    text("angle", m.angle);
    if (m.goodTake)
        o[QLatin1String("goodTake")] = true;
    if (m.rating > 0)
        o[QLatin1String("rating")] = m.rating;
    return o;
}

MediaMetadata metadataFromJson(const QJsonObject& o)
{
    MediaMetadata m;
    m.description = o.value(QLatin1String("description")).toString();
    m.comments = o.value(QLatin1String("comments")).toString();
    for (const QJsonValue& k : o.value(QLatin1String("keywords")).toArray()) {
        if (const QString t = k.toString().trimmed();
            !t.isEmpty() && !m.keywords.contains(t, Qt::CaseInsensitive))
            m.keywords.append(t);
    }
    m.scene = o.value(QLatin1String("scene")).toString();
    m.shot = o.value(QLatin1String("shot")).toString();
    m.take = o.value(QLatin1String("take")).toString();
    m.camera = o.value(QLatin1String("camera")).toString();
    m.angle = o.value(QLatin1String("angle")).toString();
    m.goodTake = o.value(QLatin1String("goodTake")).toBool(false);
    m.rating = std::clamp(o.value(QLatin1String("rating")).toInt(0), 0, 5);
    return m;
}

/// Format 9 fields of an item (all optional: absent = default).
void writeItemExtras(const MediaItem& m, QJsonObject& o)
{
    if (!m.clipColor.isEmpty())
        o[QLatin1String("clipColor")] = m.clipColor;
    if (m.flags != 0)
        o[QLatin1String("flags")] = QJsonArray::fromStringList(flagIds(m.flags));
    if (!m.metadata.isEmpty())
        o[QLatin1String("metadata")] = metadataToJson(m.metadata);
    if (m.addedMs > 0)
        o[QLatin1String("added")] = m.addedMs;
    if (m.fileModifiedMs > 0)
        o[QLatin1String("modified")] = m.fileModifiedMs;
}

void readItemExtras(const QJsonObject& o, MediaItem& m)
{
    m.clipColor = o.value(QLatin1String("clipColor")).toString();
    if (!findClipColor(m.clipColor))
        m.clipColor.clear();
    QStringList flags;
    for (const QJsonValue& f : o.value(QLatin1String("flags")).toArray())
        flags.append(f.toString());
    m.flags = flagsFromIds(flags);
    m.metadata = metadataFromJson(o.value(QLatin1String("metadata")).toObject());
    m.addedMs = o.value(QLatin1String("added")).toInteger(0);
    m.fileModifiedMs = o.value(QLatin1String("modified")).toInteger(0);
}

QJsonObject smartBinToJson(const SmartBin& b)
{
    QJsonArray rules;
    for (const SmartRule& r : b.rules) {
        rules.append(QJsonObject {{QLatin1String("field"), smartbins::fieldId(r.field)},
            {QLatin1String("op"), smartbins::operatorId(r.op)}, {QLatin1String("value"), r.value}});
    }
    return QJsonObject {{QLatin1String("id"), b.id}, {QLatin1String("name"), b.name},
        {QLatin1String("match"), b.matchAll ? QStringLiteral("all") : QStringLiteral("any")},
        {QLatin1String("rules"), rules}};
}

SmartBin smartBinFromJson(const QJsonObject& o)
{
    SmartBin b;
    b.id = o.value(QLatin1String("id")).toString();
    b.name = o.value(QLatin1String("name")).toString();
    b.matchAll = o.value(QLatin1String("match")).toString() != QLatin1String("any");
    for (const QJsonValue& v : o.value(QLatin1String("rules")).toArray()) {
        const QJsonObject r = v.toObject();
        SmartRule rule;
        // Rules with an unknown field or operator (a newer version) are dropped.
        if (!smartbins::fieldFromId(r.value(QLatin1String("field")).toString(), &rule.field)
            || !smartbins::operatorFromId(r.value(QLatin1String("op")).toString(), &rule.op))
            continue;
        rule.value = r.value(QLatin1String("value")).toString();
        b.rules.append(rule);
    }
    return b;
}

} // namespace

QJsonObject mediaPoolToJson(const MediaPoolState& state, const QDir& projectDir)
{
    QJsonArray bins;
    for (const Bin& b : state.bins) {
        bins.append(QJsonObject {{QLatin1String("id"), b.id}, {QLatin1String("name"), b.name},
            {QLatin1String("parentId"), b.parentId}});
    }
    QJsonArray items;
    for (const MediaItem& m : state.items) {
        if (m.isTimeline()) {
            // Timeline / compound clip items: no file, the info is derived from the timeline.
            QJsonObject o {{QLatin1String("id"), m.id},
                {QLatin1String("kind"),
                    m.kind == MediaItemKind::MulticamClip       ? QStringLiteral("multicam")
                        : m.kind == MediaItemKind::CompoundClip ? QStringLiteral("compound")
                                                                : QStringLiteral("timeline")},
                {QLatin1String("timelineUid"), qint64(m.timelineUid)},
                {QLatin1String("name"), m.name}, {QLatin1String("binId"), m.binId},
                {QLatin1String("info"), mediaInfoToJson(m.info)}};
            writeItemExtras(m, o);
            items.append(o);
            continue;
        }
        QJsonObject item {{QLatin1String("id"), m.id}, {QLatin1String("path"), m.path},
            {QLatin1String("relativePath"), projectDir.relativeFilePath(m.path)},
            {QLatin1String("name"), m.name}, {QLatin1String("binId"), m.binId},
            {QLatin1String("info"), mediaInfoToJson(m.info)}};
        if (!m.attributes.isDefault())
            item[QLatin1String("attributes")] = attributesToJson(m.attributes);
        if (m.isSubtitle()) {
            // Format 7: the captions travel with the project ([startUs, endUs, text]).
            item[QLatin1String("kind")] = QStringLiteral("subtitle");
            QJsonArray cues;
            for (const SubtitleCue& c : m.cues)
                cues.append(QJsonArray {qint64(c.startUs), qint64(c.endUs), c.text});
            item[QLatin1String("cues")] = cues;
        }
        writeItemExtras(m, item);
        items.append(item);
    }
    QJsonObject pool {{QLatin1String("bins"), bins}, {QLatin1String("items"), items}};
    if (!state.smartBins.isEmpty()) {
        QJsonArray smart;
        for (const SmartBin& b : state.smartBins)
            smart.append(smartBinToJson(b));
        pool[QLatin1String("smartBins")] = smart;
    }
    return pool;
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
        const QString kind = o.value(QLatin1String("kind")).toString();
        if (kind == QLatin1String("timeline") || kind == QLatin1String("compound")
            || kind == QLatin1String("multicam")) {
            m.kind = kind == QLatin1String("multicam") ? MediaItemKind::MulticamClip
                : kind == QLatin1String("compound")    ? MediaItemKind::CompoundClip
                                                       : MediaItemKind::Timeline;
            m.timelineUid = quint64(o.value(QLatin1String("timelineUid")).toInteger());
            m.name = o.value(QLatin1String("name")).toString();
            m.binId = o.value(QLatin1String("binId")).toString();
            m.info = mediaInfoFromJson(o.value(QLatin1String("info")).toObject());
            readItemExtras(o, m);
            state.items.append(m);
            continue;
        }
        m.path = o.value(QLatin1String("path")).toString();
        const QString relative = o.value(QLatin1String("relativePath")).toString();
        // media::mediaExists(): media inside a single-file project ("film.kinoklip/media/a.mp4")
        // exist although no file has that path.
        if (!media::mediaExists(m.path) && !relative.isEmpty()) {
            const QString candidate = projectDir.absoluteFilePath(relative);
            if (QFileInfo::exists(candidate))
                m.path = QFileInfo(candidate).absoluteFilePath();
        }
        // Moved media: same file name next to the project file (project folder copied or
        // media collected into it). One stat per offline item, so loading stays cheap.
        if (!media::mediaExists(m.path) && !m.path.isEmpty()) {
            const QString candidate = projectDir.filePath(QFileInfo(m.path).fileName());
            if (QFileInfo(candidate).isFile())
                m.path = QFileInfo(candidate).absoluteFilePath();
        }
        m.name = o.value(QLatin1String("name")).toString();
        m.binId = o.value(QLatin1String("binId")).toString();
        m.info = mediaInfoFromJson(o.value(QLatin1String("info")).toObject());
        m.info.path = m.path;
        if (o.contains(QLatin1String("attributes")))
            m.attributes = attributesFromJson(o.value(QLatin1String("attributes")).toObject());
        if (kind == QLatin1String("subtitle")) {
            m.kind = MediaItemKind::Subtitle;
            for (const QJsonValue& cv : o.value(QLatin1String("cues")).toArray()) {
                const QJsonArray a = cv.toArray();
                if (a.size() >= 3)
                    m.cues.append({a[0].toInteger(), a[1].toInteger(), a[2].toString()});
            }
        }
        readItemExtras(o, m);
        state.items.append(m);
    }
    for (const QJsonValue& v : json.value(QLatin1String("smartBins")).toArray())
        state.smartBins.append(smartBinFromJson(v.toObject()));
    return state;
}

} // namespace kk::serialization
