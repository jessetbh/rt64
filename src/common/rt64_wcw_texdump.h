//
// RT64
//

// [wcw] Texture export for players (aki-roadmap#48): decode a texture from TMEM on the CPU, write it as a PNG named by
// its rt64 hash, and recognise such files in a texture pack. Pure logic with unit tests (tests/wcw/rt64_wcw_texdump_tests.cpp).
//
// The decoder is the CPU twin of sampleTMEM in shaders/TextureDecoder.hlsli (the compute shader that decodes the same
// TMEM bytes for rendering), so a dumped PNG shows exactly what the game draws before any filtering.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RT64 {
    namespace WcwTexDump {
        // Values of the RDP's fmt / siz / tlut fields (shared/rt64_f3d_defines.h).
        constexpr uint32_t FmtRGBA = 0;
        constexpr uint32_t FmtYUV = 1;
        constexpr uint32_t FmtCI = 2;
        constexpr uint32_t FmtIA = 3;
        constexpr uint32_t FmtI = 4;
        constexpr uint32_t Siz4b = 0;
        constexpr uint32_t Siz8b = 1;
        constexpr uint32_t Siz16b = 2;
        constexpr uint32_t Siz32b = 3;
        constexpr uint32_t TlutNone = 0;
        constexpr uint32_t TlutRGBA16 = (2 << 14);
        constexpr uint32_t TlutIA16 = (3 << 14);
        constexpr uint32_t TmemBytes = 0x1000;

        // The texture the GPU decodes: format, TMEM address and stride in bytes (LoadTile tmem << 3, line << 3),
        // palette bank (CI4) and the TLUT mode (0, TlutRGBA16 or TlutIA16).
        struct TileFormat {
            uint32_t fmt = FmtRGBA;
            uint32_t siz = Siz16b;
            uint32_t address = 0;
            uint32_t stride = 0;
            uint32_t tlut = TlutNone;
            uint32_t palette = 0;
        };

        // One texel as RGBA8, the same value TextureDecodeCS.hlsl writes (every format decodes to exact 8-bit values).
        void sampleTexel(const uint8_t *tmem, int32_t x, int32_t y, const TileFormat &tile, uint8_t rgba[4]);

        // The whole texture, row by row, 4 bytes per texel. tmem holds TmemBytes bytes.
        std::vector<uint8_t> decodeTexture(const uint8_t *tmem, uint32_t width, uint32_t height, const TileFormat &tile);

        // RGBA8 pixels as a PNG file (miniz). Empty on failure.
        std::vector<uint8_t> encodePNG(const uint8_t *rgba, uint32_t width, uint32_t height);

        // "0123456789abcdef.png": the file name a dumped texture gets, and the name a pack uses to replace it.
        std::string hashFileName(uint64_t hash);

        // A pack file named "<16 hex digits>.png" or ".dds" (any case, in any folder) replaces the texture with that
        // rt64 hash, even without an entry in rt64.json. Returns false for any other name.
        bool hashFromFileName(const std::string &fileName, uint64_t &hash);

        // rt64.json for a pack made of hash-named files: no texture entries, autoPath rt64, the current hash version.
        std::string packDatabaseJson(uint32_t hashVersion);

        // The folder inside a dump directory that holds the PNGs and rt64.json (a ready-to-zip pack).
        constexpr const char *PackFolderName = "pack";
    };
};
