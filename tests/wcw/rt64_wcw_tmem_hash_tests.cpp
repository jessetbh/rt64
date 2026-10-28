//
// RT64
//

// [wcw] aki-roadmap#119: TMEMHasher::hash scans each hashed byte for the TLUT bitset at most once (a garbage tile height
// rescanned TMEM thousands of times). Texture packs are keyed by these hashes, so the result must stay bit for bit the
// same: it is checked against the hasher as it was before (referenceHash, copied unchanged from d0b625f) on random TMEM
// and tiles, including tall row-by-row tiles that wrap TMEM many times.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>

#include "xxHash/xxh3.h"
#include "common/rt64_load_types.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_tmem_hasher.h"

#include "wcw_test.h"

using namespace RT64;

namespace {
    struct ReferenceHasher {
        static uint64_t referenceHash(const uint8_t *TMEM, const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlut, uint32_t version) {
            const uint32_t TMEMBytes = 4096;
            const uint32_t TMEMMask8 = 4095;
            const uint32_t TMEMMask16 = 2047;
            XXH3_state_t xxh3;
            XXH3_64bits_reset(&xxh3);
            const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
            const bool usesTLUT = tlut > 0;
            bool halfTMEM = RGBA32;
            
            // Version 3 fixes an error where using TLUT did not mask the address to the lower half of TMEM.
            if ((version >= 3) && usesTLUT) {
                halfTMEM = true;
            }

            const uint32_t tmemSize = halfTMEM ? (TMEMBytes >> 1) : TMEMBytes;
            const uint32_t drawBytesPerRow = std::max(uint32_t(width) << (RGBA32 ? G_IM_SIZ_16b : loadTile.siz) >> 1U, 1U);
            const uint32_t drawBytesTotal = (loadTile.line << 3) * (height - 1) + drawBytesPerRow;
            const uint32_t tmemMask = halfTMEM ? TMEMMask16 : TMEMMask8;
            const uint32_t tmemAddress = (loadTile.tmem << 3) & tmemMask;
            uint64_t tlutBitset[4] = {};
            auto hashUpdate = [&](const uint8_t *tmemBytes, uint32_t byteCount) {
                XXH3_64bits_update(&xxh3, tmemBytes, byteCount);

                // Version 5 parses every byte individually if TLUT is enabled to determine the TLUT bytes that can be hashed.
                if ((version >= 5) && usesTLUT) {
                    if (loadTile.siz == G_IM_SIZ_4b) {
                        for (uint32_t i = 0; i < byteCount; i++) {
                            tlutBitset[0] |= (1ULL << (tmemBytes[i] & 0xFU));
                            tlutBitset[0] |= (1ULL << ((tmemBytes[i] >> 4U) & 0xFU));
                        }
                    }
                    else {
                        for (uint32_t i = 0; i < byteCount; i++) {
                            tlutBitset[tmemBytes[i] >> 6U] |= (1ULL << (tmemBytes[i] & 0x3FU));
                        }
                    }
                }
            };

            auto hashTMEM = [&](uint32_t tmemBaseAddress, uint32_t tmemOrAddress, uint32_t byteCount, bool oddRow) {
                assert((tmemBaseAddress < tmemSize) && "Base address must be masked within the bounds of the TMEM size.");

                // Too many bytes to hash in a single step. Wrap around TMEM and hash the rest.
                if ((tmemBaseAddress + byteCount) > tmemSize) {
                    const uint32_t firstBytes = (tmemSize - tmemBaseAddress);
                    hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], firstBytes);

                    // Start hashing from the start of TMEM.
                    byteCount = std::min(byteCount - firstBytes, tmemBaseAddress);
                    tmemBaseAddress = 0;
                }

                // Version 4 fixes an error where odd row word swapping was not considered when hashing rows individually.
                if (oddRow && (version >= 4)) {
                    uint32_t wordCount = byteCount / 8;
                    if (wordCount > 0) {
                        hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], wordCount * 8);
                        tmemBaseAddress += wordCount * 8;
                        byteCount -= wordCount * 8;
                    }
                    
