// SPDX-License-Identifier: GPL-3.0-or-later
// ImageLoader — decodes still images through the best available library (Qt image plugins,
// libheif, FFmpeg image decoders, LibRaw) into premultiplied ARGB32, plus a shared still cache.
#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk::media {

/// Library that decodes a still image.
enum class ImageBackend {
    None,
    Qt, ///< QImageReader plugins: png jpg webp bmp gif tiff svg ico ppm (+ installed kimageformats)
    Heif, ///< libheif: heic heif avif (only with KK_HAVE_LIBHEIF)
    FFmpeg, ///< libavcodec image decoders: jxl exr dpx psd qoi pcx sgi jpeg2000 tga ...
    Raw, ///< LibRaw: camera RAW (only with KK_HAVE_LIBRAW)
};

/// Short lower-case backend name ("qt", "heif", "ffmpeg", "raw", "none") for logs and UI.
QString imageBackendName(ImageBackend backend);

/// What readHeader() learned about a still image, usually without decoding pixels.
struct ImageHeader
{
    bool valid = false;
    QString error; ///< why !valid
    /// Size of the image load() returns: orientation (EXIF/irot/RAW flip) applied, and for
    /// camera RAW the half-size decode taken into account (see ImageLoader::setRawFullSize()).
    QSize size;
    bool hasAlpha = false;
    bool animated = false; ///< more than one frame (animated GIF/WebP); load() gives frame 0
    int frameCount = 1; ///< frames in the file (1 for stills), 0 if unknown
    QString format; ///< short format name ("png", "jpeg", "heif", "exr", "raw" ...)
    QString pixelFormat; ///< source sample format if known ("rgb24", "gbrpf32le", "16-bit" ...)
    ImageBackend backend = ImageBackend::None; ///< library that read the header (and will decode)
};

/// Result of ImageLoader::load().
struct LoadedImage
{
    QImage image; ///< Format_ARGB32_Premultiplied, orientation applied; null on failure
    QString error; ///< why image is null
    ImageBackend backend = ImageBackend::None;
    bool hasAlpha = false; ///< the source has an alpha channel
};

/// Still-image decoding for probe(), VideoDecoder and image sequences.
///
/// Backend choice: the file extension picks the first library to try (fast path — no content
/// sniffing for the common case); if it fails the remaining backends are tried in the order
/// Qt → libheif → FFmpeg → LibRaw, so a mislabelled file still opens.
///
/// | Extension                                        | First backend |
/// |--------------------------------------------------|---------------|
/// | png jpg jpeg jpe jfif webp bmp gif tif tiff svg svgz ico ppm pgm | Qt |
/// | heic heif hif avif                               | libheif (FFmpeg when built without) |
/// | jxl exr dpx psd qoi pcx sgi rgb jp2 j2k tga      | FFmpeg        |
/// | cr2 cr3 nef arw dng raf orf rw2 ... (SupportedFormats RawImage) | LibRaw |
///
/// Orientation: EXIF orientation (JPEG/TIFF/WebP via Qt), HEIF `irot`/`imir` (libheif) and the
/// RAW flip (LibRaw) are applied, so the returned image is upright.
/// Camera RAW: half-size decode by default (one pixel per 2x2 Bayer block, ~4x faster, still
/// at least 3 MP for current cameras); see setRawFullSize().
/// HDR / float sources (EXR, 16-bit PNG/TIFF, 10-bit HEIF): converted to 8 bit; EXR is converted
/// with the sRGB transfer curve and values above 1.0 are clamped.
/// TODO(media): keep float/16-bit stills for an HDR compositor path instead of clamping.
///
/// Thread-safety: all functions are thread-safe (each call uses its own decoder state; the
/// cache is mutex-protected).
class ImageLoader
{
public:
    /// True if the extension of `path` belongs to a format that is loaded as a still image
    /// (SupportedFormats Image or RawImage). Cheap; does not touch the file.
    static bool handlesExtension(const QString& path);

    /// Reads the image header (size, orientation, alpha, frame count) — header only for Qt,
    /// libheif and LibRaw; FFmpeg formats read stream info (no full decode for most formats).
    static ImageHeader readHeader(const QString& path);

    /// Decodes `path` into a full-size premultiplied ARGB32 image. Uncached; prefer
    /// loadCached() for images that are requested repeatedly.
    static LoadedImage load(const QString& path);

    /// Like load(), but returns a shared copy from the process-wide still cache when the same
    /// file (path, modification time, size, RAW mode) was decoded before. The returned QImage
    /// shares its pixels with the cache (implicit sharing): treat it as read-only.
    /// Typical cost of a hit: one hash lookup + a QImage refcount increment.
    static LoadedImage loadCached(const QString& path);

    /// Camera RAW decode size for later loads: false (default) = half size (fast), true = full
    /// resolution with AHD demosaicing. Changing it does not invalidate cached images (the
    /// mode is part of the cache key).
    static void setRawFullSize(bool fullSize);
    static bool rawFullSize();

    /// Memory budget of the still cache in bytes (default 256 MiB). Least recently used images
    /// are dropped first; an image larger than the budget is returned but not cached.
    static void setCacheLimitBytes(int64_t bytes);

    /// Drops every cached still image (e.g. after files changed on disk or in tests).
    static void clearCache();

    /// Number of images and bytes currently held by the still cache (diagnostics, tests).
    static int cachedImageCount();
    static int64_t cachedBytes();

    /// True if the build includes libheif (HEIC/HEIF/AVIF) or LibRaw (camera RAW) support.
    static bool hasHeifSupport();
    static bool hasRawSupport();
};

} // namespace kk::media
