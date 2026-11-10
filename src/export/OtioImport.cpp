// SPDX-License-Identifier: GPL-3.0-or-later
// OtioImport — reads an OpenTimelineIO (.otio) JSON file into a TimelineData.
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Serialization.h"
#include "export/Otio.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace kk::otio {

namespace {

namespace ops = editops;

/// A time range in seconds.
struct Range
{
    double start = 0.0;
    double duration = 0.0;
    double rate = 0.0; ///< rate of the duration, 0 if unknown
};

/// Name of an OTIO_SCHEMA value without its version: "Clip.2" -> "Clip".
QString schemaName(const QJsonObject& o)
{
    const QString schema = o.value(QLatin1String("OTIO_SCHEMA")).toString();
    return schema.left(
        schema.lastIndexOf(QLatin1Char('.')) < 0 ? schema.size() : schema.lastIndexOf('.'));
}

/// A RationalTime.1 as seconds (the rate snapped to the exact NTSC rate); false if it is not one.
bool readTime(const QJsonValue& v, double* seconds, double* rate = nullptr)
{
    const QJsonObject o = v.toObject();
    if (v.isNull() || !o.contains(QLatin1String("rate")) || !o.contains(QLatin1String("value")))
        return false;
    const Rational r = rateToRational(o.value(QLatin1String("rate")).toDouble());
    if (!r.isValid())
        return false;
    const double hz = r.toDouble();
    *seconds = o.value(QLatin1String("value")).toDouble() / hz;
    if (rate)
        *rate = hz;
    return true;
}

/// A TimeRange.1; false if it is null or malformed.
bool readRange(const QJsonValue& v, Range* out)
{
    const QJsonObject o = v.toObject();
    Range r;
    if (v.isNull() || !readTime(o.value(QLatin1String("start_time")), &r.start)
        || !readTime(o.value(QLatin1String("duration")), &r.duration, &r.rate))
        return false;
    *out = r;
    return true;
}

bool sameText(const QString& a, const char* b)
{
    return a.compare(QLatin1String(b), Qt::CaseInsensitive) == 0;
}

/// One import run: counts what is skipped and builds the timeline.
class Reader
{
public:
    Reader(const ImportOptions& options, TimelineImport& result)
        : m_options(options)
        , m_result(result)
    { }

    bool run(const QJsonObject& timeline)
    {
        const QJsonObject stack = timeline.value(QLatin1String("tracks")).toObject();
        const QJsonArray tracks = stack.value(QLatin1String("children")).toArray();
        m_fps = pickRate(timeline, tracks);

        TimelineData& data = m_result.timeline;
        data = makeDefaultTimeline(timeline.value(QLatin1String("name")).toString(), m_fps,
            m_options.width, m_options.height, 0, 0);
        if (data.name.isEmpty())
            data.name = m_options.fallbackName;
        noteMetadata(timeline);
        double startSeconds = 0.0;
        if (readTime(timeline.value(QLatin1String("global_start_time")), &startSeconds))
            data.startTimecode = std::max<int64_t>(0, frames(startSeconds));

        for (const QJsonValue& v : tracks)
            readTrack(v.toObject(), data);
        applyTransitions();
        readStackMarkers(stack, startSeconds);
        resolveMedia();
        for (auto it = m_skipped.begin(); it != m_skipped.end(); ++it) {
            m_result.skipped << (it.value() > 1
                    ? QStringLiteral("%1 (%2 times)").arg(it.key()).arg(it.value())
                    : it.key());
        }
        return true;
    }

private:
    struct PendingTransition
    {
        int track = 0; ///< index into m_trackRefs
        int nextChild = -1; ///< child index of the item after it (-1 = none)
        int prevChild = -1;
        int64_t before = 0, after = 0; ///< frames before / after the cut
        QString type;
        std::optional<ParamSet> params; ///< from metadata.kinoklip.params (a Kinoklip file)
    };

    void skip(const QString& what) { ++m_skipped[what]; }

    /// Notes metadata the importer does not use (Kinoklip's own key is not "skipped").
    void noteMetadata(const QJsonObject& o)
    {
        QJsonObject meta = o.value(QLatin1String("metadata")).toObject();
        meta.remove(QStringLiteral("kinoklip"));
        if (!meta.isEmpty())
            skip(QStringLiteral("metadata of %1")
                     .arg(o.value(QLatin1String("OTIO_SCHEMA")).toString()));
    }

