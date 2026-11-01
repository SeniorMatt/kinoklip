// SPDX-License-Identifier: GPL-3.0-or-later
// STUB — replaced by the media agent with a real FFmpeg implementation.
#include "media/MediaInfo.h"

#include <QFileInfo>

namespace cw::media {
MediaInfo probe(const QString& path)
{
    MediaInfo mi;
    mi.path = path;
    mi.error = QStringLiteral("probe() not implemented yet");
    return mi;
}
} // namespace cw::media
