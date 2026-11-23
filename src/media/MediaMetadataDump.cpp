// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/MediaMetadataDump.h"

#include "media/FFmpegUtil.h"

#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>

namespace kk::media {

namespace {

using Rows = QVector<MetadataEntry>;

QString text(const char* s)
{
    return s ? QString::fromUtf8(s) : QString();
}

QString number(double v, int digits = 6)
{
    return QString::number(v, 'f', digits);
}

QString rational(AVRational r)
{
    return r.den != 0 ? QStringLiteral("%1/%2").arg(r.num).arg(r.den) : QStringLiteral("%1/0").arg(r.num);
}

/// A timestamp @p ts of @p base in seconds; empty when unset.
QString seconds(int64_t ts, AVRational base)
{
    if (ts == AV_NOPTS_VALUE || base.den == 0)
        return {};
    return number(double(ts) * av_q2d(base));
}

void add(Rows& rows, const QString& group, const QString& key, const QString& value)
{
    if (!value.isEmpty())
        rows.append({group, key, value});
}

void addTags(Rows& rows, const QString& group, const AVDictionary* dict)
{
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(dict, e)))
        rows.append({group, text(e->key), text(e->value)});
}

QString dispositionText(int flags)
{
    static const struct
    {
        int bit;
        const char* name;
    } kFlags[] = {{AV_DISPOSITION_DEFAULT, "default"}, {AV_DISPOSITION_DUB, "dub"},
        {AV_DISPOSITION_ORIGINAL, "original"}, {AV_DISPOSITION_COMMENT, "comment"},
        {AV_DISPOSITION_LYRICS, "lyrics"}, {AV_DISPOSITION_KARAOKE, "karaoke"},
        {AV_DISPOSITION_FORCED, "forced"}, {AV_DISPOSITION_HEARING_IMPAIRED, "hearing_impaired"},
        {AV_DISPOSITION_VISUAL_IMPAIRED, "visual_impaired"},
        {AV_DISPOSITION_CLEAN_EFFECTS, "clean_effects"},
        {AV_DISPOSITION_ATTACHED_PIC, "attached_pic"},
        {AV_DISPOSITION_TIMED_THUMBNAILS, "timed_thumbnails"},
        {AV_DISPOSITION_CAPTIONS, "captions"}, {AV_DISPOSITION_DESCRIPTIONS, "descriptions"},
        {AV_DISPOSITION_METADATA, "metadata"}};
    QStringList names;
    for (const auto& f : kFlags) {
        if (flags & f.bit)
            names.append(QLatin1String(f.name));
    }
    return names.join(QStringLiteral(", "));
}

QString fieldOrderText(AVFieldOrder order)
{
    switch (order) {
    case AV_FIELD_PROGRESSIVE:
        return QStringLiteral("progressive");
    case AV_FIELD_TT:
        return QStringLiteral("tt (top coded first, top displayed first)");
    case AV_FIELD_BB:
        return QStringLiteral("bb (bottom coded first, bottom displayed first)");
    case AV_FIELD_TB:
        return QStringLiteral("tb (top coded first, bottom displayed first)");
    case AV_FIELD_BT:
        return QStringLiteral("bt (bottom coded first, top displayed first)");
    default:
        return {};
    }
}

/// One side data entry of a stream as "name : value".
void addSideData(Rows& rows, const QString& group, const AVPacketSideData& sd)
{
    const QString name = text(av_packet_side_data_name(sd.type));
    const QString key = name.isEmpty() ? QStringLiteral("side data %1").arg(int(sd.type)) : name;
    QString value;
    switch (sd.type) {
    case AV_PKT_DATA_DISPLAYMATRIX:
        if (sd.size >= 9 * sizeof(int32_t)) {
            const double rotation = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd.data));
            value = QStringLiteral("rotation of %1 degrees").arg(
                std::isnan(rotation) ? QStringLiteral("unknown") : number(rotation, 2));
        }
        break;
    case AV_PKT_DATA_MASTERING_DISPLAY_METADATA:
        if (sd.size >= sizeof(AVMasteringDisplayMetadata)) {
            const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd.data);
            QStringList parts;
            if (m->has_primaries) {
                const char* names[3] = {"R", "G", "B"};
                for (int i = 0; i < 3; ++i)
                    parts << QStringLiteral("%1(%2, %3)")
                                 .arg(QLatin1String(names[i]))
                                 .arg(number(av_q2d(m->display_primaries[i][0]), 4))
                                 .arg(number(av_q2d(m->display_primaries[i][1]), 4));
                parts << QStringLiteral("WP(%1, %2)")
                             .arg(number(av_q2d(m->white_point[0]), 4))
                             .arg(number(av_q2d(m->white_point[1]), 4));
            }
            if (m->has_luminance)
                parts << QStringLiteral("min_luminance %1, max_luminance %2")
                             .arg(number(av_q2d(m->min_luminance), 4))
                             .arg(number(av_q2d(m->max_luminance), 2));
            value = parts.join(QStringLiteral(", "));
        }
        break;
    case AV_PKT_DATA_CONTENT_LIGHT_LEVEL:
        if (sd.size >= sizeof(AVContentLightMetadata)) {
            const auto* c = reinterpret_cast<const AVContentLightMetadata*>(sd.data);
            value = QStringLiteral("MaxCLL %1, MaxFALL %2").arg(c->MaxCLL).arg(c->MaxFALL);
        }
        break;
    default:
        break;
    }
    if (value.isEmpty())
        value = QStringLiteral("%1 bytes").arg(qulonglong(sd.size));
    rows.append({group, key, value});
}

