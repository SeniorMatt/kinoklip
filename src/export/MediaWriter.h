// SPDX-License-Identifier: GPL-3.0-or-later
// MediaWriter — internal: a libavformat output (file or image-sequence pattern) with
// interleaved packet writing, timestamp rescaling and MP4/MOV faststart.
#pragma once

#include "export/ExportFFmpeg.h"

#include <QString>
#include <QStringList>

namespace kk::exporter {

/// One output file. Usage: open() → addStream() per encoder → writeHeader() → write()... →
/// finish(); abort() on errors/cancel (closes without a trailer; the caller deletes the file).
///
/// Interleaving: av_interleaved_write_frame() buffers packets and writes them in dts order
/// across streams, so the caller may produce audio and video in any reasonable chunking (the
/// exporter alternates one video frame and the matching audio).
///
/// Thread-safety: not thread-safe; use from one thread.
class MediaWriter
{
public:
    MediaWriter();
    ~MediaWriter();
    MediaWriter(const MediaWriter&) = delete;
    MediaWriter& operator=(const MediaWriter&) = delete;

    /// Allocates the output for muxer @p muxer at @p path (a "%05d" pattern for image2).
    [[nodiscard]] bool open(const QString& muxer, const QString& path, QString* error);
    /// True if encoders must put codec headers into extradata (AV_CODEC_FLAG_GLOBAL_HEADER).
    [[nodiscard]] bool needsGlobalHeader() const;
    /// Adds a stream with the parameters of the opened encoder @p encoder. @return its index.
    [[nodiscard]] int addStream(const AVCodecContext* encoder, QString* error);
    /// Opens the file and writes the header. @p faststart: moov atom at the front (second
    /// pass at finish(), MP4/MOV/M4A only). @p startNumber: first number of image sequences.
    [[nodiscard]] bool writeHeader(bool faststart, int startNumber, QString* error);
    /// Writes one packet of stream @p streamIndex; timestamps are in @p encoderTimeBase.
    [[nodiscard]] bool write(
        AVPacket* packet, int streamIndex, AVRational encoderTimeBase, QString* error);
    /// Flushes interleaving queues, writes the trailer and closes the file.
    [[nodiscard]] bool finish(QString* error);
    /// Closes without a trailer (cancel / error). Safe to call more than once.
    void abort();

    /// Bytes written so far (0 for image sequences).
    [[nodiscard]] int64_t bytesWritten() const;

private:
    AVFormatContext* m_fmt = nullptr;
    bool m_headerWritten = false;
    bool m_ioOpen = false;
};

} // namespace kk::exporter
