// SPDX-License-Identifier: GPL-3.0-or-later
// ImageSequence — numbered still images (name_0001.png ...) used as one video clip: detection
// from a member file, and the "sequence path" convention understood by probe()/VideoDecoder.
#pragma once

#include "core/Rational.h"

#include <QString>
#include <QVector>

namespace kk::media {

/// A detected or scanned image sequence.
struct SequenceInfo
{
    bool found = false; ///< at least ImageSequence::kMinFrames numbered files match
    QString pattern; ///< printf-style pattern: "/dir/name_%04d.png" (padded) or "/dir/f%d.png"
    int firstNumber = 0; ///< smallest frame number present
    int lastNumber = 0; ///< largest frame number present
    int fileCount = 0; ///< files present (lastNumber - firstNumber + 1 - missing.size())
    QVector<int> missing; ///< numbers in [first, last] without a file (gaps), ascending
    /// Clip length in frames: every number from first to last (a gap holds the previous frame).
    int frameCount() const { return found ? lastNumber - firstNumber + 1 : 0; }
};

/// Image sequences as video sources.
///
/// **Path convention.** A sequence is addressed by a single string that every media API
/// (probe(), VideoDecoder::open(), MediaCache, project files) accepts like a file path:
/// ```
///   <directory>/<prefix>%0<N>d<suffix>[#fps=<num>[/<den>]]
///   /footage/shot1/render_%04d.exr#fps=24000/1001
///   /footage/stills/%d.jpg                       (unpadded numbers, default 25 fps)
/// ```
/// - `%0Nd` = frame number zero-padded to N digits, `%d` = unpadded; a literal '%' in a file
///   name is written "%%". Only the file name part may contain the number.
/// - The optional `#fps=` suffix sets the clip's frame rate (default kDefaultFrameRate).
/// - The first frame is the lowest number found on disk (no start-number field needed); frame
///   k of the clip shows number first + k. Missing numbers repeat the previous existing file,
///   so timing stays aligned with the numbering.
/// probe() of such a path reports `hasVideo`, `isImageSequence`, `frameRate` = the fps,
/// `durationUs` = frameCount / fps, size/codec of the first file and the gap count.
///
/// Thread-safety: all functions are re-entrant (they only read the file system).
class ImageSequence
{
public:
    /// Frame rate used when a sequence path has no `#fps=` suffix.
    static constexpr Rational kDefaultFrameRate {25, 1};
    /// Minimum number of numbered files for detect() to report a sequence.
    static constexpr int kMinFrames = 2;

    /// Looks for numbered siblings of `filePath` (an existing image file) and returns the
    /// sequence it belongs to. The LAST run of digits in the file name is the frame number:
    /// `name_0001.png`, `name.0001.exr`, `0001.jpg`, `take2_017.tif` all work. Files must
    /// share prefix, suffix (case-sensitive) and — for zero-padded numbers — digit count.
    /// Use this in the importer to offer "import as sequence" (found == true).
    static SequenceInfo detect(const QString& filePath);

    /// Lists the files of `pattern` (a sequence path; any `#fps=` suffix is ignored).
    static SequenceInfo scan(const QString& sequencePath);

    /// True if `path` follows the sequence path convention (has a `%d`/`%0Nd` placeholder in the
    /// file name, after optional `#fps=` removal). Does not touch the file system.
    static bool isSequencePath(const QString& path);

    /// Builds a sequence path from a pattern and frame rate (fps omitted when it is the
    /// default). Example: makePath("/d/a_%04d.png", {24, 1}) → "/d/a_%04d.png#fps=24".
    static QString makePath(const QString& pattern, Rational frameRate = kDefaultFrameRate);

    /// Splits a sequence path into pattern and frame rate. @return false if not a sequence path.
    static bool parsePath(const QString& sequencePath, QString* pattern, Rational* frameRate);

    /// File name of frame `number` for `pattern` ("%04d", 7 → "0007").
    static QString fileForNumber(const QString& pattern, int number);

    /// Short name for UI lists: "render_[0001-0240].exr".
    static QString displayName(const QString& sequencePath);
};

/// Convenience for the importer: ImageSequence::detect().
inline SequenceInfo detectSequence(const QString& filePath)
{
    return ImageSequence::detect(filePath);
}

} // namespace kk::media
