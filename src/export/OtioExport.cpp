// SPDX-License-Identifier: GPL-3.0-or-later
// OtioExport — writes a timeline as an OpenTimelineIO (.otio) JSON file.
#include "core/MediaPool.h"
#include "core/Serialization.h"
#include "export/Otio.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace kk::otio {

namespace {

/// Rounding of written time values: a millionth of a frame is far below one microsecond
/// at any frame rate, and it keeps the file free of floating point noise (0.1 + 0.2 ...).
constexpr double kValueGrain = 1e6;

/// Gap, clip and track writer for one timeline; all times are at the timeline rate.
class Writer
{
public:
    Writer(const TimelineData& data, const MediaPool* pool, const ExportOptions& options)
        : m_data(data)
        , m_pool(pool)
        , m_options(options)
        , m_rate(data.fps.isValid() ? data.fps.toDouble() : 25.0)
    { }

    QJsonObject timeline() const
    {
        QJsonArray children;
        for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
            const auto& tracks = m_data.tracks(type);
            for (size_t i = 0; i < tracks.size(); ++i)
                children.append(track(tracks[i], type, int(i)));
        }
        QJsonObject stack = object(QStringLiteral("Stack.1"), QStringLiteral("tracks"));
        stack[QStringLiteral("children")] = children;
        stack[QStringLiteral("effects")] = QJsonArray();
        stack[QStringLiteral("enabled")] = true;
        stack[QStringLiteral("source_range")] = QJsonValue::Null;
        QJsonArray markers;
        for (const Marker& m : m_data.markers) {
            // Timeline markers carry the record timecode, like the EDL marker list.
            markers.append(marker(m, double(m.frame + m_data.startTimecode), double(m.duration)));
        }
        stack[QStringLiteral("markers")] = markers;

        QJsonObject root = object(QStringLiteral("Timeline.1"), m_data.name);
        root[QStringLiteral("global_start_time")] = time(double(m_data.startTimecode));
        root[QStringLiteral("tracks")] = stack;
        return root;
    }

private:
    static QJsonObject object(const QString& schema, const QString& name)
    {
        QJsonObject o;
        o[QStringLiteral("OTIO_SCHEMA")] = schema;
        o[QStringLiteral("metadata")] = QJsonObject();
        o[QStringLiteral("name")] = name;
        return o;
    }

    /// RationalTime.1 of @p value frames (fractions allowed) at the timeline rate.
    QJsonObject time(double value) const
    {
        QJsonObject o;
        o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("RationalTime.1");
        o[QStringLiteral("rate")] = m_rate;
        o[QStringLiteral("value")] = std::round(value * kValueGrain) / kValueGrain;
        return o;
    }
    /// The same for @p us microseconds.
    QJsonObject timeUs(int64_t us) const { return time(double(us) * m_rate / 1e6); }

    QJsonObject range(const QJsonObject& start, const QJsonObject& duration) const
    {
        QJsonObject o;
        o[QStringLiteral("OTIO_SCHEMA")] = QStringLiteral("TimeRange.1");
        o[QStringLiteral("start_time")] = start;
        o[QStringLiteral("duration")] = duration;
        return o;
    }

    /// Marker.2 starting at @p startValue frames, @p durationValue frames long.
    QJsonObject marker(const Marker& m, double startValue, double durationValue) const
    {
        QJsonObject o = object(QStringLiteral("Marker.2"), m.name);
        o[QStringLiteral("color")] = markerColorToOtio(m.color);
        o[QStringLiteral("comment")] = m.note;
        o[QStringLiteral("marked_range")] = range(time(startValue), time(durationValue));
        // OTIO has eleven colours; the exact one comes back through the metadata.
        o[QStringLiteral("metadata")] = QJsonObject {{QStringLiteral("kinoklip"),
            QJsonObject {{QStringLiteral("color"), m.color.name(QColor::HexRgb)}}}};
        return o;
    }

    QJsonObject gap(int64_t frames) const
    {
        QJsonObject o = object(QStringLiteral("Gap.1"), QString());
        o[QStringLiteral("source_range")] = range(time(0), time(double(frames)));
        o[QStringLiteral("effects")] = QJsonArray();
        o[QStringLiteral("markers")] = QJsonArray();
        o[QStringLiteral("enabled")] = true;
        return o;
    }

    QJsonObject transition(const TransitionData& t) const
    {
        const bool dissolve = t.type == QLatin1String("cross_dissolve");
        QJsonObject o = object(
            QStringLiteral("Transition.1"), dissolve ? QStringLiteral("Cross Dissolve") : t.type);
        const int64_t before = t.framesBeforeCut();
        o[QStringLiteral("in_offset")] = time(double(before));
        o[QStringLiteral("out_offset")] = time(double(t.duration - before));
        o[QStringLiteral("transition_type")]
            = dissolve ? QStringLiteral("SMPTE_Dissolve") : QStringLiteral("Custom_Transition");
        // The Kinoklip id and the parameters travel in the metadata, so another Kinoklip reads
        // the very transition back; a plain cross dissolve with no parameters needs none.
        if (!dissolve || !t.params.values.isEmpty() || !t.params.statics.isEmpty()) {
            o[QStringLiteral("metadata")] = QJsonObject {{QStringLiteral("kinoklip"),
                QJsonObject {{QStringLiteral("type"), t.type},
                    {QStringLiteral("params"), serialization::paramSetToJson(t.params)}}}};
        }
        return o;
    }

    int64_t startOffsetUs(const ClipData& c) const
    {
        return m_options.sourceStartUs ? m_options.sourceStartUs(c) : int64_t(0);
    }

