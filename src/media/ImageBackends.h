// SPDX-License-Identifier: GPL-3.0-or-later
// ImageBackends — internal: the per-library still-image readers behind ImageLoader
// (ImageLoader.cpp: Qt; HeifImageBackend.cpp; FFmpegImageBackend.cpp; RawImageBackend.cpp).
#pragma once

#include "media/ImageLoader.h"

#include <QImage>
#include <QString>

namespace kk::media::detail {

// Every backend offers the same two functions:
//   bool readHeader<Name>(path, header)  — fills `header` (valid=true) or returns false;
//   bool load<Name>(path, result)        — fills `result.image` (ARGB32 premultiplied, upright)
//                                          or returns false with `result.error` set.
// They never throw, are thread-safe (no shared decoder state) and set `backend` themselves.
// Backends compiled without their library return false with an explanatory error.

bool readHeaderQt(const QString& path, ImageHeader& header);
bool loadQt(const QString& path, LoadedImage& result);

bool readHeaderHeif(const QString& path, ImageHeader& header);
bool loadHeif(const QString& path, LoadedImage& result);

bool readHeaderFFmpeg(const QString& path, ImageHeader& header);
bool loadFFmpeg(const QString& path, LoadedImage& result);

/// @param fullSize false = LibRaw half-size decode (see ImageLoader::setRawFullSize()).
bool readHeaderRaw(const QString& path, bool fullSize, ImageHeader& header);
bool loadRaw(const QString& path, bool fullSize, LoadedImage& result);

/// Converts `image` to Format_ARGB32_Premultiplied (no-op when it already is) — the one output
/// format of every backend.
QImage toPremultipliedArgb(QImage image);

} // namespace kk::media::detail
