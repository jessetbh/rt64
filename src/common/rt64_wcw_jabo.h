//
// RT64
//

// [wcw] Jabo Direct3D8 hi-res texture pack names, matched at runtime (fullconv: Showdown 64 and the other Project64-era
// No Mercy packs ship in this format). A Jabo pack names each PNG after an MD5 digest, 32 hex digits
// ("243A26F6D4999E2347CEFB528D124466.png"), in any folder of the pack.
//
// The plugin is closed source and the scheme isn't published. What is known:
//   - EmuTalk, "Converting between Rice and Jabo texture packs" (2008, t-43838): Jabo's names are an MD5 (Cyberman),
//     the example name above (q00u), and that a Jabo/Rice table can only be built by dumping with both plugins (Gonetz).
//   - Gadge64's No Mercy rt64 fork (d714e96d, computeJaboKey) matched textures the game draws against a Jabo pack:
//     the digest is the MD5 of the texture decoded to A1R5G5B5, little-endian, row by row, at the tile's width and
//     height. CI4/CI8 with RGBA16 palettes and IA4 were confirmed there; the other 4/8/16-bit formats follow the same
//     decode. 32-bit textures aren't covered (no confirmed rule).
// This file holds the pure parts, unit tested in tests/wcw/rt64_wcw_jabo_tests.cpp: MD5 (RFC 1321), the A1R5G5B5
// decode, file names and a pack index. hle/rt64_rdp_tmem.cpp gathers the texels from RDRAM and
// render/rt64_texture_cache.cpp does the lookup.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RT64 {
    namespace WcwJabo {
        struct Digest {
            uint8_t bytes[16] = {};

            bool operator==(const Digest &other) const;
        };

        struct DigestHasher {
            size_t operator()(const Digest &digest) const;
        };

        // MD5 (RFC 1321).
        Digest md5(const uint8_t *data, size_t size);

        // 32 lowercase hex digits.
        std::string toHex(const Digest &digest);

        // A Jabo file name (no directory): exactly 32 hex digits (any case) and a .png or .dds extension. rt64 can't
        // load the BMPs some old packs carry, so those don't count.
        bool parseFileName(const std::string &fileName, Digest &digest);

        // A pack's Jabo-named files. Add them in case-insensitive path order: the first file for a digest wins.
        class Index {
        public:
            // True when the path was a Jabo name (whether or not it won its digest).
            bool add(const std::string &relativePath);
            const std::string *find(const Digest &digest) const;
            bool empty() const { return entries.empty(); }
            size_t size() const { return entries.size(); }

        private:
            std::unordered_map<Digest, std::string, DigestHasher> entries;
        };

        // N64 texel formats and sizes (G_IM_FMT_* / G_IM_SIZ_*).
        enum : uint8_t { FmtRGBA = 0, FmtYUV = 1, FmtCI = 2, FmtIA = 3, FmtI = 4 };
        enum : uint8_t { Siz4b = 0, Siz8b = 1, Siz16b = 2, Siz32b = 3 };

        // Single colors to A1R5G5B5 (bit 15 alpha), as Jabo's decode does.
        uint16_t fromRGBA16(uint16_t rgba5551);
        uint16_t fromIntensity(uint32_t intensity8, bool opaque);

        // Decodes a texture to A1R5G5B5, little-endian, row by row: 'width' x 'height' texels from 'texels', N64 byte
        // order (big-endian, as on the cartridge, not the recomp's swapped RDRAM), rows 'rowStride' bytes apart.
        // 'palette' is the CI palette in the same byte order: 16 RGBA16 entries from the tile's bank for CI4, 256 for
        // CI8 (nullptr otherwise). False for 32-bit, YUV or empty sizes.
        bool decodeA1R5G5B5(uint8_t fmt, uint8_t siz, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t rowStride,
            const uint8_t *palette, std::vector<uint8_t> &out);

        // The texture's Jabo digest: MD5 of decodeA1R5G5B5's output.
        bool textureDigest(uint8_t fmt, uint8_t siz, const uint8_t *texels, uint32_t width, uint32_t height, uint32_t rowStride,
            const uint8_t *palette, Digest &digest);
    };
};
