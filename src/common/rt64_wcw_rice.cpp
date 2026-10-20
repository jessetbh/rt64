//
// RT64
//

// [wcw] Rice / GLideN64 texture pack names (aki-roadmap#93). See rt64_wcw_rice.h. The hashing follows GLideN64
// (GPL; upstream RT64's texture_hasher carries the same code and licence note).

#include "rt64_wcw_rice.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace RT64 {
    namespace WcwRice {
        static std::string toLower(std::string str) {
            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            return str;
        }

        static bool parseHex(const std::string &str, size_t digits, uint32_t &value) {
            if ((digits == 0) || (str.size() != digits)) {
                return false;
            }

            value = 0;
            for (char ch : str) {
                const int c = std::tolower((unsigned char)(ch));
                uint32_t d;
                if ((c >= '0') && (c <= '9')) {
                    d = uint32_t(c - '0');
                }
                else if ((c >= 'a') && (c <= 'f')) {
                    d = uint32_t(c - 'a' + 10);
                }
                else {
                    return false;
                }

                value = (value << 4) | d;
            }

            return true;
        }

        bool parseFileName(const std::string &fileName, ParsedName &parsed) {
            const std::string lower = toLower(fileName);
            const size_t dot = lower.find_last_of('.');
            if (dot == std::string::npos) {
                return false;
            }

            const std::string extension = lower.substr(dot);
            if ((extension != ".png") && (extension != ".bmp") && (extension != ".dds")) {
                return false;
            }

            const size_t firstHash = lower.find('#');
            const size_t underscore = lower.find_last_of('_');
            if ((firstHash == std::string::npos) || (underscore == std::string::npos) || (underscore < firstHash) || (underscore > dot)) {
                return false;
            }

            const std::string suffix = lower.substr(underscore + 1, dot - underscore - 1);
            if (suffix == "all") parsed.suffix = Suffix::All;
            else if (suffix == "cibyrgba") parsed.suffix = Suffix::CiByRGBA;
            else if (suffix == "allcibyrgba") parsed.suffix = Suffix::AllCiByRGBA;
            else if (suffix == "rgb") parsed.suffix = Suffix::Rgb;
            else if (suffix == "a") parsed.suffix = Suffix::Alpha;
            else if (suffix == "ci") parsed.suffix = Suffix::Ci;
            else return false;

            // Fields between the first '#' and the suffix: crc, fmt, siz and the optional palette crc.
            std::vector<std::string> fields;
            size_t start = firstHash + 1;
            const std::string body = lower.substr(0, underscore);
            while (true) {
                const size_t next = body.find('#', start);
                if (next == std::string::npos) {
                    fields.emplace_back(body.substr(start));
                    break;
                }

                fields.emplace_back(body.substr(start, next - start));
                start = next + 1;
            }

            if ((fields.size() != 3) && (fields.size() != 4)) {
                return false;
            }

            Key key;
            if ((fields[1] == "$") && (fields[2] == "$")) {
                // crc#$#$[#palcrc]: any format and size.
                if (!parseHex(fields[0], 8, key.crc) || (key.crc == 0)) {
                    return false;
                }

                if (fields.size() == 3) {
                    key.kind = KeyAnyFormat;
                }
                else if (parseHex(fields[3], 8, key.paletteCrc)) {
                    key.kind = KeyAnyFormatPalette;
                }
                else {
                    return false;
                }

                parsed.romName = fileName.substr(0, firstHash);
                parsed.key = key;
                return true;
            }

            uint32_t fmt = 0, siz = 0;
            if (!parseHex(fields[1], 1, fmt) || !parseHex(fields[2], 1, siz) || (siz > 3)) {
                return false;
            }

            key.fmt = uint8_t(fmt);
            key.siz = uint8_t(siz);
            const bool crcWildcard = (fields[0] == "$");
            if (crcWildcard) {
                // Only "$#fmt#siz#palcrc".
                if ((fields.size() != 4) || !parseHex(fields[3], 8, key.paletteCrc)) {
                    return false;
                }

                key.kind = KeyAnyTexture;
            }
            else {
                if (!parseHex(fields[0], 8, key.crc) || (key.crc == 0)) {
                    return false;
                }

                if (fields.size() == 3) {
                    key.kind = KeyNoPalette;
                }
                else if (fields[3] == "$") {
                    key.kind = KeyAnyPalette;
                }
                else if (parseHex(fields[3], 8, key.paletteCrc)) {
                    key.kind = KeyPalette;
                }
                else {
                    return false;
                }
            }

            parsed.romName = fileName.substr(0, firstHash);
            parsed.key = key;
            return true;
        }

        std::string keyToString(const Key &key) {
            char buf[48];
            switch (key.kind) {
            case KeyPalette:
                snprintf(buf, sizeof(buf), "%08x#%u#%u#%08x", key.crc, key.fmt, key.siz, key.paletteCrc);
                break;
            case KeyAnyPalette:
                snprintf(buf, sizeof(buf), "%08x#%u#%u#$", key.crc, key.fmt, key.siz);
                break;
            case KeyAnyTexture:
                snprintf(buf, sizeof(buf), "$#%u#%u#%08x", key.fmt, key.siz, key.paletteCrc);
                break;
            case KeyAnyFormat:
                snprintf(buf, sizeof(buf), "%08x#$#$", key.crc);
                break;
            case KeyAnyFormatPalette:
                snprintf(buf, sizeof(buf), "%08x#$#$#%08x", key.crc, key.paletteCrc);
                break;
            default:
                snprintf(buf, sizeof(buf), "%08x#%u#%u", key.crc, key.fmt, key.siz);
                break;
            }

            return std::string(buf);
        }

        std::string textureKeyToString(const TextureKey &textureKey) {
            Key key;
            key.crc = textureKey.crc;
            key.paletteCrc = textureKey.paletteCrc;
            key.fmt = textureKey.fmt;
            key.siz = textureKey.siz;
            key.kind = textureKey.hasPalette ? KeyPalette : KeyNoPalette;
            return keyToString(key);
        }

        bool pathLess(const std::string &a, const std::string &b) {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
                return std::tolower(x) < std::tolower(y);
            });
        }

        bool logEnabled() {
            static const bool enabled = []() {
                const char *value = getenv("WCW_RICE_LOG");
                return (value != nullptr) && (value[0] != '\0') && (value[0] != '0');
            }();

            return enabled;
        }

        bool Index::add(const std::string &relativePath) {
            const size_t slash = relativePath.find_last_of("/\\");
            const std::string fileName = (slash == std::string::npos) ? relativePath : relativePath.substr(slash + 1);
            ParsedName parsed;
            if (!parseFileName(fileName, parsed)) {
                return false;
            }

            if (parsed.suffix == Suffix::Alpha) {
                const std::string lower = toLower(relativePath);
                const size_t suffix = lower.rfind("_a.");
                if ((suffix != std::string::npos) && (lower.compare(lower.size() - 4, 4, ".png") == 0)) {
                    alphaTwins.emplace(lower.substr(0, suffix), relativePath);
                }

                return true;
            }

            auto it = entries.find(parsed.key);
            if (it == entries.end()) {
                entries.emplace(parsed.key, Entry{ relativePath, parsed.suffix });
            }
            else if ((it->second.suffix == Suffix::Rgb) && (parsed.suffix != Suffix::Rgb)) {
                // A whole-texture file beats an _rgb file without its alpha.
                it->second = Entry{ relativePath, parsed.suffix };
            }

            return true;
        }

        const std::string *Index::alphaFor(const std::string &rgbPath) const {
            if (!isRgbFile(rgbPath)) {
                return nullptr;
            }

            const std::string lower = toLower(rgbPath);
            auto it = alphaTwins.find(lower.substr(0, lower.rfind("_rgb.")));
            return (it != alphaTwins.end()) ? &it->second : nullptr;
        }

        bool isRgbFile(const std::string &relativePath) {
            const size_t slash = relativePath.find_last_of("/\\");
            ParsedName parsed;
            return parseFileName((slash == std::string::npos) ? relativePath : relativePath.substr(slash + 1), parsed) && (parsed.suffix == Suffix::Rgb);
        }

        void mergeAlpha(uint8_t *rgba, const uint8_t *alphaRgba, size_t pixelCount) {
            for (size_t i = 0; i < pixelCount; i++) {
                rgba[i * 4 + 3] = alphaRgba[i * 4];
            }
        }

        void makeOpaque(uint8_t *rgba, size_t pixelCount) {
            for (size_t i = 0; i < pixelCount; i++) {
                rgba[i * 4 + 3] = 0xFF;
            }
        }

        const std::string *Index::find(uint32_t crc, uint32_t paletteCrc, uint8_t fmt, uint8_t siz, bool hasPalette) const {
            auto lookup = [&](uint32_t c, uint32_t p, uint8_t kind) -> const std::string * {
                Key key;
                key.crc = c;
                key.paletteCrc = p;
                const bool anyFormat = (kind == KeyAnyFormat) || (kind == KeyAnyFormatPalette);
                key.fmt = anyFormat ? 0 : fmt;
                key.siz = anyFormat ? 0 : siz;
                key.kind = kind;
                auto it = entries.find(key);
                return (it != entries.end()) ? &it->second.path : nullptr;
            };

            const std::string *path = nullptr;
            if (hasPalette) {
                if ((path = lookup(crc, paletteCrc, KeyPalette)) != nullptr) return path;
                // Packs dumped with Rice Video name TLUT textures in non-CI formats (IA8 + TLUT in No Mercy) with a
                // zero palette CRC (TNA vs ROH: all 29 such files).
                if ((paletteCrc != 0) && (path = lookup(crc, 0, KeyPalette)) != nullptr) return path;
                if ((path = lookup(crc, 0, KeyAnyPalette)) != nullptr) return path;
                if ((path = lookup(crc, 0, KeyNoPalette)) != nullptr) return path;
                if ((path = lookup(0, paletteCrc, KeyAnyTexture)) != nullptr) return path;
                if ((path = lookup(crc, paletteCrc, KeyAnyFormatPalette)) != nullptr) return path;
                if ((path = lookup(crc, 0, KeyAnyFormat)) != nullptr) return path;
            }
            else {
                if ((path = lookup(crc, 0, KeyNoPalette)) != nullptr) return path;
                if ((path = lookup(crc, 0, KeyAnyPalette)) != nullptr) return path;
                if ((path = lookup(crc, 0, KeyAnyFormat)) != nullptr) return path;
            }

            return nullptr;
        }

        uint32_t riceCRC32(const uint8_t *src, int width, int height, int size, int rowStride) {
            uint32_t crc = 0;
            const int bytesPerLine = int(uint32_t(width) << size >> 1);
            int y = height - 1;
            do {
                uint32_t esi = 0;
                int x = bytesPerLine - 4;
                do {
                    memcpy(&esi, src + x, sizeof(esi));
                    esi ^= uint32_t(x);
                    crc = (crc << 4) + ((crc >> 28) & 15);
                    crc += esi;
                    x -= 4;
                } while (x >= 0);

                esi ^= uint32_t(y);
                crc += esi;
                src += rowStride;
                --y;
            } while (y >= 0);

            return crc;
        }

        uint8_t maxCI8(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride) {
            uint8_t val = 0;
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t *buf = src + rowStride * y;
                for (uint32_t x = 0; x < width; ++x) {
                    val = std::max(val, buf[x]);
                    if (val == 0xFF) {
                        return val;
                    }
                }
            }

            return val;
        }

        uint8_t maxCI4(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride) {
            uint8_t val = 0;
            width >>= 1;
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t *buf = src + rowStride * y;
                for (uint32_t x = 0; x < width; ++x) {
                    val = std::max(val, uint8_t(buf[x] >> 4));
                    val = std::max(val, uint8_t(buf[x] & 0xF));
                    if (val == 0xF) {
                        return val;
                    }
                }
            }

            return val;
        }

        static uint32_t calculateDXT(uint32_t txl2words) {
            return (txl2words == 0) ? 1U : (2048 + txl2words - 1) / txl2words;
        }

        static uint32_t txl2Words(uint32_t width, uint32_t size) {
            static const uint32_t sizeBytes[4] = { 0, 1, 2, 4 };
            return (size == 0) ? std::max(1U, width / 16) : std::max(1U, width * sizeBytes[size & 3] / 8);
        }

        static uint32_t reverseDXT(uint32_t val, uint32_t width, uint32_t size) {
            if (val == 0x800) {
                return 1;
            }

            int low = 2047 / int(val);
            if (calculateDXT(uint32_t(low)) > val) {
                low++;
            }

            const int high = 2047 / int(val - 1);
            if (low == high) {
                return uint32_t(low);
            }

            for (int i = low; i <= high; i++) {
                if (txl2Words(width, size) == uint32_t(i)) {
                    return uint32_t(i);
                }
            }

            return uint32_t((low + high) / 2);
        }

        bool computeGeometry(const Load &load, const DrawTile &drawTile, Geometry &geometry) {
            int width = 0, height = 0, bpl = 0;
            uint32_t start = 0;
            const uint32_t bytesOffset = uint32_t(load.uls >> 2) << load.textureSiz >> 1;
            const uint32_t bytesPerRow = uint32_t(load.textureWidth) << load.textureSiz >> 1;
            if (load.type == LoadType::Tile) {
                int tileWidth = (std::max((load.lrs >> 2) - (load.uls >> 2), 0) + 1) & 0x03FF;
                const int tileHeight = (std::max((load.lrt >> 2) - (load.ult >> 2), 0) + 1) & 0x03FF;
                if (load.masks != 0) {
                    tileWidth = std::min(tileWidth, 1 << load.masks);
                }

                bpl = int(bytesPerRow);
                width = std::min(tileWidth, int(load.textureWidth));
                if (load.siz > drawTile.siz) {
                    width <<= (load.siz - drawTile.siz);
                }

                height = (load.maskt != 0) ? std::min(tileHeight, 1 << load.maskt) : tileHeight;
                start = load.address + bytesOffset + bytesPerRow * uint32_t(load.ult >> 2);
            }
            else if (load.type == LoadType::Block) {
                const int tileWidth = std::max((drawTile.lrs >> 2) - (drawTile.uls >> 2), 0) + 1;
                const int tileHeight = std::max((drawTile.lrt >> 2) - (drawTile.ult >> 2), 0) + 1;
                const int maskWidth = (drawTile.masks == 0) ? tileWidth : (1 << drawTile.masks);
                const int maskHeight = (drawTile.maskt == 0) ? tileHeight : (1 << drawTile.maskt);
                const bool clampS = (drawTile.masks == 0) || (drawTile.cms & 2);
                const bool clampT = (drawTile.maskt == 0) || (drawTile.cmt & 2);
                width = (clampS && (tileWidth <= 256)) ? std::min(maskWidth, tileWidth) : maskWidth;
                height = ((clampT && (tileHeight <= 256)) || (maskHeight > 256)) ? std::min(maskHeight, tileHeight) : maskHeight;
                if (drawTile.siz == 3) {
                    bpl = int(drawTile.line) << 4;
                }
                else if (load.lrt == 0) {
                    bpl = int(drawTile.line) << 3;
                }
                else {
                    uint32_t dxt = load.lrt;
                    if (dxt > 1) {
                        dxt = reverseDXT(dxt, load.textureWidth, load.textureSiz);
                    }

                    bpl = int(dxt << 3);
                }

                start = load.address + bytesOffset + bytesPerRow * uint32_t(load.ult);
            }
            else {
                return false;
            }

            if ((width <= 0) || (height <= 0) || (bpl <= 0)) {
                return false;
            }

            geometry.width = width;
            geometry.height = height;
            geometry.bytesPerRow = bpl;
            geometry.rdramStart = start;
            geometry.bytesRead = uint64_t(height - 1) * uint64_t(bpl) + (uint64_t(width) << drawTile.siz >> 1);
            return true;
        }

        bool usesPalette(uint8_t fmt, uint8_t siz, bool tlutEnabled) {
            return (siz < 2) && (tlutEnabled || (fmt == 2));
        }

        bool paletteEntryAddress(const std::vector<PaletteLoad> &loads, uint32_t entry, uint32_t &address) {
            const PaletteLoad *best = nullptr;
            for (const PaletteLoad &load : loads) {
                if ((entry >= load.firstEntry) && (entry < (load.firstEntry + load.count))) {
                    if ((best == nullptr) || (load.sequence > best->sequence)) {
                        best = &load;
                    }
                }
            }

            if (best == nullptr) {
                return false;
            }

            address = best->address + (entry - best->firstEntry) * 2;
            return true;
        }

        uint32_t paletteCRC(const uint8_t *paletteWithGuard, uint32_t firstEntry, uint32_t entryCount, bool ci4) {
            return riceCRC32(paletteWithGuard + 4 + firstEntry * 2, int(entryCount), 1, 2, ci4 ? 32 : 512);
        }
    };
};
