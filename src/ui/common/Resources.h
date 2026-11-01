// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit initialisation of the compiled-in resources (icons, default keymap).
#pragma once

namespace cw {

/// Registers resources/resources.qrc. The .qrc is compiled into the static cutwise_core
/// library, whose automatic initialiser the linker may drop, so call this once in main() and in
/// tests that need icons or the default keymap. Safe to call more than once.
void initResources();

} // namespace cw
