// SPDX-License-Identifier: GPL-3.0-or-later
// MediaWriter — libavformat output.
#include "export/MediaWriter.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace kk::exporter {

MediaWriter::MediaWriter() = default;

MediaWriter::~MediaWriter()
{
    abort();
}

bool MediaWriter::open(const QString& muxer, const QString& path, QString* error)
{
    initExportFfmpeg();
    const QByteArray name = muxer.toLatin1();
    const QByteArray file = path.toUtf8();
    const int err
        = avformat_alloc_output_context2(&m_fmt, nullptr, name.constData(), file.constData());
    if (err < 0 || !m_fmt) {
        if (error) {
            *error = QStringLiteral("cannot create %1 output: %2").arg(muxer, avError(err));
        }
        return false;
    }
    return true;
}

bool MediaWriter::needsGlobalHeader() const
{
    return m_fmt && (m_fmt->oformat->flags & AVFMT_GLOBALHEADER);
}

int MediaWriter::addStream(const AVCodecContext* encoder, QString* error)
{
    AVStream* stream = avformat_new_stream(m_fmt, nullptr);
    if (!stream) {
        if (error) {
            *error = QStringLiteral("cannot add a stream");
        }
        return -1;
    }
    if (int err = avcodec_parameters_from_context(stream->codecpar, encoder); err < 0) {
        if (error) {
            *error = QStringLiteral("stream parameters: %1").arg(avError(err));
        }
        return -1;
    }
    stream->time_base = encoder->time_base; // a hint; the muxer may change it in writeHeader
    if (encoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        stream->avg_frame_rate = encoder->framerate;
        stream->r_frame_rate = encoder->framerate;
        stream->sample_aspect_ratio = encoder->sample_aspect_ratio;
    }
    return stream->index;
}

bool MediaWriter::writeHeader(bool faststart, int startNumber, QString* error)
{
    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (int err = avio_open(&m_fmt->pb, m_fmt->url, AVIO_FLAG_WRITE); err < 0) {
            if (error) {
                *error = QStringLiteral("cannot write %1: %2")
                             .arg(QString::fromUtf8(m_fmt->url), avError(err));
            }
            return false;
        }
        m_ioOpen = true;
    }
    AVDictionary* opts = nullptr;
    if (faststart) {
        av_dict_set(&opts, "movflags", "+faststart", 0);
    }
    if (QLatin1String(m_fmt->oformat->name) == QLatin1String("image2")) {
        av_dict_set_int(&opts, "start_number", startNumber, 0);
    }
    if (QLatin1String(m_fmt->oformat->name) == QLatin1String("gif")) {
        av_dict_set_int(&opts, "loop", 0, 0); // loop forever, like every web GIF
    }
    const int err = avformat_write_header(m_fmt, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        if (error) {
            *error = QStringLiteral("cannot write the file header: %1").arg(avError(err));
        }
        return false;
    }
    m_headerWritten = true;
    return true;
}

bool MediaWriter::write(
    AVPacket* packet, int streamIndex, AVRational encoderTimeBase, QString* error)
{
    AVStream* stream = m_fmt->streams[streamIndex];
    packet->stream_index = streamIndex;
    av_packet_rescale_ts(packet, encoderTimeBase, stream->time_base);
    // av_interleaved_write_frame takes ownership of the packet's reference (and blanks it).
    if (int err = av_interleaved_write_frame(m_fmt, packet); err < 0) {
        if (error) {
            *error = QStringLiteral("cannot write to the output file: %1").arg(avError(err));
        }
        return false;
    }
    return true;
}

bool MediaWriter::finish(QString* error)
{
    if (!m_fmt) {
        return false;
    }
    int err = 0;
    if (m_headerWritten) {
        err = av_write_trailer(m_fmt);
    }
    if (m_ioOpen) {
        avio_closep(&m_fmt->pb);
        m_ioOpen = false;
    }
    avformat_free_context(m_fmt);
    m_fmt = nullptr;
    if (err < 0) {
        if (error) {
            *error = QStringLiteral("cannot finish the output file: %1").arg(avError(err));
        }
        return false;
    }
    return true;
}

void MediaWriter::abort()
{
    if (!m_fmt) {
        return;
    }
    if (m_ioOpen) {
        avio_closep(&m_fmt->pb);
        m_ioOpen = false;
    }
    avformat_free_context(m_fmt);
    m_fmt = nullptr;
}

int64_t MediaWriter::bytesWritten() const
{
    return m_fmt && m_fmt->pb ? avio_tell(m_fmt->pb) : 0;
}

} // namespace kk::exporter
