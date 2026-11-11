// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/WavWriter.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>

namespace kk::media {

namespace {

void put16(QByteArray& b, quint16 v)
{
    b.append(char(v & 0xff));
    b.append(char((v >> 8) & 0xff));
}

void put32(QByteArray& b, quint32 v)
{
    put16(b, quint16(v & 0xffff));
    put16(b, quint16(v >> 16));
}

QByteArray header(int rate, int channels, int bits, quint32 dataBytes)
{
    QByteArray h;
    h.append("RIFF", 4);
    put32(h, 36 + dataBytes);
    h.append("WAVEfmt ", 8);
    put32(h, 16);
    put16(h, 1); // PCM
    put16(h, quint16(channels));
    put32(h, quint32(rate));
    put32(h, quint32(rate * channels * bits / 8));
    put16(h, quint16(channels * bits / 8));
    put16(h, quint16(bits));
    h.append("data", 4);
    put32(h, dataBytes);
    return h;
}

} // namespace

WavWriter::~WavWriter()
{
    close();
}

bool WavWriter::open(const QString& path, int sampleRate, int channels, int bits)
{
    close();
    m_error.clear();
    m_frames = 0;
    if (sampleRate < 1 || channels < 1 || (bits != 16 && bits != 24)) {
        m_error = QStringLiteral("Unsupported WAV format");
        return false;
    }
    m_rate = sampleRate;
    m_channels = channels;
    m_bits = bits;
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = m_file.errorString();
        return false;
    }
    if (m_file.write(header(m_rate, m_channels, m_bits, 0)) < 0) {
        m_error = m_file.errorString();
        m_file.close();
        return false;
    }
    return true;
}

bool WavWriter::write(const float* samples, int64_t frames)
{
    if (!m_file.isOpen() || frames <= 0)
        return m_file.isOpen();
    const int bytes = m_bits / 8;
    const int64_t count = frames * m_channels;
    QByteArray out;
    out.resize(qsizetype(count) * bytes);
    char* dst = out.data();
    for (int64_t i = 0; i < count; ++i) {
        const float s = std::clamp(samples[i], -1.0f, 1.0f);
        if (m_bits == 16) {
            const auto v = qint16(std::lrintf(s * 32767.0f));
            *dst++ = char(v & 0xff);
            *dst++ = char((v >> 8) & 0xff);
        } else {
            const qint32 v = qint32(std::lrintf(s * 8388607.0f));
            *dst++ = char(v & 0xff);
            *dst++ = char((v >> 8) & 0xff);
            *dst++ = char((v >> 16) & 0xff);
        }
    }
    if (m_file.write(out) != out.size()) {
        m_error = m_file.errorString();
        return false;
    }
    m_frames += frames;
    return true;
}

bool WavWriter::close()
{
    if (!m_file.isOpen())
        return true;
    bool ok = true;
    const quint64 dataBytes = quint64(m_frames) * quint64(m_channels * m_bits / 8);
    if (dataBytes > 0xffffffffull - 36) { // beyond what a plain WAV can describe (about 4 GiB)
        m_error = QStringLiteral("The recording is too long for a WAV file");
        ok = false;
    }
    ok = ok && m_file.seek(0)
        && m_file.write(header(m_rate, m_channels, m_bits, quint32(dataBytes))) >= 0;
    if (!ok && m_error.isEmpty())
        m_error = m_file.errorString();
    m_file.close();
    return ok;
}

} // namespace kk::media