void addStream(Rows& rows, const AVStream* st)
{
    const AVCodecParameters* par = st->codecpar;
    const char* type = av_get_media_type_string(par->codec_type);
    const QString typeName = type ? text(type) : QStringLiteral("unknown");
    const QString g = QStringLiteral("%1 stream #%2")
                          .arg(typeName.left(1).toUpper() + typeName.mid(1))
                          .arg(st->index);
    add(rows, g, QStringLiteral("index"), QString::number(st->index));
    add(rows, g, QStringLiteral("codec_type"), typeName);
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    add(rows, g, QStringLiteral("codec_name"), desc ? text(desc->name) : QStringLiteral("unknown"));
    if (desc)
        add(rows, g, QStringLiteral("codec_long_name"), text(desc->long_name));
    add(rows, g, QStringLiteral("profile"), text(avcodec_profile_name(par->codec_id, par->profile)));
    char tagString[AV_FOURCC_MAX_STRING_SIZE] = {};
    if (par->codec_tag)
        add(rows, g, QStringLiteral("codec_tag"),
            QStringLiteral("%1 (0x%2)")
                .arg(text(av_fourcc_make_string(tagString, par->codec_tag)))
                .arg(par->codec_tag, 8, 16, QLatin1Char('0')));
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        add(rows, g, QStringLiteral("width"), QString::number(par->width));
        add(rows, g, QStringLiteral("height"), QString::number(par->height));
        if (par->level > 0)
            add(rows, g, QStringLiteral("level"), QString::number(par->level));
        add(rows, g, QStringLiteral("pix_fmt"),
            text(av_get_pix_fmt_name(AVPixelFormat(par->format))));
        if (par->bits_per_raw_sample > 0)
            add(rows, g, QStringLiteral("bits_per_raw_sample"),
                QString::number(par->bits_per_raw_sample));
        add(rows, g, QStringLiteral("color_range"), text(av_color_range_name(par->color_range)));
        add(rows, g, QStringLiteral("color_space"), text(av_color_space_name(par->color_space)));
        add(rows, g, QStringLiteral("color_transfer"),
            text(av_color_transfer_name(par->color_trc)));
        add(rows, g, QStringLiteral("color_primaries"),
            text(av_color_primaries_name(par->color_primaries)));
        add(rows, g, QStringLiteral("chroma_location"),
            text(av_chroma_location_name(par->chroma_location)));
        add(rows, g, QStringLiteral("field_order"), fieldOrderText(par->field_order));
        if (par->sample_aspect_ratio.num > 0)
            add(rows, g, QStringLiteral("sample_aspect_ratio"), rational(par->sample_aspect_ratio));
        if (par->video_delay > 0)
            add(rows, g, QStringLiteral("has_b_frames"), QString::number(par->video_delay));
        if (st->avg_frame_rate.num > 0)
            add(rows, g, QStringLiteral("avg_frame_rate"),
                QStringLiteral("%1 (%2 fps)")
                    .arg(rational(st->avg_frame_rate))
                    .arg(number(av_q2d(st->avg_frame_rate), 3)));
        if (st->r_frame_rate.num > 0)
            add(rows, g, QStringLiteral("r_frame_rate"), rational(st->r_frame_rate));
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        add(rows, g, QStringLiteral("sample_fmt"),
            text(av_get_sample_fmt_name(AVSampleFormat(par->format))));
        add(rows, g, QStringLiteral("sample_rate"), QString::number(par->sample_rate));
        add(rows, g, QStringLiteral("channels"), QString::number(par->ch_layout.nb_channels));
        char layout[128] = {};
        if (par->ch_layout.nb_channels > 0
            && av_channel_layout_describe(&par->ch_layout, layout, sizeof layout) > 0)
            add(rows, g, QStringLiteral("channel_layout"), text(layout));
        if (par->bits_per_coded_sample > 0)
            add(rows, g, QStringLiteral("bits_per_sample"),
                QString::number(par->bits_per_coded_sample));
    }
    if (par->bit_rate > 0)
        add(rows, g, QStringLiteral("bit_rate"), QString::number(qlonglong(par->bit_rate)));
    add(rows, g, QStringLiteral("time_base"), rational(st->time_base));
    add(rows, g, QStringLiteral("start_time"), seconds(st->start_time, st->time_base));
    add(rows, g, QStringLiteral("duration"), seconds(st->duration, st->time_base));
    if (st->nb_frames > 0)
        add(rows, g, QStringLiteral("nb_frames"), QString::number(qlonglong(st->nb_frames)));
    add(rows, g, QStringLiteral("disposition"), dispositionText(st->disposition));
    addTags(rows, g + QStringLiteral(" tags"), st->metadata);
    for (int i = 0; i < par->nb_coded_side_data; ++i)
        addSideData(rows, g + QStringLiteral(" side data"), par->coded_side_data[i]);
}

} // namespace

