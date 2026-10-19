//
// RT64
//

// [wcw] Rice / GLideN64 hi-res texture pack names, matched at runtime (aki-roadmap#93). Legacy N64 texture packs name
// each PNG after the texture's "Rice CRC": <rom name>#<crc32>#<fmt>#<siz>[#<palette crc32>]_<suffix>.png. This file
// holds the pure parts, unit tested in tests/wcw/rt64_wcw_rice_tests.cpp:
//   - parsing those file names (and GLideN64's '$' wildcards);
//   - an index of a pack's files by key, with GLideN64's lookup and duplicate rules;
//   - the Rice CRC itself over a byte buffer, the CI max index scan, and the texture geometry (width, height, bytes per
//     row, RDRAM start) Rice derives from the last load into the tile's TMEM address;
//   - which TLUT load supplies each palette entry (GLideN64 keeps a 256-entry copy of every palette loaded).
// The renderer (hle/rt64_rdp_tmem.cpp, render/rt64_texture_cache.cpp) gathers the inputs and calls these.
//
// The hashing follows GLideN64 (src/GLideNHQ/TxUtil.cpp RiceCRC32 / checksum64, src/Textures.cpp _loadHiresTexture,
// src/GLideNHQ/TxHiResLoader.cpp checkFileName), which cites Rice Video. Upstream RT64 already ships the same
// computation in src/tools/texture_hasher (GPL, see GLIDEN64-LICENSE there).

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RT64 {
    namespace WcwRice {
        // A pack file's key. Flags say which parts are present or wildcards ('$' in the name).
        enum KeyKind : uint8_t {
            KeyNoPalette = 0,      // crc#fmt#siz
            KeyPalette = 1,        // crc#fmt#siz#palcrc
            KeyAnyPalette = 2,     // crc#fmt#siz#$
            KeyAnyTexture = 3,     // $#fmt#siz#palcrc
        };

        struct Key {
            uint32_t crc = 0;
            uint32_t paletteCrc = 0;
            uint8_t fmt = 0;
            uint8_t siz = 0;
            uint8_t kind = KeyNoPalette;

            bool operator==(const Key &other) const {
                return (crc == other.crc) && (paletteCrc == other.paletteCrc) && (fmt == other.fmt) && (siz == other.siz) && (kind == other.kind);
            }
        };

        struct KeyHasher {
            size_t operator()(const Key &key) const {
                uint64_t v = (uint64_t(key.crc) << 32) ^ uint64_t(key.paletteCrc) ^ (uint64_t(key.fmt) << 8) ^ (uint64_t(key.siz) << 4) ^ uint64_t(key.kind);
                v ^= v >> 33;
                v *= 0xff51afd7ed558ccdULL;
                v ^= v >> 33;
                return size_t(v);
            }
        };

        // File name suffixes (GLideN64 TxHiResLoader): _all / _ciByRGBA / _allciByRGBA hold the whole texture; _rgb
        // the color with an optional _a alpha twin; _ci is a paletted BMP.
        enum class Suffix : uint8_t {
            All,
            CiByRGBA,
            AllCiByRGBA,
            Rgb,
            Alpha,
            Ci,
        };

        // The Rice key computed for a texture at upload time (hle/rt64_rdp_tmem.cpp), kept with the texture so packs
        // enabled later can match it too.
        struct TextureKey {
            bool valid = false;
            bool hasPalette = false;
            uint8_t fmt = 0;
            uint8_t siz = 0;
            uint32_t crc = 0;
            uint32_t paletteCrc = 0;
        };

        struct ParsedName {
            std::string romName;   // The part before the first '#' (the ROM's internal name, e.g. "WWF No Mercy").
            Key key;
            Suffix suffix = Suffix::All;
        };

        // Parses a file name (no directory) in Rice's convention, case-insensitively, with a .png/.bmp/.dds
        // extension. Returns false for anything else, and, like GLideN64, for a zero CRC without a wildcard.
        bool parseFileName(const std::string &fileName, ParsedName &parsed);

        // "crc#fmt#siz[#palcrc]" in lowercase hex (wildcards as '$'), for logs.
        std::string keyToString(const Key &key);
        std::string textureKeyToString(const TextureKey &key);

        // Case-insensitive path order (the order GLideN64 meets files in on Windows).
        bool pathLess(const std::string &a, const std::string &b);

        // WCW_RICE_LOG=1 logs every Rice match and every texture's computed key (otherwise only the first matches).
        bool logEnabled();

        // A texture pack's Rice-named files. Feed it the pack's relative paths; like GLideN64, the first file added for
        // a key wins (add them in case-insensitive path order), _a alpha files are ignored (no RGB + A merge: the _rgb
        // file is used alone) and a whole-texture file beats an _rgb one for the same key.
        class Index {
        public:
            // Returns true when the path was a Rice name (whether or not it won its key).
            bool add(const std::string &relativePath);
            bool empty() const { return entries.empty(); }
            size_t size() const { return entries.size(); }

            // The best file for a texture: exact key with palette, crc#fmt#siz#00000000 (Rice Video names TLUT textures
            // in non-CI formats with a zero palette CRC), crc#fmt#siz#$, crc#fmt#siz (packs that name CI
            // textures without a palette CRC), then $#fmt#siz#palcrc. hasPalette = the texture is paletted (CI4/CI8 or
            // TLUT on). Returns nullptr when nothing matches.
            const std::string *find(uint32_t crc, uint32_t paletteCrc, uint8_t fmt, uint8_t siz, bool hasPalette) const;

        private:
            struct Entry {
                std::string path;
                Suffix suffix;
            };

            std::unordered_map<Key, Entry, KeyHasher> entries;
        };

        // Rice's CRC (RiceCRC32 in GLideN64's TxUtil.cpp, Rice Video's CalculateRDRAMCRC): rows from the last to the first,
        // 32-bit words from the end of the row to its start, each XORed with its byte offset, rotated in; the row's
        // last word XOR its row index is added again. Words are read in host order from the emulator's
        // byte-swapped RDRAM (the recomp keeps RDRAM in the same layout). Like the original do-while loop, a row
        // shorter than 4 bytes still reads one word, at a negative offset: src must stay readable 4 bytes before.
        uint32_t riceCRC32(const uint8_t *src, int width, int height, int size, int rowStride);

        // Highest palette index a CI4 / CI8 texture uses (GLideN64 CalculateMaxCI4b / CalculateMaxCI8b), over raw
        // RDRAM bytes as stored.
        uint8_t maxCI4(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride);
        uint8_t maxCI8(const uint8_t *src, uint32_t width, uint32_t height, uint32_t rowStride);

        // The last load into the tile's TMEM address (rdp->rice.lastLoadOpByTMEM), reduced to what Rice uses.
        enum class LoadType : uint8_t { None, Tile, Block, TLUT };
        struct Load {
            LoadType type = LoadType::None;
            // Load tile (10.2 fixed point coordinates as the RDP gets them; a block load's lrt holds dxt).
            uint8_t siz = 0;
            uint8_t masks = 0;
            uint8_t maskt = 0;
            uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;
            // Texture image (SetTextureImage).
            uint32_t address = 0;
            uint8_t textureSiz = 0;
            uint16_t textureWidth = 0;
        };

        // The tile the texture is drawn with.
        struct DrawTile {
            uint8_t fmt = 0;
            uint8_t siz = 0;
            uint16_t line = 0;
            uint8_t palette = 0;
            uint8_t cms = 0, cmt = 0;
            uint8_t masks = 0, maskt = 0;
            uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;
        };

        struct Geometry {
            int width = 0;
            int height = 0;
            int bytesPerRow = 0;
            uint32_t rdramStart = 0;
            // Bytes from rdramStart that the CRC reads: (height - 1) rows plus one row of the texture.
            uint64_t bytesRead = 0;
        };

        // The rectangle Rice hashes (GLideN64 _loadHiresTexture). False for no load, a TLUT load or empty sizes.
        bool computeGeometry(const Load &load, const DrawTile &drawTile, Geometry &geometry);

        // Whether a texture's key carries a palette CRC: 4/8-bit texels with a TLUT or the CI format (GLideN64).
        bool usesPalette(uint8_t fmt, uint8_t siz, bool tlutEnabled);

        // A TLUT load seen at palette TMEM entry 'firstEntry' (TMEM word - 256): 'count' 16-bit entries from
        // RDRAM 'address', loaded in order 'sequence' (higher = newer).
        struct PaletteLoad {
            uint32_t firstEntry = 0;
            uint32_t count = 0;
            uint32_t address = 0;
            uint64_t sequence = 0;
        };

        // GLideN64 copies every TLUT load into a 256-entry palette (gDP.TexFilterPalette) and hashes from there. For
        // an entry index, the newest load covering it gives its RDRAM byte address. False when no load covers it.
        bool paletteEntryAddress(const std::vector<PaletteLoad> &loads, uint32_t entry, uint32_t &address);

        // Palette CRC over entries [first, first + count) of that palette (copied into 'palette', 2 bytes each, raw
        // RDRAM byte order): RiceCRC32(palette + first, cimax + 1, 1, 2, stride) as GLideN64 does. 'palette' is the
        // full 256-entry copy plus a 4-byte zero guard in front (see riceCRC32), so pass 4 + 512 bytes.
        uint32_t paletteCRC(const uint8_t *paletteWithGuard, uint32_t firstEntry, uint32_t entryCount, bool ci4);
    };
};
