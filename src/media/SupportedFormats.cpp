// SPDX-License-Identifier: GPL-3.0-or-later
// SupportedFormats — extension tables of plan.md §5.8 and the lookups built from them.
#include "media/SupportedFormats.h"

#include "media/ImageSequence.h"

#include <QFileInfo>
#include <QHash>

#include <array>

namespace kk::media {

namespace {

// ---- The tables (edit here to add an extension) -------------------------------------------

/// Video containers. Some (mp4, mkv, ogg...) may hold only audio; probe() tells.
constexpr std::array kVideoExtensions
    = {"mp4", "m4v", "mov", "qt", "mkv", "webm", "avi", "mts", "m2ts", "ts", "mxf", "flv", "f4v",
        "wmv", "asf", "3gp", "3g2", "ogv", "mpg", "mpeg", "m2v", "vob", "dv", "y4m"};

/// Audio files. ALAC lives in m4a/caf, Vorbis in ogg/oga.
constexpr std::array kAudioExtensions = {"wav", "aiff", "aif", "aifc", "flac", "mp3", "mp2", "aac",
    "m4a", "ogg", "oga", "opus", "wma", "ac3", "eac3", "ec3", "dts", "caf", "w64", "mka"};

/// Still images (Qt, libheif and FFmpeg image decoders; see ImageLoader.h for which is used).
constexpr std::array kImageExtensions = {"png", "jpg", "jpeg", "jpe", "jfif", "webp", "bmp", "gif",
    "tif", "tiff", "tga", "svg", "svgz", "ico", "heic", "heif", "hif", "avif", "jxl", "exr", "dpx",
    "psd", "qoi", "pcx", "sgi", "rgb", "jp2", "j2k", "ppm", "pgm"};

/// Camera RAW (LibRaw). DNG is listed here, not as TIFF.
constexpr std::array kRawExtensions = {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2",
    "dng", "raf", "orf", "rw2", "pef", "srw", "x3f", "3fr", "erf", "kdc", "mrw", "iiq", "rwl"};

template<size_t N>
QStringList toList(const std::array<const char*, N>& table)
{
    QStringList list;
    list.reserve(int(N));
    for (const char* extension : table) {
        list.append(QString::fromLatin1(extension));
    }
    return list;
}

/// Everything derived from the tables, built once (thread-safe static initialisation).
struct Registry
{
    QStringList video = toList(kVideoExtensions);
    QStringList audio = toList(kAudioExtensions);
    QStringList image = toList(kImageExtensions);
    QStringList raw = toList(kRawExtensions);
    QStringList sequence = image + raw;
    QStringList all = video + audio + image + raw;
    QHash<QString, MediaCategory> byExtension;

    Registry()
    {
        const std::pair<const QStringList*, MediaCategory> groups[]
            = {{&video, MediaCategory::Video}, {&audio, MediaCategory::Audio},
                {&image, MediaCategory::Image}, {&raw, MediaCategory::RawImage}};
        for (const auto& [list, category] : groups) {
            for (const QString& extension : *list) {
                byExtension.insert(extension, category);
            }
        }
    }
};

const Registry& registry()
{
    static const Registry instance;
    return instance;
}

/// "*.a *.b ..." for a QFileDialog filter.
QString globs(const QStringList& extensions)
{
    QString out;
    out.reserve(extensions.size() * 7);
    for (const QString& extension : extensions) {
        if (!out.isEmpty()) {
            out += QLatin1Char(' ');
        }
        out += QLatin1String("*.") + extension;
    }
    return out;
}

} // namespace

const QStringList& SupportedFormats::extensions(MediaCategory category)
{
    const Registry& r = registry();
    switch (category) {
    case MediaCategory::Video:
        return r.video;
    case MediaCategory::Audio:
        return r.audio;
    case MediaCategory::Image:
        return r.image;
    case MediaCategory::RawImage:
        return r.raw;
    case MediaCategory::ImageSequence:
        return r.sequence;
    case MediaCategory::Unknown:
        break;
    }
    static const QStringList kEmpty;
    return kEmpty;
}

const QStringList& SupportedFormats::allExtensions()
{
    return registry().all;
}

MediaCategory SupportedFormats::categoryFor(const QString& path)
{
    if (ImageSequence::isSequencePath(path)) {
        return MediaCategory::ImageSequence;
    }
    // QFileInfo::suffix() is the part after the LAST dot ("clip.final.MOV" → "MOV").
    const QString suffix = QFileInfo(path).suffix().toLower();
    return registry().byExtension.value(suffix, MediaCategory::Unknown);
}

bool SupportedFormats::isLikelyMedia(const QString& path)
{
    return categoryFor(path) != MediaCategory::Unknown;
}

QString SupportedFormats::dialogFilter(MediaCategory category)
{
    QStringList extensions;
    QString label;
    switch (category) {
    case MediaCategory::Unknown:
        return QStringLiteral("All media (%1)").arg(globs(allExtensions()));
    case MediaCategory::Image:
    case MediaCategory::RawImage:
    case MediaCategory::ImageSequence:
        label = category == MediaCategory::RawImage ? QStringLiteral("Camera RAW")
                                                    : QStringLiteral("Images");
        extensions = category == MediaCategory::RawImage ? registry().raw : registry().sequence;
        break;
    case MediaCategory::Video:
    case MediaCategory::Audio:
        label = categoryName(category);
        extensions = SupportedFormats::extensions(category);
        break;
    }
    return QStringLiteral("%1 (%2)").arg(label, globs(extensions));
}

QString SupportedFormats::dialogFilter()
{
    static const QString kFilter = dialogFilter(MediaCategory::Unknown) + QLatin1String(";;")
        + dialogFilter(MediaCategory::Video) + QLatin1String(";;")
        + dialogFilter(MediaCategory::Audio) + QLatin1String(";;")
        + dialogFilter(MediaCategory::Image) + QLatin1String(";;All files (*)");
    return kFilter;
}

QString SupportedFormats::categoryName(MediaCategory category)
{
    switch (category) {
    case MediaCategory::Video:
        return QStringLiteral("Video");
    case MediaCategory::Audio:
        return QStringLiteral("Audio");
    case MediaCategory::Image:
        return QStringLiteral("Image");
    case MediaCategory::RawImage:
        return QStringLiteral("Camera RAW");
    case MediaCategory::ImageSequence:
        return QStringLiteral("Image sequence");
    case MediaCategory::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

} // namespace kk::media
