// SPDX-License-Identifier: GPL-3.0-or-later
// GlInfo implementation.
#include "render/GlInfo.h"

#include <mutex>

namespace kk::render {

namespace {
std::mutex& infoMutex()
{
    static std::mutex m;
    return m;
}
GlDriverInfo& stored()
{
    static GlDriverInfo info;
    return info;
}
} // namespace

void rememberGlDriverInfo(const GlDriverInfo& info)
{
    const std::lock_guard lock(infoMutex());
    stored() = info;
}

GlDriverInfo knownGlDriverInfo()
{
    const std::lock_guard lock(infoMutex());
    return stored();
}

} // namespace kk::render