                    if (byteCount > 4) {
                        hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], byteCount - 4);
                        hashUpdate(&TMEM[(tmemBaseAddress + 4) | tmemOrAddress], 4);
                    }
                    else if (byteCount > 0) {
                        hashUpdate(&TMEM[(tmemBaseAddress + 4) | tmemOrAddress], byteCount);
                    }
                }
                else {
                    hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], byteCount);
                }
            };

            // Version 2 introduces the ability to hash row by row because not all bytes of the line in TMEM are used.
            if ((version >= 2) && TMEMHasher::needsToHashRowsIndividually(loadTile, width)) {
                uint32_t tmemBytesPerRow = loadTile.line << 3;
                for (uint32_t i = 0; i < height; i++) {
                    hashTMEM((tmemAddress + i * tmemBytesPerRow) & tmemMask, 0x0, drawBytesPerRow, i & 1);
                }

                if (RGBA32) {
                    for (uint32_t i = 0; i < height; i++) {
                        hashTMEM((tmemAddress + i * tmemBytesPerRow) & tmemMask, tmemSize, drawBytesPerRow, i & 1);
                    }
                }
            }
            else {
                hashTMEM(tmemAddress, 0x0, drawBytesTotal, false);

                if (RGBA32) {
                    hashTMEM(tmemAddress, tmemSize, drawBytesTotal, false);
                }
            }

            // If TLUT is active, we also hash the corresponding palette bytes.
            if (usesTLUT) {
                const bool CI4 = (loadTile.siz == G_IM_SIZ_4b);
                const int32_t paletteOffset = CI4 ? (loadTile.palette << 7) : 0;
                const int32_t paletteAddress = (TMEMBytes >> 1) + paletteOffset;

                // Version 5 stores a bitset of all the indices that should be hashed.
                if (version >= 5) {
                    // Fast path for a full bitset for CI4.
                    if (CI4 && (tlutBitset[0] == UINT16_MAX)) {
                        XXH3_64bits_update(&xxh3, &TMEM[paletteAddress], 0x80);
                    }
                    else {
                        const uint32_t bitsetCount = CI4 ? 1 : 4;
                        uint32_t bitsetIndex = 0;
                        uint32_t paletteIndex = 0;
                        for (uint32_t i = 0; i < bitsetCount; i++) {
                            // Fast path for a full bitset.
                            if (tlutBitset[i] == UINT64_MAX) {
                                XXH3_64bits_update(&xxh3, &TMEM[paletteAddress + i * 0x200], 0x200);
                            }
                            else {
                                // Must check every bit individually.
                                while (tlutBitset[i] > 0) {
                                    bitsetIndex = bitScanForward64(tlutBitset[i]);
                                    paletteIndex = (i * 0x40) + bitsetIndex;
                                    XXH3_64bits_update(&xxh3, &TMEM[paletteAddress + paletteIndex * 8], 8);
                                    tlutBitset[i] &= ~(1ULL << bitsetIndex);
                                }
                            }
                        }
                    }
                }
                else {
                    const int32_t bytesToHash = CI4 ? 0x80 : 0x800;
                    XXH3_64bits_update(&xxh3, &TMEM[paletteAddress], bytesToHash);
                }
            }
            
            // Encode more parameters into the hash that affect the final RGBA32 output.
            static_assert(sizeof(width) == 2, "Hash must use 16-bit width.");
            static_assert(sizeof(height) == 2, "Hash must use 16-bit height.");
            static_assert(sizeof(tlut) == 4, "Hash must use 32-bit TLUT.");
            static_assert(sizeof(loadTile.line) == 2, "Hash must use 16-bit line.");
            static_assert(sizeof(loadTile.siz) == 1, "Hash must use 8-bit siz.");
            static_assert(sizeof(loadTile.fmt) == 1, "Hash must use 8-bit fmt.");
            XXH3_64bits_update(&xxh3, &width, sizeof(width));
            XXH3_64bits_update(&xxh3, &height, sizeof(height));
            XXH3_64bits_update(&xxh3, &tlut, sizeof(tlut));
            XXH3_64bits_update(&xxh3, &loadTile.line, sizeof(loadTile.line));
            XXH3_64bits_update(&xxh3, &loadTile.siz, sizeof(loadTile.siz));
            XXH3_64bits_update(&xxh3, &loadTile.fmt, sizeof(loadTile.fmt));

            return XXH3_64bits_digest(&xxh3);
        }

    };

    uint8_t tmemBuffer[4096 + 64];   // an odd row's swap may read a few bytes past TMEM, as on the RDP struct

    LoadTile randomTile(std::mt19937 &rng) {
        LoadTile t = {};
        const uint8_t sizes[] = { G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b };
        const uint8_t formats[] = { G_IM_FMT_RGBA, G_IM_FMT_CI, G_IM_FMT_IA, G_IM_FMT_I };
        t.siz = sizes[rng() % 4];
        t.fmt = formats[rng() % 4];
        t.line = uint16_t(1 + rng() % 64);
        t.tmem = uint16_t(rng() % 512);
        t.palette = uint8_t(rng() % 16);
        return t;
    }
}

