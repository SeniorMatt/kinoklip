// SPDX-License-Identifier: GPL-3.0-or-later
// SupportedFormats — the one registry of importable file types (docs/archive/plan.md §5.8): categories,
// file name extensions, QFileDialog filters and quick "is this media?" checks.
#pragma once

#include <QString>
#include <QStringList>

namespace kk::media {

/// What kind of media a file name suggests. Only used for UI purposes (file dialogs, media
/// browser filters, icons, the "import as sequence" offer); whether a file is really accepted
/// is decided by probe(), which tries every decoder regardless of the extension.
enum class MediaCategory {
    Unknown,
    Video, ///< video containers (mp4, mov, mkv, ...); may also be audio-only
    Audio, ///< audio files (wav, flac, mp3, ...)
    Image, ///< still images (png, jpg, heic, exr, ...); gif/webp may be animated
    RawImage, ///< camera RAW stills (cr2, nef, dng, ...), decoded with LibRaw
    ImageSequence, ///< a numbered-file pattern path, see ImageSequence.h
};

/// Static registry of the supported import formats. All functions are cheap (hash lookups on
/// tables built once) and thread-safe.
///
/// To add an extension, append it to the right table in SupportedFormats.cpp — nothing else
/// needs to change for dialogs and filters. Decoding is separate: FFmpeg/Qt/libheif/LibRaw
/// decide what actually opens (see readme.md "How to extend").
class SupportedFormats
{
public:
    using Category = MediaCategory;

    /// Lower-case extensions (without dot) of `category`, in display order.
    /// ImageSequence returns the image extensions usable in sequences (Image + RawImage).
    static const QStringList& extensions(MediaCategory category);

    /// Every extension of Video, Audio, Image and RawImage.
    static const QStringList& allExtensions();

    /// Category suggested by the path: ImageSequence for sequence pattern paths
    /// (ImageSequence::isSequencePath), otherwise by the (case-insensitive) extension.
    static MediaCategory categoryFor(const QString& path);

    /// True if the extension is in any category (or the path is a sequence pattern).
    /// A quick pre-filter for drag & drop and folder scans; the file is not opened.
    static bool isLikelyMedia(const QString& path);

    /// Filter string for QFileDialog::getOpenFileNames():
    /// "All media (...);;Video (...);;Audio (...);;Images (...);;All files (*)".
    /// Images includes camera RAW. The labels are English; the UI may translate them.
    static QString dialogFilter();

    /// One QFileDialog filter entry, e.g. "Audio (*.wav *.aif ...)".
    static QString dialogFilter(MediaCategory category);

    /// Human-readable category name ("Video", "Audio", "Image", "Camera RAW", "Image sequence").
    static QString categoryName(MediaCategory category);
};

/// Shorthands for the free-function style used elsewhere in the media engine.
inline MediaCategory categoryFor(const QString& path)
{
    return SupportedFormats::categoryFor(path);
}
inline bool isLikelyMedia(const QString& path)
{
    return SupportedFormats::isLikelyMedia(path);
}

} // namespace kk::media
