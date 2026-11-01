// SPDX-License-Identifier: GPL-3.0-or-later
// MediaTestUtil — shared helpers for the media tests: fixture generation and barcode reading.
#pragma once

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

#include <cstdint>

namespace kk::test {

/// Directory holding the generated fixtures (gitignored).
inline QString fixtureDir()
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated");
}

/// Absolute path of a generated fixture file.
inline QString fixture(const char* name)
{
    return fixtureDir() + QLatin1Char('/') + QString::fromLatin1(name);
}

/// Runs scripts/make_test_media.sh (fast no-op when the fixtures exist).
/// @return false with a reason when ffmpeg is unavailable or generation failed.
inline bool ensureFixtures(QString* why)
{
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        *why = QStringLiteral("ffmpeg CLI not found; cannot generate media fixtures");
        return false;
    }
    const QString script
        = QDir::cleanPath(QStringLiteral(KK_TEST_DATA_DIR "/../../scripts/make_test_media.sh"));
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(QStringLiteral("bash"), {script, fixtureDir()});
    if (!process.waitForFinished(5 * 60 * 1000) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        *why = QStringLiteral("make_test_media.sh failed (exit code %1)").arg(process.exitCode());
        return false;
    }
    return true;
}

/// Reads the frame-index barcode drawn by make_test_media.sh into the top 1/8 of a frame
/// (16 columns × 2 rows; row 0 = 12 index bits LSB first + marker 1010, row 1 = inverted).
/// Works on scaled images. @return the frame index, or -1 if the barcode is not readable.
inline int readFrameIndex(const QImage& image)
{
    if (image.isNull() || image.width() < 16 || image.height() < 32) {
        return -1;
    }
    const double stripHeight = image.height() / 8.0;
    auto bitAt = [&](int column, int row) {
        const int x = int((column + 0.5) * image.width() / 16.0);
        const int y = int((row * 0.5 + 0.25) * stripHeight);
        return qGray(image.pixel(x, y)) > 128 ? 1 : 0;
    };
    int index = 0;
    for (int column = 0; column < 16; ++column) {
        const int bit = bitAt(column, 0);
        if (bit == bitAt(column, 1)) {
            return -1; // row 1 must be the inverse of row 0
        }
        if (column < 12) {
            index |= bit << column;
        } else if (bit != (column % 2 == 0 ? 1 : 0)) {
            return -1; // marker mismatch
        }
    }
    return index;
}

/// Index of the frame shown at `timeUs` in a constant-rate fixture of `num/den` fps
/// (frame k covers [k·den/num s, (k+1)·den/num s)).
inline int64_t expectedFrameIndex(int64_t timeUs, int64_t num, int64_t den)
{
    return (timeUs * num) / (den * 1'000'000);
}

/// First microsecond (rounded up) at which frame `index` of a `num/den` fps fixture is shown.
inline int64_t frameStartUs(int64_t index, int64_t num, int64_t den)
{
    return (index * den * 1'000'000 + num - 1) / num;
}

} // namespace kk::test
