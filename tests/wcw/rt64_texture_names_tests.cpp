//
// RT64
//

// [wcw] Unit tests for what rt64 itself keeps of foreign texture pack names (aki-roadmap#93): the provider registry and
// the BMP check. The matching lives in the provider and is tested there.

#include "common/rt64_texture_names.h"
#include "common/rt64_wcw_logic.h"

#include <vector>

#include "wcw_test.h"

using namespace RT64;
using WcwLogic::isBmp;

namespace {
    class NullProvider : public TextureNameProvider {
    public:
        void computeKey(const TextureNameInput &, TextureNameKey &) const override {}
        std::unique_ptr<TextureNamePackIndex> createPackIndex(int, const std::vector<std::string> &) const override { return nullptr; }
        void applySeparateAlpha(uint8_t *, const uint8_t *, size_t) const override {}
    };

    // A BMP file header ("BM", size, reserved, pixel offset) followed by a DIB header of dibSize bytes (only its size field).
    std::vector<uint8_t> bmpHeader(uint32_t dibSize) {
        std::vector<uint8_t> b(14 + 40, 0);
        b[0] = 'B';
        b[1] = 'M';
        b[14] = uint8_t(dibSize);
        b[15] = uint8_t(dibSize >> 8);
        b[16] = uint8_t(dibSize >> 16);
        b[17] = uint8_t(dibSize >> 24);
        return b;
    }
}

TEST_CASE("texture names: no provider until one is installed, and it can be removed (#93)") {
    CHECK(textureNameProvider() == nullptr);
    NullProvider provider;
    setTextureNameProvider(&provider);
    CHECK(textureNameProvider() == &provider);
    setTextureNameProvider(nullptr);
    CHECK(textureNameProvider() == nullptr);
}

TEST_CASE("texture names: a new texture's key starts out invalid") {
    TextureNameKey key;
    CHECK(!key.valid);
}

TEST_CASE("bmp: Windows and OS/2 BMP headers are recognized, so .bmp packs load (#93)") {
    CHECK(isBmp(bmpHeader(40).data(), 54));  // BITMAPINFOHEADER (what Rice Video and Paint write)
    CHECK(isBmp(bmpHeader(108).data(), 54)); // BITMAPV4HEADER
    CHECK(isBmp(bmpHeader(124).data(), 54)); // BITMAPV5HEADER (what Pillow writes for RGBA)
    CHECK(isBmp(bmpHeader(12).data(), 54));  // OS/2 BITMAPCOREHEADER
}

TEST_CASE("bmp: PNG, DDS, truncated files and a bogus DIB size are not BMPs") {
    const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D', 'R', 0, 0 };
    const uint8_t dds[] = { 'D', 'D', 'S', ' ', 124, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!isBmp(png, sizeof(png)));
    CHECK(!isBmp(dds, sizeof(dds)));
    CHECK(!isBmp(bmpHeader(40).data(), 17));
    CHECK(!isBmp(nullptr, 0));
    CHECK(!isBmp(bmpHeader(0).data(), 54));
    CHECK(!isBmp(bmpHeader(500).data(), 54));
}
