//
// RT64
//

// [wcw] Unit tests for common/rt64_wcw_texdump (texture export for players, aki-roadmap#48) and for the texture pack
// loader's hash-named file fallback in ReplacementDatabase::resolvePaths.

#include "common/rt64_wcw_texdump.h"
#include "common/rt64_replacement_database.h"

#include <cstring>
#include <map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb/stb_image.h>

#include "wcw_test.h"

using namespace RT64;
using namespace RT64::WcwTexDump;

namespace {
    struct Tmem {
        uint8_t bytes[TmemBytes] = {};

        void put16(uint32_t address, uint16_t value) {
            bytes[address] = uint8_t(value >> 8);
            bytes[address + 1] = uint8_t(value & 0xFF);
        }
    };

    std::vector<uint8_t> texel(const Tmem &tmem, int x, int y, const TileFormat &tile) {
        std::vector<uint8_t> rgba(4);
        sampleTexel(tmem.bytes, x, y, tile, rgba.data());
        return rgba;
    }

    std::vector<uint8_t> rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return { r, g, b, a };
    }

    uint16_t rgba16(uint32_t r5, uint32_t g5, uint32_t b5, uint32_t a1) {
        return uint16_t((r5 << 11) | (g5 << 6) | (b5 << 1) | a1);
    }

    // A file system over a list of names, for resolvePaths.
    struct ListIterator : FileSystemIteratorImplementation {
        const std::vector<std::string> *names;
        size_t index;
        ListIterator(const std::vector<std::string> *names, size_t index) : names(names), index(index) {}
        const std::string &value() override { return (*names)[index]; }
        void increment() override { index++; }
        bool compare(const FileSystemIteratorImplementation *other) const override {
            return index == static_cast<const ListIterator *>(other)->index;
        }
    };

    struct ListFileSystem : FileSystem {
        std::vector<std::string> names;
        explicit ListFileSystem(std::vector<std::string> names) : names(std::move(names)) {}
        Iterator begin() const override { return Iterator{ std::make_shared<ListIterator>(&names, 0) }; }
        Iterator end() const override { return Iterator{ std::make_shared<ListIterator>(&names, names.size()) }; }
        bool load(const std::string &, uint8_t *, size_t) const override { return false; }
        size_t getSize(const std::string &) const override { return 0; }
        bool exists(const std::string &path) const override {
            for (const std::string &name : names) {
                if (name == path) {
                    return true;
                }
            }

            return false;
        }

        std::string makeCanonical(const std::string &path) const override { return path; }
    };

    std::map<uint64_t, std::string> resolve(const std::string &databaseJson, const std::vector<std::string> &files) {
        ReplacementDatabase db = json::parse(databaseJson);
        ListFileSystem fileSystem(files);
        std::unordered_map<uint64_t, ReplacementResolvedPath> resolved;
        db.resolvePaths(&fileSystem, 0, resolved, false);
        std::map<uint64_t, std::string> out;
        for (const auto &it : resolved) {
            out[it.first] = it.second.relativePath;
        }

        return out;
    }

    std::string get(const std::map<uint64_t, std::string> &resolved, uint64_t hash) {
        auto it = resolved.find(hash);
        return (it != resolved.end()) ? it->second : std::string();
    }
}

