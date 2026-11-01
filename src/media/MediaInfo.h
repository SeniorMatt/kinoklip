// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/Rational.h"

#include <QString>

#include <cstdint>

namespace cw::media {

struct MediaInfo
{
    bool valid = false;
    QString error;
    QString path;
    bool hasVideo = false;
    bool hasAudio = false;
    bool isStillImage = false; // png/jpg/...: hasVideo=true, durationUs=0 (infinite)
    int width = 0;
    int height = 0;
    int rotation = 0; // display rotation in degrees (0/90/180/270)
    Rational frameRate {0, 1}; // average/nominal frame rate; invalid for stills
    int64_t durationUs = 0;
    int audioChannels = 0;
    int audioSampleRate = 0;
    QString videoCodec;
    QString audioCodec;
    QString container;
};

// Synchronous; safe to call from any thread.
MediaInfo probe(const QString& path);

} // namespace cw::media
