//
// RT64
//

// [wcw] Texture pack names from other emulators (aki-roadmap#93): an interface and nothing else. Pack files named by
// another renderer's scheme instead of an rt64 hash are matched through a TextureNameProvider, which the application
// may install with setTextureNameProvider. rt64 gathers the facts below and carries the opaque key; how a key is
// computed and how a pack's names are indexed is entirely up to the provider. Without one, rt64 does no such matching.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RT64 {
    // The last load into a texture's TMEM address, as the RDP received it.
    struct TextureNameLoad {
        enum class Type : uint8_t { None, Tile, Block };
        Type type = Type::None;
        uint8_t siz = 0;
        uint8_t masks = 0;
        uint8_t maskt = 0;
        uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;
        uint32_t address = 0;       // SetTextureImage address.
        uint8_t textureSiz = 0;
        uint16_t textureWidth = 0;
    };

    // The tile a texture is drawn with.
    struct TextureNameDrawTile {
        uint8_t fmt = 0;
        uint8_t siz = 0;
        uint16_t line = 0;
        uint8_t palette = 0;
        uint8_t cms = 0, cmt = 0;
        uint8_t masks = 0, maskt = 0;
        uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;
    };

    // A TLUT load seen in the palette half of TMEM: 'count' 16-bit entries from RDRAM 'address', starting at palette
    // entry 'firstEntry'; a higher 'sequence' is a newer load.
    struct TextureNamePaletteLoad {
        uint32_t firstEntry = 0;
        uint32_t count = 0;
        uint32_t address = 0;
        uint64_t sequence = 0;
    };

    struct TextureNameInput {
        const uint8_t *rdram = nullptr;     // Byte-swapped in 32-bit words, as the recomp keeps it.
        uint64_t rdramBytes = 0;
        TextureNameLoad load;
        TextureNameDrawTile drawTile;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t tlut = 0;
        // Only filled in for textures that may use a palette (a TLUT is on, or the CI format).
        const TextureNamePaletteLoad *paletteLoads = nullptr;
        size_t paletteLoadCount = 0;
    };

    // A provider's names for one texture, kept with the texture so packs enabled later can match it too.
    struct TextureNameKey {
        bool valid = false;
        alignas(8) uint8_t bytes[64] = {};
    };

    // One pack's files, as seen by a provider.
    class TextureNamePackIndex {
    public:
        virtual ~TextureNamePackIndex() = default;

        // The pack file for a texture, or nullptr. 'textureHash' and 'packIndex' are for the provider's logs.
        virtual const std::string *find(const TextureNameKey &key, uint64_t textureHash, int packIndex) const = 0;

        // Whether this file's alpha is kept apart from its colour. If so, alphaPath is the pack file holding the alpha,
        // or nullptr when the pack has none.
        virtual bool separateAlpha(const std::string &relativePath, const std::string *&alphaPath) const = 0;
    };

    class TextureNameProvider {
    public:
        virtual ~TextureNameProvider() = default;

        // Fills in the key of a texture seen for the first time. Must not read outside input.rdramBytes.
        virtual void computeKey(const TextureNameInput &input, TextureNameKey &key) const = 0;

        // Indexes a pack's files (relative paths with forward slashes). nullptr when none of them concern this provider.
        // Called once per pack, packIndex counting up from 0, whenever the packs are loaded.
        virtual std::unique_ptr<TextureNamePackIndex> createPackIndex(int packIndex, const std::vector<std::string> &relativePaths) const = 0;

        // Applies a separate alpha file to decoded RGBA8 pixels. alphaRgba (decoded RGBA8, same size) is nullptr when
        // the pack has no alpha file for this one.
        virtual void applySeparateAlpha(uint8_t *rgba, const uint8_t *alphaRgba, size_t pixelCount) const = 0;
    };

    // Set once, before the renderer starts. The provider must outlive the renderer. nullptr means no provider.
    void setTextureNameProvider(const TextureNameProvider *provider);
    const TextureNameProvider *textureNameProvider();
};
