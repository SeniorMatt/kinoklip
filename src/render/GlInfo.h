// SPDX-License-Identifier: GPL-3.0-or-later
// GlInfo — the OpenGL driver strings of the context the compositor already made, kept for
// Help > Report a Bug (src/app/BugReport). Nothing here creates a context.
#pragma once

#include <QString>

namespace kk::render {

/// GL_VENDOR / GL_RENDERER / GL_VERSION of a context that was really created.
struct GlDriverInfo
{
    QString vendor;
    QString renderer;
    QString version;
    [[nodiscard]] bool isEmpty() const { return renderer.isEmpty() && version.isEmpty(); }
};

/// Stores @p info (the GPU compositor calls this once its context is current). Thread-safe.
void rememberGlDriverInfo(const GlDriverInfo& info);
/// The last stored strings, empty when no OpenGL context was made in this process (yet).
/// Thread-safe.
[[nodiscard]] GlDriverInfo knownGlDriverInfo();

} // namespace kk::render
