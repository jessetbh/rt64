//
// RT64
//

#include "rt64_wcw_texdump.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>

#include <miniz/miniz.h>

namespace RT64 {
    namespace WcwTexDump {
        static const uint32_t TmemPalette = 0x800;
        static const uint32_t TmemMask8 = 0xFFF;
        static const uint32_t TmemMask16 = 0x7FF;

        static void set(uint8_t rgba[4], uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
            rgba[0] = uint8_t(r);
            rgba[1] = uint8_t(g);
            rgba[2] = uint8_t(b);
            rgba[3] = uint8_t(a);
        }

        // Formats.hlsli conversions, as 8-bit values.
        static void fromI4(uint32_t i4, uint8_t rgba[4]) {
            const uint32_t i = (i4 << 4) | i4;
            set(rgba, i, i, i, i);
        }

        static void fromIA4(uint32_t ia4, uint8_t rgba[4]) {
            uint32_t i = ia4 & 0xE;
            i = ((i << 4) | (i << 1) | (i >> 2)) & 0xFF;
            set(rgba, i, i, i, (ia4 & 1) ? 255 : 0);
        }

        static void fromI8(uint32_t i, uint8_t rgba[4]) {
            set(rgba, i & 0xFF, i & 0xFF, i & 0xFF, i & 0xFF);
        }

        static void fromIA8(uint32_t ia8, uint8_t rgba[4]) {
            uint32_t i = (ia8 >> 4) & 0xF;
            uint32_t a = ia8 & 0xF;
            i = (i << 4) | i;
            a = (a << 4) | a;
            set(rgba, i, i, i, a);
        }

        static void fromRGBA16(uint32_t c, uint8_t rgba[4]) {
            const uint32_t r = (c >> 11) & 0x1F;
            const uint32_t g = (c >> 6) & 0x1F;
            const uint32_t b = (c >> 1) & 0x1F;
            set(rgba, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (c & 1) ? 255 : 0);
        }

        static void fromIA16(uint32_t ia16, uint8_t rgba[4]) {
            const uint32_t i = (ia16 >> 8) & 0xFF;
            set(rgba, i, i, i, ia16 & 0xFF);
        }

        // implLoadTMEM: odd rows swap 32-bit words within the row (the RDP's interleaving).
        static uint32_t loadTMEM(const uint8_t *tmem, uint32_t relativeAddress, uint32_t maskAddress, uint32_t orAddress, bool oddRow, const TileFormat &tile) {
            uint32_t finalAddress = tile.address + relativeAddress;
            if (oddRow) {
                const uint32_t rowSize = tile.stride;
                const uint32_t rowStart = (rowSize > 0) ? (relativeAddress / rowSize) * rowSize : 0;
                const uint32_t wordIndex = (relativeAddress - rowStart) / 4;
                finalAddress = tile.address + rowStart + ((wordIndex ^ 1) * 4) + (relativeAddress & 0x3);
            }

            return tmem[((finalAddress & maskAddress) | orAddress) & TmemMask8];
        }

