// SPDX-License-Identifier: GPL-3.0-or-later
// ClipAttributes — interpretation helpers, channel matrix, fade curves, string ids.
#include "core/ClipAttributes.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

int normalizedRotation(int degrees)
{
    const int quarter = int(std::lround(double(degrees) / 90.0));
    return ((quarter % 4) + 4) % 4 * 90;
}

Rational effectivePixelAspect(const ClipAttributes& a, Rational fileSar)
{
    if (a.pixelAspect.isValid())
        return a.pixelAspect;
    return fileSar.isValid() ? fileSar : Rational {1, 1};
}

bool channelMatrix(const ClipAttributes& a, int channels, double* m)
{
    if (a.channelFormat == ChannelFormat::Auto || channels <= 0)
        return false;
    std::fill(m, m + 2 * channels, 0.0);
    // One output row: unity gain from a single source channel (an index beyond the file's
    // channels is silent, like kMutedChannel), or the average of all channels.
    const auto route = [&](double* row, int source) {
        if (source == kMixAllChannels) {
            for (int c = 0; c < channels; ++c)
                row[c] = 1.0 / channels;
        } else if (source >= 0 && source < channels) {
            row[source] = 1.0;
        }
    };
    if (a.channelFormat == ChannelFormat::Mono) {
        route(m, a.left);
        route(m + channels, a.left);
    } else {
        route(m, a.left == kMixAllChannels ? kMutedChannel : a.left);
        route(m + channels, a.right == kMixAllChannels ? kMutedChannel : a.right);
    }
    return true;
}

namespace {

template<typename E, size_t N>
QString toId(E value, const char* const (&names)[N])
{
    const size_t i = size_t(value);
    return QString::fromLatin1(i < N ? names[i] : names[0]);
}

template<typename E, size_t N>
E fromId(const QString& s, const char* const (&names)[N])
{
    for (size_t i = 0; i < N; ++i) {
        if (s == QLatin1String(names[i]))
            return E(i);
    }
    return E(0);
}

const char* const kLevels[] = {"auto", "video", "full"};
const char* const kFields[] = {"auto", "progressive", "upper", "lower"};
const char* const kFormats[] = {"auto", "stereo", "mono"};

} // namespace

QString dataLevelsToString(DataLevels v)
{
    return toId(v, kLevels);
}
DataLevels dataLevelsFromString(const QString& s)
{
    return fromId<DataLevels>(s, kLevels);
}
QString fieldOrderToString(FieldOrder v)
{
    return toId(v, kFields);
}
FieldOrder fieldOrderFromString(const QString& s)
{
    return fromId<FieldOrder>(s, kFields);
}
QString channelFormatToString(ChannelFormat v)
{
    return toId(v, kFormats);
}
ChannelFormat channelFormatFromString(const QString& s)
{
    return fromId<ChannelFormat>(s, kFormats);
}

double videoFadeFactor(int64_t local, int64_t duration, int64_t fadeIn, int64_t fadeOut)
{
    double f = 1.0;
    fadeIn = std::min(fadeIn, duration);
    fadeOut = std::min(fadeOut, duration);
    if (fadeIn > 0 && local < fadeIn)
        f = std::max(0.0, double(local) / double(fadeIn));
    const int64_t fromEnd = duration - 1 - local; // 0 on the last frame
    if (fadeOut > 0 && fromEnd < fadeOut)
        f *= std::max(0.0, double(fromEnd) / double(fadeOut));
    return std::clamp(f, 0.0, 1.0);
}

double audioFadeGain(double local, int64_t duration, int64_t fadeIn, int64_t fadeOut)
{
    constexpr double kHalfPi = std::numbers::pi / 2.0;
    double g = 1.0;
    fadeIn = std::min(fadeIn, duration);
    fadeOut = std::min(fadeOut, duration);
    if (fadeIn > 0 && local < double(fadeIn))
        g = std::sin(std::clamp(local / double(fadeIn), 0.0, 1.0) * kHalfPi);
    const double fromEnd = double(duration) - local; // 0 at the clip's end
    if (fadeOut > 0 && fromEnd < double(fadeOut))
        g *= std::sin(std::clamp(fromEnd / double(fadeOut), 0.0, 1.0) * kHalfPi);
    return g;
}

} // namespace kk