TEST_CASE("texdump: RGBA16 decodes 5-bit channels like Formats.hlsli, odd rows read their 32-bit words swapped") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtRGBA;
    tile.siz = Siz16b;
    tile.stride = 8;
    // Row 0: four texels in order.
    tmem.put16(0, rgba16(31, 0, 0, 1));
    tmem.put16(2, rgba16(0, 31, 0, 1));
    tmem.put16(4, rgba16(0, 0, 31, 0));
    tmem.put16(6, rgba16(16, 8, 4, 1));
    // Row 1: the RDP stores odd rows with each pair of 32-bit words swapped, so texels 0-1 sit in the second word.
    tmem.put16(8 + 4, rgba16(1, 2, 3, 1));
    tmem.put16(8 + 6, rgba16(4, 5, 6, 0));
    tmem.put16(8 + 0, rgba16(7, 8, 9, 1));
    tmem.put16(8 + 2, rgba16(10, 11, 12, 1));

    CHECK(texel(tmem, 0, 0, tile) == rgba(255, 0, 0, 255));
    CHECK(texel(tmem, 1, 0, tile) == rgba(0, 255, 0, 255));
    CHECK(texel(tmem, 2, 0, tile) == rgba(0, 0, 255, 0));
    // 16 -> (16 << 3) | (16 >> 2) = 132, 8 -> 66, 4 -> 33.
    CHECK(texel(tmem, 3, 0, tile) == rgba(132, 66, 33, 255));
    CHECK(texel(tmem, 0, 1, tile) == rgba(8, 16, 24, 255));
    CHECK(texel(tmem, 1, 1, tile) == rgba(33, 41, 49, 0));
    CHECK(texel(tmem, 2, 1, tile) == rgba(57, 66, 74, 255));
    CHECK(texel(tmem, 3, 1, tile) == rgba(82, 90, 99, 255));
}

TEST_CASE("texdump: the tile's TMEM address offsets every texel") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtRGBA;
    tile.siz = Siz16b;
    tile.stride = 8;
    tile.address = 0x100;
    tmem.put16(0x100 + 2, rgba16(31, 31, 31, 1));
    CHECK(texel(tmem, 1, 0, tile) == rgba(255, 255, 255, 255));
    CHECK(texel(tmem, 0, 0, tile) == rgba(0, 0, 0, 0));
}

TEST_CASE("texdump: CI4 with an RGBA16 TLUT reads the palette bank, high nibble first") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtCI;
    tile.siz = Siz4b;
    tile.stride = 8;
    tile.tlut = TlutRGBA16;
    tile.palette = 2;
    tmem.bytes[0] = 0x3A; // texel 0 = index 3, texel 1 = index 10
    // Palette entries live in the upper half at 8 bytes each (the RDP quadruplicates them), bank = 16 entries.
    tmem.put16(0x800 + (2 << 7) + (3 << 3), rgba16(31, 0, 0, 1));
    tmem.put16(0x800 + (2 << 7) + (10 << 3), rgba16(0, 0, 31, 1));
    tmem.put16(0x800 + (0 << 7) + (3 << 3), rgba16(0, 31, 0, 1)); // bank 0: must not be used
    CHECK(texel(tmem, 0, 0, tile) == rgba(255, 0, 0, 255));
    CHECK(texel(tmem, 1, 0, tile) == rgba(0, 0, 255, 255));
}

TEST_CASE("texdump: CI8 with an IA16 TLUT indexes the whole palette") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtCI;
    tile.siz = Siz8b;
    tile.stride = 8;
    tile.tlut = TlutIA16;
    tmem.bytes[1] = 200;
    tmem.put16(0x800 + (200 << 3), 0x80C0);
    CHECK(texel(tmem, 1, 0, tile) == rgba(0x80, 0x80, 0x80, 0xC0));
}