        void sampleTexel(const uint8_t *tmem, int32_t x, int32_t y, const TileFormat &tile, uint8_t rgba[4]) {
            const bool oddRow = (y & 1);
            const bool oddColumn = (x & 1);
            const bool isRgba32 = (tile.fmt == FmtRGBA) && (tile.siz == Siz32b);
            const bool usesTlut = (tile.tlut > 0);
            const uint32_t tmemShift = isRgba32 ? 2 : tile.siz;
            const uint32_t addressMask = (isRgba32 || usesTlut) ? TmemMask16 : TmemMask8;
            const uint32_t pixelAddress = uint32_t(y) * tile.stride + ((uint32_t(x) << tmemShift) >> 1);
            const uint32_t v0 = loadTMEM(tmem, pixelAddress + 0, addressMask, 0, oddRow, tile);
            const uint32_t v1 = loadTMEM(tmem, pixelAddress + 1, addressMask, 0, oddRow, tile);
            const uint32_t v4 = (v0 >> (oddColumn ? 0 : 4)) & 0xF;
            if (usesTlut) {
                const uint32_t paletteAddress = (tile.siz == Siz4b)
                    ? TmemPalette + (tile.palette << 7) + (v4 << 3)
                    : TmemPalette + (v0 << 3);
                const uint32_t value = tmem[(paletteAddress + 1) & TmemMask8] | (uint32_t(tmem[paletteAddress & TmemMask8]) << 8);
                if (tile.tlut == TlutRGBA16) {
                    fromRGBA16(value, rgba);
                }
                else if (tile.tlut == TlutIA16) {
                    fromIA16(value, rgba);
                }
                else {
                    set(rgba, 0, 0, 0, 255);
                }

                return;
            }

            const uint32_t pixelAddress2 = isRgba32 ? pixelAddress : pixelAddress + 2;
            const uint32_t orAddress = isRgba32 ? (TmemBytes >> 1) : 0;
            const uint32_t v2 = loadTMEM(tmem, pixelAddress2 + 0, addressMask, orAddress, oddRow, tile);
            const uint32_t v3 = loadTMEM(tmem, pixelAddress2 + 1, addressMask, orAddress, oddRow, tile);
            switch (tile.siz) {
            case Siz4b:
                switch (tile.fmt) {
                case FmtCI:
                    // Not a real format: the palette bank becomes the upper bits.
                    fromI8((tile.palette << 4) | v4, rgba);
                    return;
                case FmtIA:
                    fromIA4(v4, rgba);
                    return;
                case FmtI:
                case FmtRGBA:
                    fromI4(v4, rgba);
                    return;
                default:
                    break;
                }
                break;
            case Siz8b:
                switch (tile.fmt) {
                case FmtIA:
                    fromIA8(v0, rgba);
                    return;
                case FmtI:
                case FmtRGBA:
                case FmtCI:
                    fromI8(v0, rgba);
                    return;
                default:
                    break;
                }
                break;
            case Siz16b:
                switch (tile.fmt) {
                case FmtRGBA:
                    fromRGBA16(v1 | (v0 << 8), rgba);
                    return;
                case FmtIA:
                    fromIA16(v1 | (v0 << 8), rgba);
                    return;
                case FmtCI:
                case FmtI:
                    // Not a real format: decoded as IA and stored as IAIA.
                    set(rgba, v0, v1, v0, v1);
                    return;
                default:
                    break;
                }
                break;
            case Siz32b:
                switch (tile.fmt) {
                case FmtRGBA:
                    set(rgba, v0, v1, v2, v3);
                    return;
                case FmtCI:
                case FmtIA:
                case FmtI:
                    // Not a real format: RGRG on odd columns, BABA on even ones (the shader's select order).
                    if (oddColumn) {
                        set(rgba, v0, v1, v0, v1);
                    }
                    else {
                        set(rgba, v2, v3, v2, v3);
                    }
                    return;
                default:
                    break;
                }
                break;
            default:
                break;
            }

            set(rgba, 0, 0, 0, 255);
        }

        std::vector<uint8_t> decodeTexture(const uint8_t *tmem, uint32_t width, uint32_t height, const TileFormat &tile) {
            std::vector<uint8_t> rgba(size_t(width) * height * 4);
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    sampleTexel(tmem, int32_t(x), int32_t(y), tile, &rgba[(size_t(y) * width + x) * 4]);
                }
            }

            return rgba;
        }

        std::vector<uint8_t> encodePNG(const uint8_t *rgba, uint32_t width, uint32_t height) {
            if ((rgba == nullptr) || (width == 0) || (height == 0)) {
                return {};
            }

            size_t pngSize = 0;
            void *png = tdefl_write_image_to_png_file_in_memory_ex(rgba, int(width), int(height), 4, &pngSize, 6, MZ_FALSE);
            if (png == nullptr) {
                return {};
            }

            std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t *>(png), reinterpret_cast<const uint8_t *>(png) + pngSize);
            mz_free(png);
            return bytes;
        }

        std::string hashFileName(uint64_t hash) {
            char name[32];
            snprintf(name, sizeof(name), "%016" PRIx64 ".png", hash);
            return name;
        }

        bool hashFromFileName(const std::string &fileName, uint64_t &hash) {
            // Only the file name counts, not its folder.
            const size_t slash = fileName.find_last_of("/\\");
            const std::string name = (slash == std::string::npos) ? fileName : fileName.substr(slash + 1);
            if (name.size() != 20) {
                return false;
            }

            std::string extension = name.substr(16);
            for (char &c : extension) {
                c = char(std::tolower(uint8_t(c)));
            }

            if ((extension != ".png") && (extension != ".dds")) {
                return false;
            }

            uint64_t value = 0;
            for (size_t i = 0; i < 16; i++) {
                const char c = char(std::tolower(uint8_t(name[i])));
                uint32_t digit;
                if ((c >= '0') && (c <= '9')) {
                    digit = uint32_t(c - '0');
                }
                else if ((c >= 'a') && (c <= 'f')) {
                    digit = uint32_t(c - 'a' + 10);
                }
                else {
                    return false;
                }

                value = (value << 4) | digit;
            }

            hash = value;
            return true;
        }

        std::string packDatabaseJson(uint32_t hashVersion) {
            char text[512];
            snprintf(text, sizeof(text),
                "{\n"
                "    \"configuration\": {\n"
                "        \"autoPath\": \"rt64\",\n"
                "        \"configurationVersion\": 3,\n"
                "        \"defaultOperation\": \"stream\",\n"
                "        \"defaultShift\": \"half\",\n"
                "        \"hashVersion\": %u\n"
                "    },\n"
                "    \"textures\": []\n"
                "}\n", hashVersion);
            return text;
        }
    };
};
