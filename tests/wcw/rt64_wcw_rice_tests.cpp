//
// RT64
//

// [wcw] Unit tests for common/rt64_wcw_rice (Rice / GLideN64 texture pack names matched at runtime, aki-roadmap#93).

#include "common/rt64_wcw_rice.h"

#include <cstring>
#include <vector>

#include "wcw_test.h"

using namespace RT64::WcwRice;

namespace {
    // The texture_hasher form of the CRC (while loops), for rows of 4 bytes or more where both forms agree.
    uint32_t referenceCRC(const uint8_t *src, int width, int height, int size, int rowStride) {
        uint32_t crc = 0;
        const int bytesPerLine = width << size >> 1;
        for (int y = height - 1; y >= 0; y--) {
            uint32_t esi = 0;
            for (int x = bytesPerLine - 4; x >= 0; x -= 4) {
                memcpy(&esi, src + x, 4);
                esi ^= uint32_t(x);
                crc = (crc << 4) + ((crc >> 28) & 15);
                crc += esi;
            }

            esi ^= uint32_t(y);
            crc += esi;
            src += rowStride;
        }

        return crc;
    }

    std::vector<uint8_t> pattern(size_t size, uint32_t seed) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; i++) {
            seed = seed * 1664525u + 1013904223u;
            bytes[i] = uint8_t(seed >> 24);
        }

        return bytes;
    }
}

TEST_CASE("rice: parses the GLideN64 name of a CI8 texture with a palette CRC") {
    ParsedName parsed;
    CHECK(parseFileName("WWF No Mercy#0EF51D29#2#1#70556D40_ciByRGBA.png", parsed));
    CHECK(parsed.romName == "WWF No Mercy");
    CHECK_EQ(parsed.key.crc, 0x0EF51D29u);
    CHECK_EQ(parsed.key.paletteCrc, 0x70556D40u);
    CHECK_EQ(int(parsed.key.fmt), 2);
    CHECK_EQ(int(parsed.key.siz), 1);
    CHECK_EQ(int(parsed.key.kind), int(KeyPalette));
    CHECK(parsed.suffix == Suffix::CiByRGBA);
    CHECK(keyToString(parsed.key) == "0ef51d29#2#1#70556d40");
}

TEST_CASE("rice: parses names without a palette, wildcards and every suffix, case-insensitively") {
    ParsedName parsed;
    CHECK(parseFileName("GAME#DEADBEEF#0#2_all.png", parsed));
    CHECK_EQ(int(parsed.key.kind), int(KeyNoPalette));
    CHECK(parsed.suffix == Suffix::All);
    CHECK(parseFileName("game#deadbeef#0#2_RGB.PNG", parsed) && (parsed.suffix == Suffix::Rgb));
    CHECK(parseFileName("game#deadbeef#0#2_a.bmp", parsed) && (parsed.suffix == Suffix::Alpha));
    CHECK(parseFileName("game#deadbeef#2#0#12345678_allciByRGBA.png", parsed) && (parsed.suffix == Suffix::AllCiByRGBA));
    CHECK(parseFileName("game#deadbeef#2#0#12345678_ci.bmp", parsed) && (parsed.suffix == Suffix::Ci));
    CHECK(parseFileName("game#deadbeef#2#1#$_ciByRGBA.png", parsed));
    CHECK_EQ(int(parsed.key.kind), int(KeyAnyPalette));
    CHECK(parseFileName("game#$#2#1#12345678_ciByRGBA.png", parsed));
    CHECK_EQ(int(parsed.key.kind), int(KeyAnyTexture));
    CHECK_EQ(parsed.key.paletteCrc, 0x12345678u);
    // A ROM name with an underscore still parses (the suffix is after the last '_').
    CHECK(parseFileName("MY_GAME#00000001#0#2_all.png", parsed) && (parsed.romName == "MY_GAME"));
}

