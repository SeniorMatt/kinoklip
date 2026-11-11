// SPDX-License-Identifier: GPL-3.0-or-later
// WavWriter — writes a PCM WAV file from float samples, growing as samples arrive (voice-over
// takes, spec 75). The sizes in the header are patched when the file is closed.
#pragma once

#include <QFile>
#include <QString>

#include <cstdint>

namespace kk::media {

/// Streams interleaved float samples (-1..1) into a RIFF/WAVE file as 16- or 24-bit
/// integer PCM (values beyond +-1 are clamped; no dithering). Mono, stereo or more channels
/// (the plain PCM header is used for every channel count).
///
/// Thread-safety: use from one thread.
class WavWriter
{
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /// Creates @p path (replacing a file there). @p bits is 16 or 24. False and error() on failure.
    bool open(const QString& path, int sampleRate, int channels, int bits);
    /// Appends @p frames frames of interleaved samples. False after a write error.
    bool write(const float* samples, int64_t frames);
    /// Patches the header and closes the file. False if the file could not be finished. Safe to
    /// call twice; the destructor calls it.
    bool close();

    [[nodiscard]] bool isOpen() const { return m_file.isOpen(); }
    [[nodiscard]] int64_t framesWritten() const { return m_frames; }
    [[nodiscard]] QString error() const { return m_error; }

private:
    QFile m_file;
    int m_rate = 48000;
    int m_channels = 1;
    int m_bits = 16;
    int64_t m_frames = 0;
    QString m_error;
};

} // namespace kk::media