TEST_CASE("TMEM hash: unchanged from the reference hasher on random tiles (every version, TLUT on and off)") {
    std::mt19937 rng(119);
    uint32_t checked = 0;
    for (uint32_t c = 0; c < 4000; c++) {
        for (uint8_t &b : tmemBuffer) {
            b = uint8_t(rng() % 3 == 0 ? rng() : (rng() % 4));   // some repeats, some full-range bytes
        }

        const LoadTile tile = randomTile(rng);
        const uint16_t width = uint16_t(1 + rng() % 256);
        const uint16_t height = uint16_t(1 + rng() % 128);
        const uint32_t tlut = (rng() % 2) ? (0x8000U + (rng() % 2) * 0x4000U) : 0;
        const uint32_t version = 1 + rng() % TMEMHasher::CurrentHashVersion;
        const uint64_t want = ReferenceHasher::referenceHash(tmemBuffer, tile, width, height, tlut, version);
        const uint64_t got = TMEMHasher::hash(tmemBuffer, tile, width, height, tlut, version);
        CHECK_EQ(got, want);
        checked++;
    }

    CHECK_EQ(checked, uint32_t(4000));
}

TEST_CASE("TMEM hash: unchanged on tall row-by-row TLUT tiles that wrap TMEM (Starrcade 2.0's garbage heights)") {
    std::mt19937 rng(2);
    for (uint32_t c = 0; c < 200; c++) {
        for (uint8_t &b : tmemBuffer) {
            b = uint8_t(rng());
        }

        LoadTile tile = randomTile(rng);
        tile.siz = (c % 2) ? G_IM_SIZ_4b : G_IM_SIZ_8b;
        tile.fmt = G_IM_FMT_CI;
        tile.line = uint16_t(32 + rng() % 32);                  // wider than the drawn row: hashed row by row
        const uint16_t width = uint16_t(8 + rng() % 64);
        const uint16_t height = uint16_t(512 + rng() % 3584);   // far more rows than TMEM holds
        CHECK(TMEMHasher::needsToHashRowsIndividually(tile, width));
        const uint64_t want = ReferenceHasher::referenceHash(tmemBuffer, tile, width, height, 0x8000, TMEMHasher::CurrentHashVersion);
        CHECK_EQ(TMEMHasher::hash(tmemBuffer, tile, width, height, 0x8000, TMEMHasher::CurrentHashVersion), want);
    }
}

TEST_CASE("TMEM hash: a texture that uses few palette entries still hashes only those (the bitset path)") {
    // All indices 0..3: the palette part of the hash covers 4 entries, whether the bytes were scanned or marked.
    for (uint8_t &b : tmemBuffer) {
        b = 0x12;
    }

    LoadTile tile = {};
    tile.siz = G_IM_SIZ_8b;
    tile.fmt = G_IM_FMT_CI;
    tile.line = 64;
    const uint64_t shortTile = TMEMHasher::hash(tmemBuffer, tile, 16, 8, 0x8000, 5);
    const uint64_t tallTile = TMEMHasher::hash(tmemBuffer, tile, 16, 3000, 0x8000, 5);
    CHECK_EQ(shortTile, ReferenceHasher::referenceHash(tmemBuffer, tile, 16, 8, 0x8000, 5));
    CHECK_EQ(tallTile, ReferenceHasher::referenceHash(tmemBuffer, tile, 16, 3000, 0x8000, 5));
    CHECK(shortTile != tallTile);   // the height is part of the hash
}