TEST_CASE("rice: rejects names that aren't Rice names") {
    ParsedName parsed;
    CHECK(!parseFileName("upper arm.png", parsed));
    CHECK(!parseFileName("0123456789abcdef.png", parsed));
    CHECK(!parseFileName("WWF No Mercy#5DD4FA12#2#0#D8C606D2_ciByRGBA.png.png", parsed));
    CHECK(!parseFileName("game#deadbeef#2#1#70556D40_ciByRGBA.txt", parsed));
    CHECK(!parseFileName("game#deadbeef#2#1#70556D40_other.png", parsed));
    CHECK(!parseFileName("game#deadbee#2#1_all.png", parsed));
    CHECK(!parseFileName("game#deadbeef#2#4_all.png", parsed));
    CHECK(!parseFileName("game#deadbeef#2_all.png", parsed));
    // GLideN64 refuses a zero CRC unless it's a wildcard.
    CHECK(!parseFileName("game#00000000#2#1_all.png", parsed));
    CHECK(!parseFileName("game#$#2#1_all.png", parsed));
}

TEST_CASE("rice: index lookup order, first file wins, _a ignored, whole texture beats _rgb") {
    Index index;
    CHECK(index.add("Pack/A/G#11111111#2#1#22222222_ciByRGBA.png"));
    CHECK(index.add("Pack/B/G#11111111#2#1#22222222_ciByRGBA.png"));
    CHECK(index.add("G#33333333#0#2_rgb.png"));
    CHECK(index.add("G#33333333#0#2_a.png"));
    CHECK(index.add("G#33333333#0#2_all.png"));
    CHECK(index.add("G#44444444#2#0_all.png"));
    CHECK(index.add("G#55555555#2#1#$_all.png"));
    CHECK(index.add("G#$#2#0#66666666_all.png"));
    CHECK(!index.add("readme.txt"));
    CHECK_EQ(index.size(), size_t(5));

    const std::string *path = index.find(0x11111111, 0x22222222, 2, 1, true);
    CHECK((path != nullptr) && (*path == "Pack/A/G#11111111#2#1#22222222_ciByRGBA.png"));
    // Wrong palette, wrong fmt or siz: no match.
    CHECK(index.find(0x11111111, 0x22222223, 2, 1, true) == nullptr);
    CHECK(index.find(0x11111111, 0x22222222, 2, 0, true) == nullptr);
    CHECK(index.find(0x11111111, 0x22222222, 3, 1, true) == nullptr);

    path = index.find(0x33333333, 0, 0, 2, false);
    CHECK((path != nullptr) && (*path == "G#33333333#0#2_all.png"));

    // A paletted texture falls back to a name without the palette CRC, the crc#$ wildcard and the $#palcrc wildcard.
    path = index.find(0x44444444, 0x99999999, 2, 0, true);
    CHECK((path != nullptr) && (*path == "G#44444444#2#0_all.png"));
    path = index.find(0x55555555, 0x12345678, 2, 1, true);
    CHECK((path != nullptr) && (*path == "G#55555555#2#1#$_all.png"));
    path = index.find(0x77777777, 0x66666666, 2, 0, true);
    CHECK((path != nullptr) && (*path == "G#$#2#0#66666666_all.png"));
}

TEST_CASE("rice: a paletted texture matches a name with a zero palette CRC (Rice Video's IA + TLUT names)") {
    // TNA vs ROH names IA8 textures drawn with a TLUT "crc#3#1#00000000"; the game's palette CRC is not zero.
    Index index;
    CHECK(index.add("WWF No Mercy#0B8FDC27#3#1#00000000_ciByRGBA.png"));
    const std::string *path = index.find(0x0B8FDC27, 0x4F18C34B, 3, 1, true);
    CHECK((path != nullptr) && (*path == "WWF No Mercy#0B8FDC27#3#1#00000000_ciByRGBA.png"));
    // The exact palette still wins over the zero one.
    CHECK(index.add("WWF No Mercy#0B8FDC27#3#1#4F18C34B_ciByRGBA.png"));
    path = index.find(0x0B8FDC27, 0x4F18C34B, 3, 1, true);
    CHECK((path != nullptr) && (*path == "WWF No Mercy#0B8FDC27#3#1#4F18C34B_ciByRGBA.png"));
}

