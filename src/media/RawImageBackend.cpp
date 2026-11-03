// SPDX-License-Identifier: GPL-3.0-or-later
// RawImageBackend — camera RAW stills through LibRaw: half-size (default) or full AHD decode,
// camera white balance, sRGB 8-bit output, sensor orientation applied. Stubs without
// KK_HAVE_LIBRAW.
#include "media/ImageBackends.h"

#ifdef KK_HAVE_LIBRAW

#include "media/MediaLog.h"
#include "media/MediaSource.h"

#include <QFile>

#include <libraw.h>
#include <memory>

namespace kk::media::detail {

namespace {

/// LibRaw `user_qual` for full-size decodes: 3 = AHD demosaicing (good quality, moderate cost).
constexpr int kFullSizeDemosaic = 3;

/// LibRaw `output_color` 1 = sRGB primaries.
constexpr int kOutputSrgb = 1;

/// The LibRaw object is large (several hundred KB of tables): heap-allocate it.
/// We link libraw_r, the thread-safe build: separate instances may run concurrently.
/// Media embedded in a single-file project are read into `buffer` (LibRaw keeps a pointer to
/// it): the caller declares it before the returned object so it outlives it.
std::unique_ptr<LibRaw> openRaw(
    const QString& path, bool fullSize, QByteArray& buffer, QString* error)
{
    auto raw = std::make_unique<LibRaw>();
    libraw_output_params_t& params = raw->imgdata.params;
    params.half_size = fullSize ? 0 : 1; // one RGB pixel per 2x2 Bayer quad: no demosaicing
    params.user_qual = kFullSizeDemosaic;
    params.use_camera_wb = 1;
    params.output_color = kOutputSrgb;
    params.output_bps = 8;
    const MediaSource source = MediaSource::resolve(path);
    int ret = LIBRAW_SUCCESS;
    if (source.isEmbedded()) {
        buffer = source.readAll(error);
        if (buffer.isNull()) {
            return {};
        }
        ret = raw->open_buffer(buffer.constData(), size_t(buffer.size()));
    } else {
        const QByteArray file = QFile::encodeName(path);
        ret = raw->open_file(file.constData());
    }
    if (ret != LIBRAW_SUCCESS) {
        *error = QString::fromUtf8(libraw_strerror(ret));
        return {};
    }
    return raw;
}

} // namespace

bool readHeaderRaw(const QString& path, bool fullSize, ImageHeader& header)
{
    QString error;
    QByteArray buffer;
    const std::unique_ptr<LibRaw> raw = openRaw(path, fullSize, buffer, &error);
    if (!raw) {
        header.error = error;
        return false;
    }
    // Computes the output size without decoding: half-size and the flip (portrait shots
    // swap width and height) are taken into account, matching dcraw_make_mem_image().
    if (raw->adjust_sizes_info_only() != LIBRAW_SUCCESS) {
        header.error = QStringLiteral("Cannot determine RAW image size");
        return false;
    }
    const libraw_image_sizes_t& sizes = raw->imgdata.sizes;
    header.valid = true;
    header.size = {sizes.iwidth, sizes.iheight};
    header.format = QStringLiteral("raw");
    const libraw_iparams_t& id = raw->imgdata.idata;
    header.pixelFormat = QStringLiteral("%1 %2")
                             .arg(QString::fromUtf8(id.make), QString::fromUtf8(id.model))
                             .trimmed();
    header.backend = ImageBackend::Raw;
    return true;
}

bool loadRaw(const QString& path, bool fullSize, LoadedImage& result)
{
    QByteArray buffer;
    const std::unique_ptr<LibRaw> raw = openRaw(path, fullSize, buffer, &result.error);
    if (!raw) {
        return false;
    }
    int ret = raw->unpack();
    if (ret == LIBRAW_SUCCESS) {
        ret = raw->dcraw_process();
    }
    if (ret != LIBRAW_SUCCESS) {
        result.error = QString::fromUtf8(libraw_strerror(ret));
        return false;
    }
    // dcraw_make_mem_image() applies the orientation (flip) while copying.
    libraw_processed_image_t* processed = raw->dcraw_make_mem_image(&ret);
    if (!processed) {
        result.error = QString::fromUtf8(libraw_strerror(ret));
        return false;
    }
    const std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> owner(
        processed, LibRaw::dcraw_clear_mem);
    if (processed->type != LIBRAW_IMAGE_BITMAP || processed->colors != 3 || processed->bits != 8) {
        result.error = QStringLiteral("Unexpected RAW output format");
        return false;
    }
    const QImage wrapped(processed->data, processed->width, processed->height, processed->width * 3,
        QImage::Format_RGB888);
    result.image = wrapped.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    result.backend = ImageBackend::Raw;
    return !result.image.isNull();
}

} // namespace kk::media::detail

#else // !KK_HAVE_LIBRAW

namespace kk::media::detail {

bool readHeaderRaw(const QString&, bool, ImageHeader& header)
{
    header.error = QStringLiteral("Built without LibRaw");
    return false;
}

bool loadRaw(const QString&, bool, LoadedImage& result)
{
    result.error = QStringLiteral("Built without LibRaw (camera RAW support)");
    return false;
}

} // namespace kk::media::detail

#endif