TEST_CASE("texdump: intensity formats (I4, IA4, I8, IA8, IA16)") {
    Tmem tmem;
    TileFormat tile;
    tile.stride = 8;

    tile.fmt = FmtI;
    tile.siz = Siz4b;
    tmem.bytes[0] = 0x5F;
    CHECK(texel(tmem, 0, 0, tile) == rgba(0x55, 0x55, 0x55, 0x55));
    CHECK(texel(tmem, 1, 0, tile) == rgba(0xFF, 0xFF, 0xFF, 0xFF));

    tile.fmt = FmtIA;
    // IA4: 3 bits intensity + 1 bit alpha. 0xF -> i = 0xE -> (0xE << 4) | (0xE << 1) | (0xE >> 2) = 0xFF (8 bits).
    tmem.bytes[0] = 0xF6;
    CHECK(texel(tmem, 0, 0, tile) == rgba(0xFF, 0xFF, 0xFF, 255));
    // 0x6 -> i = 0x6 -> 0x60 | 0x0C | 0x01 = 0x6D, alpha bit 0.
    CHECK(texel(tmem, 1, 0, tile) == rgba(0x6D, 0x6D, 0x6D, 0));

    tile.fmt = FmtI;
    tile.siz = Siz8b;
    tmem.bytes[0] = 0x42;
    CHECK(texel(tmem, 0, 0, tile) == rgba(0x42, 0x42, 0x42, 0x42));

    tile.fmt = FmtIA;
    tmem.bytes[0] = 0x3C;
    CHECK(texel(tmem, 0, 0, tile) == rgba(0x33, 0x33, 0x33, 0xCC));

    tile.siz = Siz16b;
    tmem.put16(0, 0x7F10);
    CHECK(texel(tmem, 0, 0, tile) == rgba(0x7F, 0x7F, 0x7F, 0x10));
}

TEST_CASE("texdump: RGBA32 keeps red/green in the low half of TMEM and blue/alpha in the high half") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtRGBA;
    tile.siz = Siz32b;
    tile.stride = 8;
    // Texel 1: 16-bit stride, so bytes 2-3 of each half.
    tmem.bytes[2] = 10;
    tmem.bytes[3] = 20;
    tmem.bytes[0x800 + 2] = 30;
    tmem.bytes[0x800 + 3] = 40;
    CHECK(texel(tmem, 1, 0, tile) == rgba(10, 20, 30, 40));
}

TEST_CASE("texdump: decodeTexture lays texels out row by row") {
    Tmem tmem;
    TileFormat tile;
    tile.fmt = FmtI;
    tile.siz = Siz8b;
    tile.stride = 8;
    tmem.bytes[1] = 7;
    tmem.bytes[8 + 4 + 2] = 9; // row 1, x = 2: odd row, word swapped
    const std::vector<uint8_t> image = decodeTexture(tmem.bytes, 3, 2, tile);
    CHECK_EQ(image.size(), size_t(3 * 2 * 4));
    CHECK_EQ(int(image[1 * 4]), 7);
    CHECK_EQ(int(image[(1 * 3 + 2) * 4]), 9);
}

TEST_CASE("texdump: encodePNG writes a PNG that decodes to the same RGBA pixels") {
    const uint32_t w = 5, h = 3;
    std::vector<uint8_t> pixels(w * h * 4);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = uint8_t(i * 37 + 11);
    }

    const std::vector<uint8_t> png = encodePNG(pixels.data(), w, h);
    CHECK(png.size() > 8);
    CHECK(std::memcmp(png.data(), "\x89PNG", 4) == 0);
    int dw = 0, dh = 0, channels = 0;
    stbi_uc *decoded = stbi_load_from_memory(png.data(), int(png.size()), &dw, &dh, &channels, 4);
    CHECK(decoded != nullptr);
    if (decoded != nullptr) {
        CHECK_EQ(dw, int(w));
        CHECK_EQ(dh, int(h));
        CHECK(std::memcmp(decoded, pixels.data(), pixels.size()) == 0);
        stbi_image_free(decoded);
    }

    CHECK(encodePNG(pixels.data(), 0, 3).empty());
}

