// SPDX-License-Identifier: GPL-3.0-or-later
// HeifImageBackend — HEIC/HEIF/AVIF stills through libheif (primary image, transformations
// irot/imir/clap applied, 8-bit RGB(A) output). Compiled to stubs without KK_HAVE_LIBHEIF.
#include "media/ImageBackends.h"

#ifdef KK_HAVE_LIBHEIF

#include "media/MediaLog.h"

#include <QFile>

#include <libheif/heif.h>
#include <memory>
#include <mutex>

namespace kk::media::detail {

namespace {

// RAII for libheif's C objects.
struct HeifContextDeleter
{
    void operator()(heif_context* ctx) const { heif_context_free(ctx); }
};
struct HeifHandleDeleter
{
    void operator()(heif_image_handle* handle) const { heif_image_handle_release(handle); }
};
struct HeifImageDeleter
{
    void operator()(heif_image* image) const { heif_image_release(image); }
};
using HeifContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HeifHandlePtr = std::unique_ptr<heif_image_handle, HeifHandleDeleter>;
using HeifImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;

/// libheif ≥ 1.13 loads its codec plugins (libde265, dav1d, aom...) in heif_init(); the call is
/// reference counted, we never deinit (plugins stay loaded for the process lifetime).
void initHeif()
{
    static std::once_flag once;
    std::call_once(once, [] {
#if LIBHEIF_HAVE_VERSION(1, 13, 0)
        heif_init(nullptr);
#endif
    });
}

/// Opens `path` and returns its primary image handle (nullptr + `error` on failure).
HeifHandlePtr openPrimary(const QString& path, HeifContextPtr& ctx, QString* error)
{
    initHeif();
    ctx.reset(heif_context_alloc());
    if (!ctx) {
        *error = QStringLiteral("Out of memory");
        return {};
    }
    const QByteArray file = QFile::encodeName(path);
    heif_error err = heif_context_read_from_file(ctx.get(), file.constData(), nullptr);
    if (err.code != heif_error_Ok) {
        *error = QString::fromUtf8(err.message);
        return {};
    }
    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx.get(), &handle);
    if (err.code != heif_error_Ok) {
        *error = QString::fromUtf8(err.message);
        return {};
    }
    return HeifHandlePtr(handle);
}

/// Brand of the file for MediaInfo::container ("avif" for AV1 files, else "heif").
QString formatName(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(12); // ftyp box: size, "ftyp", major brand
        if (head.size() == 12 && head.mid(8, 4).startsWith("avi")) {
            return QStringLiteral("avif");
        }
    }
    return QStringLiteral("heif");
}

} // namespace

bool readHeaderHeif(const QString& path, ImageHeader& header)
{
    HeifContextPtr ctx;
    QString error;
    const HeifHandlePtr handle = openPrimary(path, ctx, &error);
    if (!handle) {
        header.error = error;
        return false;
    }
    // heif_image_handle_get_width/height already include the irot/clap transformations.
    header.valid = true;
    header.size
        = {heif_image_handle_get_width(handle.get()), heif_image_handle_get_height(handle.get())};
    header.hasAlpha = heif_image_handle_has_alpha_channel(handle.get()) != 0;
    header.format = formatName(path);
    const int bits = heif_image_handle_get_luma_bits_per_pixel(handle.get());
    header.pixelFormat = bits > 0 ? QStringLiteral("%1-bit").arg(bits) : QString();
    header.backend = ImageBackend::Heif;
    return true;
}

bool loadHeif(const QString& path, LoadedImage& result)
{
    HeifContextPtr ctx;
    const HeifHandlePtr handle = openPrimary(path, ctx, &result.error);
    if (!handle) {
        return false;
    }
    const bool alpha = heif_image_handle_has_alpha_channel(handle.get()) != 0;
    // Interleaved 8-bit output: libheif converts YCbCr (and >8-bit sources) itself and applies
    // the orientation transformations (default decoding options).
    heif_image* raw = nullptr;
    const heif_error err = heif_decode_image(handle.get(), &raw, heif_colorspace_RGB,
        alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok) {
        result.error = QString::fromUtf8(err.message);
        return false;
    }
    const HeifImagePtr image(raw);
    int stride = 0;
    const uint8_t* data
        = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
    const int width = heif_image_get_width(image.get(), heif_channel_interleaved);
    const int height = heif_image_get_height(image.get(), heif_channel_interleaved);
    if (!data || width <= 0 || height <= 0) {
        result.error = QStringLiteral("HEIF decoder returned no pixels");
        return false;
    }
    // Wrap libheif's buffer without copying, then convert (that conversion is the one copy).
    const QImage wrapped(
        data, width, height, stride, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    result.image = wrapped.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    result.hasAlpha = alpha;
    result.backend = ImageBackend::Heif;
    return !result.image.isNull();
}

} // namespace kk::media::detail

#else // !KK_HAVE_LIBHEIF

namespace kk::media::detail {

bool readHeaderHeif(const QString&, ImageHeader& header)
{
    header.error = QStringLiteral("Built without libheif");
    return false;
}

bool loadHeif(const QString&, LoadedImage& result)
{
    result.error = QStringLiteral("Built without libheif (HEIC/HEIF/AVIF support)");
    return false;
}

} // namespace kk::media::detail

#endif