TEST_CASE("rice: CRC of hand-computed buffers") {
    // One row of one word: the word rotated in (crc was 0), then the last word XOR row 0 added again.
    uint8_t one[8] = { 0, 0, 0, 0, 0x44, 0x33, 0x22, 0x11 };
    CHECK_EQ(riceCRC32(one + 4, 2, 1, 2, 4), 0x22446688u);

    // Two rows of one word each, read from the last row up: row 1 = A, row 0 = B (rowStride 4 walks forward
    // through src, so the first row read is src[0..3] with y = 1).
    uint32_t words[2] = { 0x00000010u, 0x00000100u };
    // y = 1: crc = 0x10; += 0x10 ^ 1 = 0x11 -> 0x21. y = 0: crc = rot4(0x21) + 0x100 = 0x310; += 0x100 -> 0x410.
    CHECK_EQ(riceCRC32(reinterpret_cast<const uint8_t *>(words), 2, 2, 2, 4), 0x410u);

    // Words are XORed with their byte offset within the row.
    uint32_t row[2] = { 0, 0 };
    // x = 4: esi = 4 -> crc 4. x = 0: crc = 0x40 + 0 = 0x40. Last esi (0) ^ y (0) -> 0x40.
    CHECK_EQ(riceCRC32(reinterpret_cast<const uint8_t *>(row), 4, 1, 2, 8), 0x40u);
}

TEST_CASE("rice: CRC matches texture_hasher's form for every size, stride and odd sizes") {
    const std::vector<uint8_t> bytes = pattern(4096, 7);
    for (int size = 0; size <= 3; size++) {
        for (int width : { 8, 16, 24, 32, 64 }) {
            for (int height : { 1, 2, 7, 32 }) {
                const int bpl = width << size >> 1;
                for (int stride : { bpl, bpl + 8 }) {
                    if ((stride * height) > 4000) continue;
                    CHECK_EQ(riceCRC32(bytes.data() + 16, width, height, size, stride), referenceCRC(bytes.data() + 16, width, height, size, stride));
                }
            }
        }
    }
}

TEST_CASE("rice: a row under 4 bytes still reads one word before it (Rice's do-while loop)") {
    // 2 bytes per row (one 16-bit palette entry): x starts at -2, so the word read is src[-2..1].
    uint8_t buffer[8] = { 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 0, 0 };
    uint32_t expectedWord;
    memcpy(&expectedWord, buffer + 2, 4);
    const uint32_t esi = expectedWord ^ uint32_t(-2);
    CHECK_EQ(riceCRC32(buffer + 4, 1, 1, 2, 2), esi + esi);
}

TEST_CASE("rice: CI max index scans") {
    const uint8_t ci8[8] = { 1, 7, 3, 0, 9, 2, 0xFE, 4 };
    CHECK_EQ(int(maxCI8(ci8, 4, 1, 4)), 7);
    CHECK_EQ(int(maxCI8(ci8, 4, 2, 4)), 0xFE);
    CHECK_EQ(int(maxCI8(ci8, 2, 2, 4)), 9);
    const uint8_t ci4[4] = { 0x12, 0x30, 0x0A, 0x45 };
    CHECK_EQ(int(maxCI4(ci4, 4, 1, 2)), 3);
    CHECK_EQ(int(maxCI4(ci4, 4, 2, 2)), 0xA);
    const uint8_t full[2] = { 0xF0, 0x00 };
    CHECK_EQ(int(maxCI4(full, 4, 1, 2)), 0xF);
}

TEST_CASE("rice: geometry of a tile load") {
    // 64x32 CI8 texture image, load tile (0,0)-(63,31) in 10.2 fixed point.
    Load load;
    load.type = LoadType::Tile;
    load.siz = 1;
    load.uls = 0; load.ult = 0; load.lrs = 63 << 2; load.lrt = 31 << 2;
    load.address = 0x1000;
    load.textureSiz = 1;
    load.textureWidth = 64;
    DrawTile draw;
    draw.fmt = 2; draw.siz = 1;
    Geometry g;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.width, 64);
    CHECK_EQ(g.height, 32);
    CHECK_EQ(g.bytesPerRow, 64);
    CHECK_EQ(g.rdramStart, 0x1000u);
    CHECK_EQ(g.bytesRead, uint64_t(31 * 64 + 64));

    // Offset rectangle (8,4)-(39,19) of a 128-wide image: start at row 4, byte 8; mask clamps the width.
    load.uls = 8 << 2; load.ult = 4 << 2; load.lrs = 39 << 2; load.lrt = 19 << 2;
    load.textureWidth = 128;
    load.masks = 4;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.width, 16);
    CHECK_EQ(g.height, 16);
    CHECK_EQ(g.bytesPerRow, 128);
    CHECK_EQ(g.rdramStart, 0x1000u + 4 * 128 + 8);

    // Loaded as 16-bit, drawn as 8-bit: the width doubles.
    load.masks = 0;
    load.siz = 2; load.textureSiz = 2;
    load.uls = 0; load.ult = 0; load.lrs = 15 << 2; load.lrt = 7 << 2;
    load.textureWidth = 16;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.width, 32);
    CHECK_EQ(g.height, 8);
}

