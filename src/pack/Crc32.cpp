// SPDX-License-Identifier: GPL-3.0-or-later
#include "pack/Crc32.h"

#include <QtGlobal>

#include <cstring>

namespace kk::pack {

namespace {

/// t[0] is the classic byte-wise table; t[k][i] is the CRC of byte i followed by k zero bytes,
/// which lets the main loop fold 8 input bytes with 8 independent look-ups.
struct Tables
{
    uint32_t t[8][256];
};

constexpr Tables makeTables()
{
    Tables r {};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        r.t[0][i] = c;
    }
    for (int s = 1; s < 8; ++s) {
        for (uint32_t i = 0; i < 256; ++i)
            r.t[s][i] = (r.t[s - 1][i] >> 8) ^ r.t[0][r.t[s - 1][i] & 0xFF];
    }
    return r;
}

constexpr Tables kTables = makeTables();

} // namespace

uint32_t crc32Update(uint32_t crc, const void* data, size_t size)
{
    const auto& t = kTables.t;
    const auto* p = static_cast<const unsigned char*>(data);
    uint32_t c = ~crc;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    while (size >= 8) {
        uint32_t one;
        uint32_t two;
        std::memcpy(&one, p, 4);
        std::memcpy(&two, p + 4, 4);
        one ^= c;
        c = t[7][one & 0xFF] ^ t[6][(one >> 8) & 0xFF] ^ t[5][(one >> 16) & 0xFF] ^ t[4][one >> 24]
            ^ t[3][two & 0xFF] ^ t[2][(two >> 8) & 0xFF] ^ t[1][(two >> 16) & 0xFF]
            ^ t[0][two >> 24];
        p += 8;
        size -= 8;
    }
#endif
    while (size--)
        c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

} // namespace kk::pack
