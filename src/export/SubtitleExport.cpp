// SPDX-License-Identifier: GPL-3.0-or-later
// SubtitleExport — Deliver's subtitle options, sidecar files and the text subtitle encoder.
#include "export/SubtitleExport.h"

#include "core/TimelineData.h"
#include "export/ExportFFmpeg.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstring>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::SubtitleExport", text);
}

/// Largest encoded caption (a caption is a few lines of text; mov_text adds a small box).
constexpr int kMaxSubtitlePacket = 64 * 1024;

/// ASS header given to every text subtitle encoder (mov_text, subrip and webvtt convert from
/// ASS events; the style matters only for "ass"). Bottom centre, white with a black outline.
QByteArray assHeader(int width, int height)
{
    return QStringLiteral("[Script Info]\r\n"
                          "ScriptType: v4.00+\r\n"
                          "PlayResX: %1\r\n"
                          "PlayResY: %2\r\n"
                          "ScaledBorderAndShadow: yes\r\n"
                          "\r\n"
                          "[V4+ Styles]\r\n"
                          "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                          "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
                          "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
                          "MarginL, MarginR, MarginV, Encoding\r\n"
                          "Style: Default,Sans,%3,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,0,0,"
                          "0,0,100,100,0,0,1,%4,1,2,%5,%5,%6,1\r\n"
                          "\r\n"
                          "[Events]\r\n"
                          "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                          "Effect, Text\r\n")
        .arg(width)
        .arg(height)
        .arg(std::max(8, height * 52 / 1080))
        .arg(std::max(1, height * 2 / 1080))
        .arg(width * 8 / 100)
        .arg(height * 7 / 100)
        .toUtf8();
}

/// ASS dialogue event body ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text")
/// of a caption: newlines become \N, braces (override blocks) are neutralised.
QByteArray assEvent(const QString& text, int readOrder)
{
    QString body = text;
    body.replace(QLatin1Char('{'), QLatin1Char('('));
    body.replace(QLatin1Char('}'), QLatin1Char(')'));
    body.replace(QLatin1Char('\n'), QLatin1String("\\N"));
    return QStringLiteral("%1,0,Default,,0,0,0,,%2").arg(readOrder).arg(body).toUtf8();
}

} // namespace

// ---- Settings ----

QString subtitleModeId(SubtitleExportMode mode)
{
    switch (mode) {
    case SubtitleExportMode::BurnIn:
        return QStringLiteral("burn_in");
    case SubtitleExportMode::Embedded:
        return QStringLiteral("embedded");
    case SubtitleExportMode::Sidecar:
        break;
    }
    return QStringLiteral("sidecar");
}

SubtitleExportMode subtitleModeFromId(const QString& id)
{
    if (id == QLatin1String("burn_in"))
        return SubtitleExportMode::BurnIn;
    if (id == QLatin1String("embedded"))
        return SubtitleExportMode::Embedded;
    return SubtitleExportMode::Sidecar;
}

QJsonObject SubtitleExportSettings::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("enabled")] = enabled;
    o[QStringLiteral("mode")] = subtitleModeId(mode);
    o[QStringLiteral("format")] = subtitles::suffix(sidecarFormat);
    if (!embeddedCodec.isEmpty())
        o[QStringLiteral("codec")] = embeddedCodec;
    return o;
}

SubtitleExportSettings SubtitleExportSettings::fromJson(
    const QJsonObject& o, const SubtitleExportSettings& base)
{
    SubtitleExportSettings s = base;
    s.enabled = o.value(QStringLiteral("enabled")).toBool(base.enabled);
    if (o.contains(QStringLiteral("mode")))
        s.mode = subtitleModeFromId(o.value(QStringLiteral("mode")).toString());
    if (o.contains(QStringLiteral("format")))
        s.sidecarFormat = o.value(QStringLiteral("format")).toString() == QLatin1String("vtt")
            ? SubtitleFormat::WebVtt
            : SubtitleFormat::Srt;
    s.embeddedCodec = o.value(QStringLiteral("codec")).toString(base.embeddedCodec);
    return s;
}

QStringList subtitleCodecs(const QString& containerId)
{
    if (containerId == QLatin1String("mp4") || containerId == QLatin1String("mov"))
        return {QStringLiteral("mov_text")};
    if (containerId == QLatin1String("mkv"))
        return {QStringLiteral("subrip"), QStringLiteral("ass")};
    if (containerId == QLatin1String("webm"))
        return {QStringLiteral("webvtt")};
    return {};
}

QString subtitleCodecName(const QString& codec)
{
    if (codec == QLatin1String("mov_text"))
        return tr("MP4 Timed Text");
    if (codec == QLatin1String("subrip"))
        return tr("SubRip (SRT)");
    if (codec == QLatin1String("ass"))
        return tr("Advanced SubStation (ASS)");
    if (codec == QLatin1String("webvtt"))
        return tr("WebVTT");
    return codec;
}

QString containerLanguage(const QString& language, const QString& containerId)
{
    if (containerId != QLatin1String("mov"))
        return language;
    // The ISO 639-2 languages whose bibliographic (B) code differs from the terminology (T)
    // code; QuickTime's language table uses the B codes.
    static const char* const kTerminologyToBibliographic[][2]
        = {{"sqi", "alb"}, {"hye", "arm"}, {"eus", "baq"}, {"mya", "bur"}, {"zho", "chi"},
            {"ces", "cze"}, {"nld", "dut"}, {"fra", "fre"}, {"kat", "geo"}, {"deu", "ger"},
            {"ell", "gre"}, {"isl", "ice"}, {"mkd", "mac"}, {"mri", "mao"}, {"msa", "may"},
            {"fas", "per"}, {"ron", "rum"}, {"slk", "slo"}, {"bod", "tib"}, {"cym", "wel"}};
    const QString lower = language.toLower();
    for (const auto& pair : kTerminologyToBibliographic) {
        if (lower == QLatin1String(pair[0]))
            return QLatin1String(pair[1]);
    }
    return language;
}