TEST_CASE("rice: geometry of a block load, with and without dxt") {
    Load load;
    load.type = LoadType::Block;
    load.siz = 1;
    load.address = 0x2000;
    load.textureSiz = 1;
    load.textureWidth = 1;
    DrawTile draw;
    draw.fmt = 2; draw.siz = 1;
    draw.line = 4;        // 32 bytes per row
    draw.lrs = 31 << 2; draw.lrt = 15 << 2;
    draw.masks = 5; draw.maskt = 4;
    Geometry g;

    // dxt 0: bytes per row come from the tile's line.
    load.lrt = 0;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.width, 32);
    CHECK_EQ(g.height, 16);
    CHECK_EQ(g.bytesPerRow, 32);
    CHECK_EQ(g.rdramStart, 0x2000u);

    // dxt for 4 words per row (2048 / 4 = 512): reversed back to 4 words = 32 bytes.
    load.lrt = 512;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.bytesPerRow, 32);

    // 32-bit textures: line << 4.
    draw.siz = 3;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.bytesPerRow, 64);

    // Wrapping (not clamped) tile larger than its mask: the mask size wins.
    draw.siz = 1;
    draw.cms = 0; draw.masks = 3;
    CHECK(computeGeometry(load, draw, g));
    CHECK_EQ(g.width, 8);

    // A TLUT load or no load has no texture geometry.
    load.type = LoadType::TLUT;
    CHECK(!computeGeometry(load, draw, g));
    load.type = LoadType::None;
    CHECK(!computeGeometry(load, draw, g));
}

TEST_CASE("rice: palette entries come from the newest TLUT load covering them") {
    std::vector<PaletteLoad> loads;
    uint32_t address = 0;
    CHECK(!paletteEntryAddress(loads, 0, address));

    // Full 256-entry palette at 0x1000, then bank 2 (entries 32..47) reloaded from 0x5000.
    loads.push_back({ 0, 256, 0x1000, 1 });
    loads.push_back({ 32, 16, 0x5000, 2 });
    CHECK(paletteEntryAddress(loads, 5, address) && (address == 0x1000u + 10));
    CHECK(paletteEntryAddress(loads, 33, address) && (address == 0x5000u + 2));
    CHECK(paletteEntryAddress(loads, 48, address) && (address == 0x1000u + 96));

    // A newer full load covers bank 2 again.
    loads.push_back({ 0, 256, 0x9000, 3 });
    CHECK(paletteEntryAddress(loads, 33, address) && (address == 0x9000u + 66));
    CHECK(!paletteEntryAddress(loads, 256, address));
}

TEST_CASE("rice: palette CRC hashes cimax + 1 entries from the bank's first entry") {
    std::vector<uint8_t> palette(4 + 512, 0);
    const std::vector<uint8_t> bytes = pattern(512, 3);
    memcpy(palette.data() + 4, bytes.data(), 512);
    // CI8, 200 entries: RiceCRC32(palette, 200, 1, 2, 512).
    CHECK_EQ(paletteCRC(palette.data(), 0, 200, false), referenceCRC(bytes.data(), 200, 1, 2, 512));
    // CI4 bank 3, 16 entries.
    CHECK_EQ(paletteCRC(palette.data(), 48, 16, true), referenceCRC(bytes.data() + 96, 16, 1, 2, 32));
}

TEST_CASE("rice: which textures carry a palette CRC") {
    CHECK(usesPalette(2, 0, true));
    CHECK(usesPalette(2, 1, false));
    CHECK(usesPalette(3, 1, true));
    CHECK(!usesPalette(3, 1, false));
    CHECK(!usesPalette(0, 2, true));
    CHECK(!usesPalette(2, 2, true));
}