TEST_CASE("texdump: hash file names round-trip, and only <16 hex>.png/.dds names count") {
    uint64_t hash = 0;
    CHECK(hashFileName(0x0001d0556bc980b5ULL) == std::string("0001d0556bc980b5.png"));
    CHECK(hashFromFileName(hashFileName(0xfedcba9876543210ULL), hash));
    CHECK_EQ(hash, 0xfedcba9876543210ULL);
    CHECK(hashFromFileName("textures/HUD/0001D0556BC980B5.DDS", hash));
    CHECK_EQ(hash, 0x0001d0556bc980b5ULL);
    CHECK(hashFromFileName("a\\b\\00000000000000ff.png", hash));
    CHECK_EQ(hash, 0xffULL);
    CHECK(!hashFromFileName("0001d0556bc980b5.v5.png", hash));
    CHECK(!hashFromFileName("0001d0556bc980b5.tmem", hash));
    CHECK(!hashFromFileName("0001d0556bc980b.png", hash));
    CHECK(!hashFromFileName("0001d0556bc980b5x.png", hash));
    CHECK(!hashFromFileName("0001d0556bc980g5.png", hash));
    CHECK(!hashFromFileName("WCW#61094ee7#4#1_all.png", hash));
    CHECK(!hashFromFileName("rt64.json", hash));
}

TEST_CASE("texdump: a dump's pack folder (hash-named PNGs, empty database) resolves every texture") {
    const auto resolved = resolve(packDatabaseJson(5), { "rt64.json", "0000000000000001.png", "sub/00000000000000AB.png", "notes.txt" });
    CHECK_EQ(resolved.size(), size_t(2));
    CHECK(get(resolved, 0x1) == std::string("0000000000000001.png"));
    CHECK(get(resolved, 0xab) == std::string("sub/00000000000000AB.png"));
}

TEST_CASE("texdump: database entries win over hash-named files, and a .dds wins over a .png of the same hash") {
    const std::string db = R"({
        "configuration": { "autoPath": "rt64", "configurationVersion": 3, "hashVersion": 5 },
        "textures": [ { "hashes": { "rt64": "0000000000000001", "rice": "" }, "path": "custom/one.png" } ]
    })";
    const auto resolved = resolve(db, { "custom/one.png", "0000000000000001.png", "0000000000000002.png", "0000000000000002.dds", "0000000000000003.dds", "0000000000000003.png" });
    CHECK(get(resolved, 0x1) == std::string("custom/one.png"));
    CHECK(get(resolved, 0x2) == std::string("0000000000000002.dds"));
    CHECK(get(resolved, 0x3) == std::string("0000000000000003.dds"));
}

TEST_CASE("replacement database: shift filters survive a save and load (to_json wrote them as \"operation\", #99)") {
    ReplacementDatabase db;
    ReplacementShiftFilter filter;
    filter.wildcard = "menus/*";
    filter.shift = ReplacementShift::None;
    db.shiftFilters.push_back(filter);
    const json saved = db;
    // Under "shift", the key every reader (also upstream rt64's) looks for.
    CHECK(saved["shiftFilters"][0].contains("shift"));
    CHECK(!saved["shiftFilters"][0].contains("operation"));
    const ReplacementDatabase loaded = json::parse(saved.dump());
    CHECK_EQ(loaded.shiftFilters.size(), size_t(1));
    CHECK(loaded.shiftFilters[0].wildcard == std::string("menus/*"));
    CHECK(loaded.shiftFilters[0].shift == ReplacementShift::None);
}

TEST_CASE("replacement database: shift filters saved before #99 (under \"operation\") still load") {
    const std::string text = R"({ "configuration": { "configurationVersion": 3, "hashVersion": 5 }, "textures": [],
        "shiftFilters": [ { "wildcard": "a/*", "operation": "auto" }, { "wildcard": "b/*", "shift": "none" } ] })";
    const ReplacementDatabase loaded = json::parse(text);
    CHECK_EQ(loaded.shiftFilters.size(), size_t(2));
    CHECK(loaded.shiftFilters[0].shift == ReplacementShift::Auto);
    CHECK(loaded.shiftFilters[1].shift == ReplacementShift::None);
}

TEST_CASE("texdump: Rice-mode packs don't pick up hash-named files") {
    const std::string db = R"({ "configuration": { "autoPath": "rice", "configurationVersion": 3, "hashVersion": 5 }, "textures": [] })";
    CHECK(resolve(db, { "0000000000000001.png" }).empty());
}
