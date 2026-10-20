//
// RT64
//

// [wcw] Unit tests for common/rt64_wcw_jabo (Jabo Direct3D8 texture pack names matched at runtime).

#include "common/rt64_wcw_jabo.h"

#include <cstring>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace RT64::WcwJabo;

namespace {
    std::string md5Hex(const std::string &text) {
        return toHex(md5(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    }
}

TEST_CASE("jabo: MD5 matches the RFC 1321 test suite") {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("jabo: MD5 across the 55/56/64-byte padding edges") {
    // Lengths where the length field moves to a second padding block; checked against Python's hashlib.
    CHECK(md5Hex(std::string(55, 'x')) == "04364420e25c512fd958a70738aa8f72");
    CHECK(md5Hex(std::string(56, 'x')) == "668a72d5ba17f08e62dabcafad6db14b");
    CHECK(md5Hex(std::string(64, 'x')) == "c1bb4f81d892b2d57947682aeb252456");
}

TEST_CASE("jabo: file names are 32 hex digits with a .png or .dds extension") {
    Digest digest;
    // The example name from EmuTalk's 2008 Rice/Jabo thread.
    CHECK(parseFileName("243A26F6D4999E2347CEFB528D124466.png", digest));
    CHECK(toHex(digest) == "243a26f6d4999e2347cefb528d124466");
    CHECK(parseFileName("243a26f6d4999e2347cefb528d124466.PNG", digest));
    CHECK(parseFileName("243A26F6D4999E2347CEFB528D124466.dds", digest));
    CHECK(!parseFileName("243A26F6D4999E2347CEFB528D124466.bmp", digest));
    CHECK(!parseFileName("243A26F6D4999E2347CEFB528D12446.png", digest));
    CHECK(!parseFileName("243A26F6D4999E2347CEFB528D1244666.png", digest));
    CHECK(!parseFileName("243A26F6D4999E2347CEFB528D12446G.png", digest));
    CHECK(!parseFileName("WWF No Mercy#11111111#2#1_all.png", digest));
}

TEST_CASE("jabo: index, first file in path order wins, any folder") {
    Index index;
    CHECK(index.add("Arenas/Raw/0A7362AE0836847DBE924B8F2CB67647.png"));
    CHECK(index.add("Zz/0a7362ae0836847dbe924b8f2cb67647.png"));
    CHECK(index.add("397DF732A49563757EDA3203319E976F.png"));
    CHECK(!index.add("Arenas/readme.txt"));
    CHECK(!index.add("Arenas/Raw/0A7362AE0836847DBE924B8F2CB67647.psd"));
    CHECK_EQ(index.size(), size_t(2));

    Digest digest;
    CHECK(parseFileName("0A7362AE0836847DBE924B8F2CB67647.png", digest));
    const std::string *path = index.find(digest);
    CHECK((path != nullptr) && (*path == "Arenas/Raw/0A7362AE0836847DBE924B8F2CB67647.png"));
    CHECK(parseFileName("00000000000000000000000000000000.png", digest));
    CHECK(index.find(digest) == nullptr);
}

TEST_CASE("jabo: single colors to A1R5G5B5") {
    // RGBA5551 red, opaque -> alpha bit on top, then R5 G5 B5.
    CHECK_EQ(fromRGBA16(0xF801), uint16_t(0xFC00));
    // Transparent white.
    CHECK_EQ(fromRGBA16(0xFFFE), uint16_t(0x7FFF));
    CHECK_EQ(fromRGBA16(0x0001), uint16_t(0x8000));
    CHECK_EQ(fromIntensity(0xFF, true), uint16_t(0xFFFF));
    CHECK_EQ(fromIntensity(0xFF, false), uint16_t(0x7FFF));
    CHECK_EQ(fromIntensity(0x08, true), uint16_t(0x8000 | (1 << 10) | (1 << 5) | 1));
}

TEST_CASE("jabo: decode of each texel size, little-endian rows, stride skipped") {
    std::vector<uint8_t> out;

    // RGBA16 2x1, then a padding pair the stride skips, then the second row.
    const uint8_t rgba16[] = { 0xF8, 0x01, 0x07, 0xC1, 0xAA, 0xAA, 0x00, 0x3F, 0xFF, 0xFE };
    CHECK(decodeA1R5G5B5(FmtRGBA, Siz16b, rgba16, 2, 1, 4, nullptr, out));
    const uint8_t rgba16Out[] = { 0x00, 0xFC, 0xE0, 0x83 };
    CHECK((out.size() == 4) && (memcmp(out.data(), rgba16Out, 4) == 0));
    CHECK(decodeA1R5G5B5(FmtRGBA, Siz16b, rgba16, 2, 2, 6, nullptr, out));
    CHECK_EQ(out.size(), size_t(8));
    // Row 2 starts 6 bytes in: 0x003F, 0xFFFE.
    CHECK_EQ(uint16_t(out[4] | (out[5] << 8)), fromRGBA16(0x003F));
    CHECK_EQ(uint16_t(out[6] | (out[7] << 8)), fromRGBA16(0xFFFE));

    // CI4: high nibble first, palette entries big-endian.
    uint8_t palette[32] = {};
    palette[2] = 0xF8; palette[3] = 0x01;   // entry 1: red
    palette[30] = 0x07; palette[31] = 0xC1; // entry 15: green
    const uint8_t ci4[] = { 0x1F };
    CHECK(decodeA1R5G5B5(FmtCI, Siz4b, ci4, 2, 1, 1, palette, out));
    CHECK_EQ(uint16_t(out[0] | (out[1] << 8)), uint16_t(0xFC00));
    CHECK_EQ(uint16_t(out[2] | (out[3] << 8)), fromRGBA16(0x07C1));
    // CI without a palette, CI16, 32-bit and YUV aren't decoded.
    CHECK(!decodeA1R5G5B5(FmtCI, Siz4b, ci4, 2, 1, 1, nullptr, out));
    CHECK(!decodeA1R5G5B5(FmtCI, Siz16b, rgba16, 1, 1, 2, palette, out));
    CHECK(!decodeA1R5G5B5(FmtRGBA, Siz32b, rgba16, 1, 1, 4, nullptr, out));
    CHECK(!decodeA1R5G5B5(FmtYUV, Siz16b, rgba16, 1, 1, 2, nullptr, out));

    // IA4: 3-bit intensity, 1-bit alpha. 0xF = full intensity, opaque; 0xE = full, transparent.
    const uint8_t ia4[] = { 0xFE };
    CHECK(decodeA1R5G5B5(FmtIA, Siz4b, ia4, 2, 1, 1, nullptr, out));
    CHECK_EQ(uint16_t(out[0] | (out[1] << 8)), uint16_t(0xFFFF));
    CHECK_EQ(uint16_t(out[2] | (out[3] << 8)), uint16_t(0x7FFF));

    // IA8: 4-bit intensity, 4-bit alpha (>= 8 = opaque). I8: intensity, opaque.
    const uint8_t ia8[] = { 0xF8, 0xF7 };
    CHECK(decodeA1R5G5B5(FmtIA, Siz8b, ia8, 2, 1, 2, nullptr, out));
    CHECK_EQ(uint16_t(out[0] | (out[1] << 8)), uint16_t(0xFFFF));
    CHECK_EQ(uint16_t(out[2] | (out[3] << 8)), uint16_t(0x7FFF));
    const uint8_t i8[] = { 0x80 };
    CHECK(decodeA1R5G5B5(FmtI, Siz8b, i8, 1, 1, 1, nullptr, out));
    CHECK_EQ(uint16_t(out[0] | (out[1] << 8)), fromIntensity(0x80, true));

    // IA16: intensity byte, alpha byte (>= 0x80 = opaque).
    const uint8_t ia16[] = { 0xFF, 0x80, 0xFF, 0x7F };
    CHECK(decodeA1R5G5B5(FmtIA, Siz16b, ia16, 2, 1, 4, nullptr, out));
    CHECK_EQ(uint16_t(out[0] | (out[1] << 8)), uint16_t(0xFFFF));
    CHECK_EQ(uint16_t(out[2] | (out[3] << 8)), uint16_t(0x7FFF));
}

TEST_CASE("jabo: a texture's digest is the MD5 of its A1R5G5B5 bytes") {
    const uint8_t rgba16[] = { 0xF8, 0x01, 0x07, 0xC1 };
    Digest digest;
    CHECK(textureDigest(FmtRGBA, Siz16b, rgba16, 2, 1, 4, nullptr, digest));
    const uint8_t expected[] = { 0x00, 0xFC, 0xE0, 0x83 };
    CHECK(digest == md5(expected, sizeof(expected)));
    CHECK(!textureDigest(FmtRGBA, Siz32b, rgba16, 1, 1, 4, nullptr, digest));
}
