// SPDX-License-Identifier: GPL-3.0-or-later
// Texts that are translated at run time through a helper function lupdate does not know
// (QCoreApplication::translate with a variable), listed here once so that lupdate finds them.
// The context is the one the helper uses.
#include <QtGlobal>

namespace kk {

[[maybe_unused]] static const char* const kTimelinePainterTexts[] = {
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Media Offline"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Freeze 0%"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "R"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Video %1"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Subtitle %1"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Audio %1"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Mono"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "Stereo"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "1 Clip"),
    QT_TRANSLATE_NOOP("kk::TimelinePainter", "%1 Clips"),
};

[[maybe_unused]] static const char* const kExportPresetNames[] = {
    QT_TRANSLATE_NOOP("kk::ExportPresets", "YouTube 1080p"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "YouTube 4K"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Vimeo 1080p"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "H.265 MP4"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "AV1 MP4"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Web WebM"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Master ProRes 422 HQ"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "ProRes 4444 with alpha"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Archive DNxHR HQ"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "MKV H.264 + FLAC"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Audio WAV"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Audio FLAC"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Audio MP3"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Audio AAC"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Audio Opus"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "PNG sequence"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "JPEG sequence"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "TIFF sequence"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "OpenEXR sequence"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Frame grab PNG"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "Frame grab JPEG"),
    QT_TRANSLATE_NOOP("kk::ExportPresets", "GIF"),
};

} // namespace kk