    static QString kindName(ClipKind kind)
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
        return {};
    }

    QJsonObject mediaReference(const ClipData& c) const
    {
        const MediaItem* item
            = c.kind == ClipKind::Media && m_pool ? m_pool->item(c.mediaId) : nullptr;
        if (!item || item->path.isEmpty()) {
            QJsonObject o = object(QStringLiteral("MissingReference.1"), c.name);
            o[QStringLiteral("available_range")] = QJsonValue::Null;
            return o;
        }
        QJsonObject o = object(QStringLiteral("ExternalReference.1"), item->name);
        o[QStringLiteral("target_url")]
            = QUrl::fromLocalFile(item->path).toString(QUrl::FullyEncoded);
        const int64_t durationUs
            = item->info.durationUs > 0 ? item->info.durationUs : c.sourceDurationUs;
        // A still or a clip of unknown length has no available range.
        o[QStringLiteral("available_range")] = durationUs > 0
            ? QJsonValue(range(timeUs(startOffsetUs(c)), timeUs(durationUs)))
            : QJsonValue(QJsonValue::Null);
        o[QStringLiteral("available_image_bounds")] = QJsonValue::Null;
        return o;
    }

    QJsonObject clip(const ClipData& c) const
    {
        const Rational fps = m_data.fps;
        const bool animated = c.retime.isAnimated();
        // A ramp is written at its average speed over the clip, like the EDL export.
        const double speed = animated
            ? double(c.sourceTimeUsAt(c.end(), fps) - c.sourceTimeUsAt(c.start, fps))
                / std::max(1.0, double(framesToUs(c.duration, fps)))
            : c.speed;
        const int64_t sourceStartUs = startOffsetUs(c) + c.sourceTimeUsAt(c.start, fps);

        const MediaItem* item
            = c.kind == ClipKind::Media && m_pool ? m_pool->item(c.mediaId) : nullptr;
        const QString name = !c.name.isEmpty() ? c.name : (item ? item->name : QString());
        QJsonObject o = object(
            m_options.legacyClipSchema ? QStringLiteral("Clip.1") : QStringLiteral("Clip.2"), name);
        o[QStringLiteral("source_range")] = range(timeUs(sourceStartUs), time(double(c.duration)));
        o[QStringLiteral("enabled")] = c.enabled;

        const QJsonObject reference = mediaReference(c);
        if (m_options.legacyClipSchema) {
            o[QStringLiteral("media_reference")] = reference;
        } else {
            const QString key = QStringLiteral("DEFAULT_MEDIA");
            o[QStringLiteral("media_references")] = QJsonObject {{key, reference}};
            o[QStringLiteral("active_media_reference_key")] = key;
        }

        QJsonArray effects;
        if (std::abs(speed - 1.0) > 1e-9) {
            const bool freeze = std::abs(speed) < 1e-9;
            QJsonObject warp = object(
                freeze ? QStringLiteral("FreezeFrame.1") : QStringLiteral("LinearTimeWarp.1"),
                QString());
            warp[QStringLiteral("effect_name")]
                = freeze ? QStringLiteral("FreezeFrame") : QStringLiteral("LinearTimeWarp");
            warp[QStringLiteral("time_scalar")] = freeze ? 0.0 : speed;
            effects.append(warp);
        }
        o[QStringLiteral("effects")] = effects;

        // Clip markers sit in source time: the clip-local frame, scaled by the speed.
        QJsonArray markers;
        for (const Marker& m : c.markers) {
            const double startValue = double(sourceStartUs) * m_rate / 1e6
                + double(m.frame) * (std::abs(speed) < 1e-9 ? 0.0 : speed);
            markers.append(marker(m, startValue, double(m.duration)));
        }
        o[QStringLiteral("markers")] = markers;

        if (c.kind != ClipKind::Media) {
            // Kinoklip's own kinds have no OTIO counterpart: an offline clip that keeps its name.
            o[QStringLiteral("metadata")] = QJsonObject {{QStringLiteral("kinoklip"),
                QJsonObject {{QStringLiteral("kind"), kindName(c.kind)}}}};
        }
        return o;
    }

    QJsonObject track(const TrackData& t, TrackType type, int index) const
    {
        QJsonObject o = object(QStringLiteral("Track.1"), trackDisplayName(t, index));
        o[QStringLiteral("kind")]
            = type == TrackType::Video ? QStringLiteral("Video") : QStringLiteral("Audio");
        o[QStringLiteral("source_range")] = QJsonValue::Null;
        o[QStringLiteral("effects")] = QJsonArray();
        o[QStringLiteral("markers")] = QJsonArray();
        o[QStringLiteral("enabled")] = type == TrackType::Video ? t.enabled : !t.muted;

        QJsonArray children;
        int64_t cursor = 0;
        for (size_t i = 0; i < t.clips.size(); ++i) {
            const ClipData& c = t.clips[i];
            if (c.start > cursor)
                children.append(gap(c.start - cursor));
            // The transition of a cut sits between the two clips. It is stored on the incoming
            // clip, or (older data) on the outgoing one; fades from / to nothing are skipped.
            if (i > 0 && t.clips[i - 1].end() == c.start) {
                if (c.transitionIn)
                    children.append(transition(*c.transitionIn));
                else if (t.clips[i - 1].transitionOut)
                    children.append(transition(*t.clips[i - 1].transitionOut));
            }
            children.append(clip(c));
            cursor = c.end();
        }
        o[QStringLiteral("children")] = children;
        return o;
    }

    const TimelineData& m_data;
    const MediaPool* m_pool;
    const ExportOptions& m_options;
    double m_rate;
};

} // namespace

QByteArray timelineToOtio(
    const TimelineData& data, const MediaPool* pool, const ExportOptions& options)
{
    return QJsonDocument(Writer(data, pool, options).timeline()).toJson(QJsonDocument::Indented);
}

} // namespace kk::otio