    int64_t frames(double seconds) const
    {
        return int64_t(std::llround(seconds * m_fps.toDouble()));
    }

    /// Timeline rate: global_start_time, else the first Gap or other item with a range.
    Rational pickRate(const QJsonObject& timeline, const QJsonArray& tracks) const
    {
        double seconds = 0.0, hz = 0.0;
        if (readTime(timeline.value(QLatin1String("global_start_time")), &seconds, &hz))
            return rateToRational(hz);
        std::map<double, int> votes; // rate -> count; gaps count more than clips
        for (const QJsonValue& t : tracks) {
            for (const QJsonValue& c : t.toObject().value(QLatin1String("children")).toArray()) {
                const QJsonObject item = c.toObject();
                Range r;
                if (readRange(item.value(QLatin1String("source_range")), &r) && r.rate > 0.0)
                    votes[r.rate] += schemaName(item) == QLatin1String("Gap") ? 1000 : 1;
            }
        }
        const auto best = std::max_element(votes.begin(), votes.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        return best == votes.end() ? m_options.defaultFps : rateToRational(best->first);
    }

    /// Reference object of a clip (Clip.1 media_reference or the active one of Clip.2).
    QJsonObject referenceOf(const QJsonObject& clip) const
    {
        if (clip.contains(QLatin1String("media_references"))) {
            const QJsonObject refs = clip.value(QLatin1String("media_references")).toObject();
            QString key = clip.value(QLatin1String("active_media_reference_key")).toString();
            if (!refs.contains(key))
                key = QStringLiteral("DEFAULT_MEDIA");
            if (refs.contains(key))
                return refs.value(key).toObject();
            return refs.isEmpty() ? QJsonObject() : refs.begin().value().toObject();
        }
        return clip.value(QLatin1String("media_reference")).toObject();
    }

    void readTrack(const QJsonObject& track, TimelineData& data)
    {
        const QString kind = track.value(QLatin1String("kind")).toString();
        if (schemaName(track) != QLatin1String("Track")
            || !(sameText(kind, "Video") || sameText(kind, "Audio"))) {
            skip(QStringLiteral("%1 (kind \"%2\")")
                     .arg(track.value(QLatin1String("OTIO_SCHEMA")).toString(), kind));
            return;
        }
        const TrackType type = sameText(kind, "Video") ? TrackType::Video : TrackType::Audio;
        const int index = ops::addTrack(data, type);
        const TrackRef ref {type, index};
        TrackData& t = *data.track(ref);
        t.name = track.value(QLatin1String("name")).toString();
        if (!track.value(QLatin1String("enabled")).toBool(true)) {
            t.enabled = false;
            t.muted = true;
        }
        noteMetadata(track);
        if (!track.value(QLatin1String("effects")).toArray().isEmpty())
            skip(QStringLiteral("effects on a track"));
        if (!track.value(QLatin1String("markers")).toArray().isEmpty())
            skip(QStringLiteral("markers on a track"));

        const QJsonArray children = track.value(QLatin1String("children")).toArray();
        const int trackId = int(m_trackRefs.size());
        m_trackRefs.push_back(ref);
        std::vector<quint64> clipIds(size_t(children.size()), 0);
        double cursor = 0.0;
        for (int i = 0; i < children.size(); ++i) {
            const QJsonObject item = children[i].toObject();
            const QString schema = schemaName(item);
            if (schema == QLatin1String("Transition")) {
                readTransition(item, trackId, i);
                continue;
            }
            Range range;
            const bool hasRange = readRange(item.value(QLatin1String("source_range")), &range);
            if (schema == QLatin1String("Gap")) {
                if (hasRange)
                    cursor += range.duration;
                continue;
            }
            if (schema != QLatin1String("Clip")) {
                // A nested stack or track, or something unknown: its length is kept as a gap.
                skip(item.value(QLatin1String("OTIO_SCHEMA")).toString());
                if (hasRange)
                    cursor += range.duration;
                continue;
            }
            clipIds[size_t(i)] = readClip(item, ref, type, cursor);
        }
        m_clipIds.push_back(std::move(clipIds));
    }

    /// Reads one clip, places it at @p cursor seconds (advanced by its length) and returns its
    /// id (0 if it could not be placed).
    quint64 readClip(const QJsonObject& item, TrackRef ref, TrackType type, double& cursor)
    {
        const QJsonObject reference = referenceOf(item);
        Range available;
        const bool hasAvailable
            = readRange(reference.value(QLatin1String("available_range")), &available);
        Range source;
        bool hasSource = readRange(item.value(QLatin1String("source_range")), &source);
        if (!hasSource && hasAvailable) {
            source = available;
            hasSource = true;
        }
        if (!hasSource) {
            skip(QStringLiteral("clip without a length"));
            return 0;
        }

        ClipData clip;
        clip.name = item.value(QLatin1String("name")).toString();
        clip.enabled = item.value(QLatin1String("enabled")).toBool(true);
        noteMetadata(item);

        double speed = 1.0;
        for (const QJsonValue& e : item.value(QLatin1String("effects")).toArray()) {
            const QJsonObject effect = e.toObject();
            const QString name = schemaName(effect);
            if (name == QLatin1String("LinearTimeWarp")) {
                speed = effect.value(QLatin1String("time_scalar")).toDouble(1.0);
            } else if (name == QLatin1String("FreezeFrame")) {
                speed = 0.0;
            } else {
                skip(QStringLiteral("effect %1")
                         .arg(effect.value(QLatin1String("OTIO_SCHEMA")).toString()));
            }
        }
        if (!std::isfinite(speed))
            speed = 1.0;
        clip.speed = speed;

        // Source in: relative to the media's first frame when the reference says where that is.
        const double inSeconds = source.start - (hasAvailable ? available.start : 0.0);
        clip.sourceInUs = std::max<int64_t>(0, int64_t(std::llround(inSeconds * 1e6)));
        if (inSeconds < -1e-6)
            skip(QStringLiteral("clip source start before the start of its media"));

        const int64_t startFrame = frames(cursor);
        const int64_t endFrame = frames(cursor + source.duration);
        cursor += source.duration;
        clip.start = startFrame;
        clip.duration = endFrame - startFrame;
        if (clip.duration < 1) {
            skip(QStringLiteral("clip shorter than one frame"));
            return 0;
        }

        // Media: the real file is found after the whole file is read (resolveMedia()).
        const QString refSchema = reference.value(QLatin1String("OTIO_SCHEMA")).toString();
        QString path;
        if (schemaName(reference) == QLatin1String("ExternalReference")) {
            path = localPath(reference.value(QLatin1String("target_url")).toString());
            noteMetadata(reference);
        } else if (!refSchema.isEmpty()
            && schemaName(reference) != QLatin1String("MissingReference")) {
            skip(refSchema); // generator, image sequence: the clip becomes offline
        }
        const QString key
            = path.isEmpty() ? QStringLiteral("name:") + clip.name : QStringLiteral("file:") + path;
        MediaRef& media = m_media[key];
        media.path = path;
        media.name = clip.name;
        (type == TrackType::Video ? media.hasVideo : media.hasAudio) = true;
        if (available.rate > 0.0)
            media.frameRate = rateToRational(available.rate);
        // Length of the media: what the reference says, else the furthest source time used.
        const double usedSeconds = inSeconds + std::abs(source.duration * speed);
        const double extentSeconds
            = hasAvailable ? std::max(available.duration, usedSeconds) : usedSeconds;
        media.durationUs = std::max(media.durationUs, int64_t(std::llround(extentSeconds * 1e6)));
        clip.mediaId = key;

        for (const QJsonValue& m : item.value(QLatin1String("markers")).toArray()) {
            const QJsonObject marker = m.toObject();
            Range marked;
            if (!readRange(marker.value(QLatin1String("marked_range")), &marked)) {
                skip(QStringLiteral("marker without a range"));
                continue;
            }
            // Clip markers sit in source time; the clip-local frame follows the speed.
            const double local
                = std::abs(speed) < 1e-9 ? 0.0 : (marked.start - source.start) / speed;
            const int64_t frame = frames(local);
            if (frame < 0) {
                skip(QStringLiteral("clip marker before the clip"));
                continue;
            }
            clip.markers.push_back(readMarker(marker, frame, marked.duration));
        }
        std::sort(clip.markers.begin(), clip.markers.end(),
            [](const Marker& a, const Marker& b) { return a.frame < b.frame; });

        const quint64 id = ops::overwriteClip(m_result.timeline, ref, std::move(clip));
        if (id)
            ++m_result.clipCount;
        return id;
    }

    Marker readMarker(const QJsonObject& o, int64_t frame, double durationSeconds)
    {
        Marker m;
        m.frame = frame;
        m.name = o.value(QLatin1String("name")).toString();
        m.note = o.value(QLatin1String("comment")).toString();
        m.duration = std::max<int64_t>(1, frames(durationSeconds));
        const QString exact = o.value(QLatin1String("metadata"))
                                  .toObject()
                                  .value(QLatin1String("kinoklip"))
                                  .toObject()
                                  .value(QLatin1String("color"))
                                  .toString();
        const QColor color = QColor::isValidColorName(exact) && !exact.isEmpty()
            ? QColor(exact)
            : markerColorFromOtio(o.value(QLatin1String("color")).toString());
        if (color.isValid())
            m.color = color;
        return m;
    }

    void readStackMarkers(const QJsonObject& stack, double startSeconds)
    {
        TimelineData& data = m_result.timeline;
        for (const QJsonValue& v : stack.value(QLatin1String("markers")).toArray()) {
            const QJsonObject marker = v.toObject();
            Range marked;
            if (!readRange(marker.value(QLatin1String("marked_range")), &marked)) {
                skip(QStringLiteral("marker without a range"));
                continue;
            }
            const int64_t frame = frames(marked.start - startSeconds);
            if (frame < 0) {
                skip(QStringLiteral("timeline marker before the start"));
                continue;
            }
            data.markers.push_back(readMarker(marker, frame, marked.duration));
        }
        std::sort(data.markers.begin(), data.markers.end(),
            [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
    }

    void readTransition(const QJsonObject& item, int trackId, int childIndex)
    {
        double inSeconds = 0.0, outSeconds = 0.0;
        if (!readTime(item.value(QLatin1String("in_offset")), &inSeconds)
            || !readTime(item.value(QLatin1String("out_offset")), &outSeconds)) {
            skip(QStringLiteral("transition without offsets"));
            return;
        }
        PendingTransition p;
        p.track = trackId;
        p.prevChild = childIndex - 1;
        p.nextChild = childIndex + 1;
        p.before = std::max<int64_t>(0, frames(inSeconds));
        p.after = std::max<int64_t>(0, frames(outSeconds));
        // SMPTE_Dissolve is the cross dissolve; a Kinoklip type written by timelineToOtio() comes
        // back as it was; every other type is imported as a cross dissolve and reported.
        const QString otioType = item.value(QLatin1String("transition_type")).toString();
        const QJsonObject ownData = item.value(QLatin1String("metadata"))
                                        .toObject()
                                        .value(QLatin1String("kinoklip"))
                                        .toObject();
        const QString own = ownData.value(QLatin1String("type")).toString();
        p.type = QStringLiteral("cross_dissolve");
        if (!own.isEmpty() && EffectRegistry::instance().find(own)) {
            p.type = own;
            if (ownData.contains(QLatin1String("params")))
                p.params = serialization::paramSetFromJson(
                    ownData.value(QLatin1String("params")).toObject());
        } else if (!otioType.isEmpty() && !sameText(otioType, "SMPTE_Dissolve"))
            skip(QStringLiteral("transition type %1 (imported as a cross dissolve)").arg(otioType));
        m_transitions.push_back(p);
    }

    /// Puts the transitions on the clips once every clip has its id: a transition belongs to
    /// the clip after it (between two clips, or a fade in after a gap), else to the clip
    /// before it (a fade out into a gap).
    void applyTransitions()
    {
        TimelineData& data = m_result.timeline;
        for (const PendingTransition& p : m_transitions) {
            const auto& ids = m_clipIds[size_t(p.track)];
            const auto id = [&ids](int child) {
                return child >= 0 && child < int(ids.size()) ? ids[size_t(child)] : quint64(0);
            };
            const int64_t length = p.before + p.after;
            if (length < 1) {
                skip(QStringLiteral("transition without a length"));
                continue;
            }
            TransitionData t = EffectRegistry::instance().createTransition(p.type, length);
            if (p.params)
                t.params = *p.params;
            t.alignment = p.before == 0 ? TransitionAlignment::StartOnCut
                : p.after == 0          ? TransitionAlignment::EndOnCut
                                        : TransitionAlignment::Centre;
            bool placed = false;
            if (const quint64 next = id(p.nextChild))
                placed = ops::placeTransition(data, next, true, t);
            else if (const quint64 prev = id(p.prevChild))
                placed = ops::placeTransition(data, prev, false, t);
            if (!placed)
                skip(QStringLiteral("transition that does not fit between its clips"));
        }
    }

    /// Local path of a target_url (file:// URL, or a plain path; relative ones against baseDir).
    QString localPath(const QString& url) const
    {
        if (url.isEmpty())
            return {};
        QString path;
        const QUrl parsed(url);
        if (parsed.isLocalFile())
            path = parsed.toLocalFile();
        else if (parsed.scheme().isEmpty() || parsed.scheme().size() == 1) // plain / "C:" path
            path = url;
        else
            return {}; // http: and other remote media are not fetched
        if (QDir::isRelativePath(path) && !m_options.baseDir.isEmpty())
            path = QDir(m_options.baseDir).absoluteFilePath(path);
        return QDir::cleanPath(path);
    }

    /// Asks the caller for the media pool item of every distinct reference and fills in the
    /// clips; clips whose item is refused are removed.
    void resolveMedia()
    {
        TimelineData& data = m_result.timeline;
        QHash<QString, ResolvedMedia> resolved;
        for (auto& [key, media] : m_media) {
            ResolvedMedia r;
            if (m_options.resolveMedia)
                r = m_options.resolveMedia(media);
            resolved.insert(key, r);
        }
        for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
            for (TrackData& track : data.tracks(type)) {
                for (auto it = track.clips.begin(); it != track.clips.end();) {
                    const ResolvedMedia r = resolved.value(it->mediaId);
                    if (r.mediaId.isEmpty()) {
                        it = track.clips.erase(it);
                        --m_result.clipCount;
                        skip(QStringLiteral("clip whose media could not be created"));
                        continue;
                    }
                    it->mediaId = r.mediaId;
                    it->sourceDurationUs = r.durationUs;
                    ++it;
                }
            }
        }
    }

    const ImportOptions& m_options;
    TimelineImport& m_result;
    Rational m_fps {24, 1};
    QMap<QString, int> m_skipped;
    std::map<QString, MediaRef> m_media; ///< by key; order of paths does not matter
    std::vector<TrackRef> m_trackRefs;
    std::vector<std::vector<quint64>> m_clipIds; ///< per track, per child (0 = not a clip)
    std::vector<PendingTransition> m_transitions;
};

} // namespace

TimelineImport importOtio(const QByteArray& json, const ImportOptions& options)
{
    TimelineImport result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        result.error = QCoreApplication::translate("kk::Otio", "Not a JSON file: %1")
                           .arg(parseError.errorString());
        return result;
    }
    QJsonObject root = doc.object();
    QString schema = schemaName(root);
    QStringList extra;
    if (schema == QLatin1String("SerializableCollection")) {
        // A collection of timelines: the first one is imported.
        const QJsonArray children = root.value(QLatin1String("children")).toArray();
        QJsonObject first;
        int timelines = 0;
        for (const QJsonValue& c : children) {
            if (schemaName(c.toObject()) == QLatin1String("Timeline") && timelines++ == 0)
                first = c.toObject();
        }
        if (timelines > 1)
            extra
                << QCoreApplication::translate("kk::Otio", "%1 more timelines").arg(timelines - 1);
        root = first;
        schema = schemaName(root);
    }
    if (schema != QLatin1String("Timeline")) {
        result.error = QCoreApplication::translate(
            "kk::Otio", "This is not an OpenTimelineIO timeline (found \"%1\").")
                           .arg(root.value(QLatin1String("OTIO_SCHEMA")).toString());
        return result;
    }
    if (!options.resolveMedia) {
        result.error
            = QCoreApplication::translate("kk::Otio", "No way to find the media was given.");
        return result;
    }
    Reader reader(options, result);
    result.ok = reader.run(root);
    result.skipped = extra + result.skipped;
    return result;
}

} // namespace kk::otio
