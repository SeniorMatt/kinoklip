// SPDX-License-Identifier: GPL-3.0-or-later
// Crc32 — the CRC-32 of ZIP, PNG and zlib (reflected polynomial 0xEDB88320), slicing-by-8.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kk::pack {

/// Continues a CRC-32 over @p size bytes at @p data. Start with 0; same semantics as zlib's
/// crc32(crc, data, size), so crc32Update(crc32Update(0, a), b) == crc32 of a followed by b.
/// About 1.5 - 2 GB/s on one core (slicing-by-8: eight table look-ups per 8 input bytes).
uint32_t crc32Update(uint32_t crc, const void* data, size_t size);

} // namespace kk::pack