// ---- Collecting and sidecars ----

QVector<SubtitleTrackExport> collectSubtitleTracks(
    const TimelineData& timeline, int64_t first, int64_t end)
{
    QVector<SubtitleTrackExport> out;
    for (size_t i = 0; i < timeline.subtitleTracks.size(); ++i) {
        const TrackData& t = timeline.subtitleTracks[i];
        if (!t.enabled)
            continue;
        SubtitleTrackExport e;
        e.name = trackDisplayName(t, int(i));
        e.language = t.language;
        e.cues = subtitles::cuesFromTrack(t, timeline.fps, first, end);
        if (!e.cues.isEmpty())
            out.append(std::move(e));
    }
    return out;
}

QString sidecarPath(const QString& outputPath, const SubtitleTrackExport& track, int index,
    int count, SubtitleFormat format)
{
    const QFileInfo info(outputPath);
    QString base = info.dir().filePath(info.completeBaseName());
    if (count > 1)
        base += QLatin1Char('.')
            + (track.language.isEmpty() ? QString::number(index + 1) : track.language);
    return base + QLatin1Char('.') + subtitles::suffix(format);
}

bool writeSidecars(const QString& outputPath, const QVector<SubtitleTrackExport>& tracks,
    SubtitleFormat format, QStringList* files, QString* error)
{
    for (int i = 0; i < tracks.size(); ++i) {
        const QString path = sidecarPath(outputPath, tracks[i], i, int(tracks.size()), format);
        QString why;
        if (!subtitles::writeFile(path, tracks[i].cues, format, &why)) {
            if (error)
                *error = tr("Cannot write %1: %2").arg(path, why);
            return false;
        }
        if (files)
            files->append(path);
    }
    return true;
}

// ---- Encoder ----

namespace exporter {

struct SubtitleEncoder::Impl
{
    CodecContextPtr context;
    std::vector<uint8_t> buffer; ///< encode target, reused for every cue
    int readOrder = 0;
};

SubtitleEncoder::SubtitleEncoder()
    : m_impl(std::make_unique<Impl>())
{ }

SubtitleEncoder::~SubtitleEncoder() = default;

bool SubtitleEncoder::open(
    const QString& codec, int width, int height, bool globalHeader, QString* error)
{
    initExportFfmpeg();
    const AVCodec* encoder = avcodec_find_encoder_by_name(codec.toLatin1().constData());
    if (!encoder || encoder->type != AVMEDIA_TYPE_SUBTITLE) {
        if (error)
            *error = tr("The subtitle encoder %1 is not available.").arg(codec);
        return false;
    }
    m_impl->context.reset(avcodec_alloc_context3(encoder));
    AVCodecContext* ctx = m_impl->context.get();
    ctx->time_base = AVRational {1, 1000};
    ctx->width = width;
    ctx->height = height;
    if (globalHeader)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // The encoders read the style from an ASS header (mov_text needs it for its sample
    // description); FFmpeg frees it with the context.
    const QByteArray header = assHeader(width, height);
    ctx->subtitle_header = static_cast<uint8_t*>(av_mallocz(size_t(header.size()) + 1));
    std::memcpy(ctx->subtitle_header, header.constData(), size_t(header.size()));
    ctx->subtitle_header_size = int(header.size());
    if (int err = avcodec_open2(ctx, encoder, nullptr); err < 0) {
        if (error)
            *error = tr("Cannot open the subtitle encoder %1: %2").arg(codec, avError(err));
        m_impl->context.reset();
        return false;
    }
    m_impl->buffer.resize(kMaxSubtitlePacket);
    return true;
}

AVCodecContext* SubtitleEncoder::context() const
{
    return m_impl->context.get();
}

bool SubtitleEncoder::encode(
    const SubtitleCue& cue, const std::function<bool(AVPacket*)>& sink, QString* error)
{
    AVCodecContext* ctx = m_impl->context.get();
    if (!ctx)
        return false;
    const int64_t startMs = (cue.startUs + 500) / 1000;
    const int64_t endMs = std::max(startMs + 1, (cue.endUs + 500) / 1000);
    AVSubtitle sub {};
    sub.format = 1; // text
    sub.start_display_time = 0;
    sub.end_display_time = uint32_t(endMs - startMs);
    sub.pts = startMs * 1000; // AV_TIME_BASE (µs)
    sub.num_rects = 1;
    sub.rects = static_cast<AVSubtitleRect**>(av_mallocz(sizeof(AVSubtitleRect*)));
    sub.rects[0] = static_cast<AVSubtitleRect*>(av_mallocz(sizeof(AVSubtitleRect)));
    sub.rects[0]->type = SUBTITLE_ASS;
    sub.rects[0]->ass = av_strdup(assEvent(cue.text, m_impl->readOrder++).constData());
    const int size
        = avcodec_encode_subtitle(ctx, m_impl->buffer.data(), int(m_impl->buffer.size()), &sub);
    avsubtitle_free(&sub);
    if (size < 0) {
        if (error)
            *error = tr("Subtitle encoding failed: %1").arg(avError(size));
        return false;
    }
    PacketPtr packet(av_packet_alloc());
    if (av_new_packet(packet.get(), size) < 0)
        return false;
    std::memcpy(packet->data, m_impl->buffer.data(), size_t(size));
    packet->pts = startMs;
    packet->dts = startMs;
    packet->duration = endMs - startMs;
    packet->flags |= AV_PKT_FLAG_KEY;
    return sink(packet.get());
}

} // namespace exporter
} // namespace kk