QVector<MetadataEntry> readAllMetadata(const QString& path, QString* error)
{
    Rows rows;
    detail::FormatContextPtr fmt = detail::openInput(path, error);
    if (!fmt)
        return rows;
    const QString c = QStringLiteral("Container");
    add(rows, c, QStringLiteral("filename"), QFileInfo(path).fileName());
    if (fmt->iformat) {
        add(rows, c, QStringLiteral("format_name"), text(fmt->iformat->name));
        add(rows, c, QStringLiteral("format_long_name"), text(fmt->iformat->long_name));
    }
    add(rows, c, QStringLiteral("nb_streams"), QString::number(fmt->nb_streams));
    add(rows, c, QStringLiteral("start_time"),
        fmt->start_time == AV_NOPTS_VALUE ? QString() : number(double(fmt->start_time) / AV_TIME_BASE));
    add(rows, c, QStringLiteral("duration"),
        fmt->duration == AV_NOPTS_VALUE ? QString() : number(double(fmt->duration) / AV_TIME_BASE));
    if (fmt->bit_rate > 0)
        add(rows, c, QStringLiteral("bit_rate"), QString::number(qlonglong(fmt->bit_rate)));
    add(rows, c, QStringLiteral("probe_score"), QString::number(fmt->probe_score));
    addTags(rows, QStringLiteral("Container tags"), fmt->metadata);
    for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
        const AVChapter* ch = fmt->chapters[i];
        const QString g = QStringLiteral("Chapter %1").arg(i + 1);
        add(rows, g, QStringLiteral("start"), seconds(ch->start, ch->time_base));
        add(rows, g, QStringLiteral("end"), seconds(ch->end, ch->time_base));
        addTags(rows, g, ch->metadata);
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        addStream(rows, fmt->streams[i]);
    return rows;
}

QVector<MetadataEntry> filterMetadata(const QVector<MetadataEntry>& rows, const QString& filter)
{
    const QStringList words = filter.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return rows;
    Rows out;
    for (const MetadataEntry& e : rows) {
        const QString hay = (e.group + QLatin1Char(' ') + e.key + QLatin1Char(' ') + e.value).toLower();
        bool all = true;
        for (const QString& w : words)
            all = all && hay.contains(w);
        if (all)
            out.append(e);
    }
    return out;
}

QString metadataToText(const QVector<MetadataEntry>& rows)
{
    QString out;
    QString group;
    for (const MetadataEntry& e : rows) {
        if (e.group != group) {
            if (!out.isEmpty())
                out += QLatin1Char('\n');
            out += QLatin1Char('[') + e.group + QStringLiteral("]\n");
            group = e.group;
        }
        out += e.key + QStringLiteral(" : ") + e.value + QLatin1Char('\n');
    }
    return out;
}

} // namespace kk::media
