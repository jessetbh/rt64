//
// RT64
//

#include "rt64_rdp_tmem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cinttypes>
#include <cstring>

#include "xxHash/xxh3.h"

#include "common/rt64_tmem_hasher.h"
#include "common/rt64_wcw_logic.h"
#include "common/rt64_wcw_texdump.h"
#include "common/rt64_texture_names.h"

#include "rt64_state.h"

namespace RT64 {
    // [wcw] What rt64 knows about a new texture, for the application's TextureNameProvider if it installed one
    // (common/rt64_texture_names.h): names from other renderers' texture packs, aki-roadmap#93.
    static TextureNameKey computeTextureNames(const State *state, const LoadTile &drawTile, uint16_t width, uint16_t height, uint32_t tlut) {
        TextureNameKey key;
        const TextureNameProvider *provider = textureNameProvider();
        if (provider == nullptr) {
            return key;
        }

        const LoadOperation &loadOp = state->rdp->rice.lastLoadOpByTMEM[drawTile.tmem & (RDP_TMEM_WORDS - 1)];
        if (loadOp.texture.address == 0) {
            return key;
        }

        TextureNameInput input;
        input.rdram = reinterpret_cast<const uint8_t *>(state->RDRAM);
        input.rdramBytes = uint64_t(RDRAMSize) + 1;
        if (loadOp.type == LoadOperation::Type::Tile) {
            input.load.type = TextureNameLoad::Type::Tile;
        }
        else if (loadOp.type == LoadOperation::Type::Block) {
            input.load.type = TextureNameLoad::Type::Block;
        }

        input.load.siz = loadOp.tile.siz;
        input.load.masks = loadOp.tile.masks;
        input.load.maskt = loadOp.tile.maskt;
        input.load.uls = loadOp.tile.uls;
        input.load.ult = loadOp.tile.ult;
        input.load.lrs = loadOp.tile.lrs;
        input.load.lrt = loadOp.tile.lrt;
        input.load.address = loadOp.texture.address;
        input.load.textureSiz = loadOp.texture.siz;
        input.load.textureWidth = loadOp.texture.width;
        input.drawTile.fmt = drawTile.fmt;
        input.drawTile.siz = drawTile.siz;
        input.drawTile.line = drawTile.line;
        input.drawTile.palette = drawTile.palette;
        input.drawTile.cms = drawTile.cms;
        input.drawTile.cmt = drawTile.cmt;
        input.drawTile.masks = drawTile.masks;
        input.drawTile.maskt = drawTile.maskt;
        input.drawTile.uls = drawTile.uls;
        input.drawTile.ult = drawTile.ult;
        input.drawTile.lrs = drawTile.lrs;
        input.drawTile.lrt = drawTile.lrt;
        input.width = width;
        input.height = height;
        input.tlut = tlut;

        // The TLUT loads recorded at the palette half of TMEM (entry = TMEM word - 256).
        std::vector<TextureNamePaletteLoad> paletteLoads;
        if ((tlut != 0) || (drawTile.fmt == G_IM_FMT_CI)) {
            const uint32_t paletteBase = RDP_TMEM_WORDS >> 1;
            for (uint32_t t = paletteBase; t < RDP_TMEM_WORDS; t++) {
                const LoadOperation &op = state->rdp->rice.lastLoadOpByTMEM[t];
                if ((op.type != LoadOperation::Type::TLUT) || (op.texture.address == 0)) {
                    continue;
                }

                const int columns = std::max((op.tile.lrs >> 2) - (op.tile.uls >> 2), 0) + 1;
                const int rows = std::max((op.tile.lrt >> 2) - (op.tile.ult >> 2), 0) + 1;
                TextureNamePaletteLoad paletteLoad;
                paletteLoad.firstEntry = t - paletteBase;
                paletteLoad.count = std::min(uint32_t(columns * rows), 256U - paletteLoad.firstEntry);
                paletteLoad.address = op.texture.address;
                paletteLoad.sequence = state->rdp->rice.lastLoadSeqByTMEM[t];
                paletteLoads.emplace_back(paletteLoad);
            }

            input.paletteLoads = paletteLoads.data();
            input.paletteLoadCount = paletteLoads.size();
        }

        provider->computeKey(input, key);
        return key;
    }

    // TextureManager

    void TextureManager::uploadEmpty(State *state, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint64_t replacementHash) {
        if (hashSet.find(replacementHash) == hashSet.end()) {
            hashSet.insert(replacementHash);
            textureCache->queueGPUUploadTMEM(replacementHash, creationFrame, nullptr, 0, width, height, 0, LoadTile(), false);
        }
    }

    uint64_t TextureManager::uploadTMEM(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t byteOffset, uint16_t byteCount, uint16_t width, uint16_t height, uint32_t tlut) {
        XXH3_state_t xxh3;
        XXH3_64bits_reset(&xxh3);
        const uint8_t *TMEM = reinterpret_cast<const uint8_t *>(state->rdp->TMEM);
        XXH3_64bits_update(&xxh3, &TMEM[byteOffset], byteCount);
        XXH3_64bits_update(&xxh3, &byteOffset, sizeof(byteOffset));
        XXH3_64bits_update(&xxh3, &byteCount, sizeof(byteCount));
        const uint64_t hash = XXH3_64bits_digest(&xxh3);
        if (hashSet.find(hash) == hashSet.end()) {
            hashSet.insert(hash);
            textureCache->queueGPUUploadTMEM(hash, creationFrame, TMEM, RDP_TMEM_BYTES, width, height, 0, LoadTile(), false);
        }
        
        // Dump memory contents into a file if the process is active.
        if (!state->dumpingTexturesDirectory.empty()) {
            // Since width and height are not exactly guaranteed to be sane values when using raw TMEM, ensure we only dump textures when the values make sense.
            const bool validTextureCheck = (width > 0x0) && (height > 0x0);
            const bool bigTextureCheck = (width > 0x1000) || (height > 0x1000);
            if (validTextureCheck && !bigTextureCheck) {
                dumpTexture(hash, state, loadTile, width, height, tlut);
            }
        }

        return hash;
    }

    // [wcw] aki-roadmap#105: a CI4 mat's TMEM with its painted rope-shadow lines filled in. Each line texel takes the
    // palette entry nearest the colour interpolated from the texels on both sides of its line, so the GPU decodes the
    // copy like any other texture. Returns empty when the texture has no lines.
    // The listed mat hashes (WcwLogic::delineHashes) are TMEM hash v5: a new hash version needs them measured again.
    static_assert(TMEMHasher::CurrentHashVersion == 5, "aki-roadmap#105: re-measure WcwLogic::delineHashes for the new hash version");

    static std::vector<uint8_t> wcwDelineTMEM(const uint8_t *TMEM, const LoadTile &loadTile, uint32_t width, uint32_t height, uint32_t tlut, uint32_t &changedTexels) {
        changedTexels = 0;
        WcwTexDump::TileFormat tileFormat;
        tileFormat.fmt = loadTile.fmt;
        tileFormat.siz = loadTile.siz;
        tileFormat.address = uint32_t(loadTile.tmem) << 3;
        tileFormat.stride = uint32_t(loadTile.line) << 3;
        tileFormat.tlut = tlut;
        tileFormat.palette = loadTile.palette;
        const std::vector<uint8_t> rgba = WcwTexDump::decodeTexture(TMEM, width, height, tileFormat);
        std::vector<float> luminance(size_t(width) * height);
        for (size_t i = 0; i < luminance.size(); i++) {
            luminance[i] = 0.299f * rgba[i * 4 + 0] + 0.587f * rgba[i * 4 + 1] + 0.114f * rgba[i * 4 + 2];
        }

        const std::vector<WcwLogic::DelineLine> lines = WcwLogic::delineFind(luminance, width, height);
        if (lines.empty()) {
            return {};
        }

        std::vector<uint8_t> filled = rgba;
        WcwLogic::delineFill(filled, width, height, lines);

        // The tile's 16 palette colours, decoded like WcwTexDump does.
        uint8_t palette[16 * 4];
        for (uint32_t i = 0; i < 16; i++) {
            const uint32_t address = 0x800 + (uint32_t(loadTile.palette) << 7) + (i << 3);
            const uint32_t value = (uint32_t(TMEM[address & 0xFFF]) << 8) | TMEM[(address + 1) & 0xFFF];
            uint8_t *c = &palette[i * 4];
            if (tlut == WcwTexDump::TlutIA16) {
                c[0] = c[1] = c[2] = uint8_t(value >> 8);
                c[3] = uint8_t(value & 0xFF);
            }
            else {
                const uint32_t r = (value >> 11) & 0x1F, g = (value >> 6) & 0x1F, b = (value >> 1) & 0x1F;
                c[0] = uint8_t((r << 3) | (r >> 2));
                c[1] = uint8_t((g << 3) | (g >> 2));
                c[2] = uint8_t((b << 3) | (b >> 2));
                c[3] = (value & 1) ? 255 : 0;
            }
        }

        std::vector<uint8_t> bytes(TMEM, TMEM + RDP_TMEM_BYTES);
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const size_t i = (size_t(y) * width + x) * 4;
                if (memcmp(&filled[i], &rgba[i], 4) == 0) {
                    continue;
                }

                const uint32_t index = WcwLogic::nearestPaletteIndex(&filled[i], palette, 16);
                const uint32_t byte = WcwLogic::tmem4bTexelByte(tileFormat.address, tileFormat.stride, x, y);
                const uint8_t updated = (x & 1) ? uint8_t((bytes[byte] & 0xF0) | index) : uint8_t((bytes[byte] & 0x0F) | (index << 4));
                changedTexels += (updated != bytes[byte]) ? 1 : 0;
                bytes[byte] = updated;
            }
        }

        return bytes;
    }

    uint64_t TextureManager::uploadTexture(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint32_t tlut) {
        const uint8_t *TMEM = reinterpret_cast<const uint8_t *>(state->rdp->TMEM);
        // [wcw] aki-roadmap#119 (experiment): WCW_TEXHASH_LOG=1 logs, every 2 s, how many texture hashes ran, their time,
        // how many were new, and the slowest call's tile.
        static const bool wcwHashLog = getenv("WCW_TEXHASH_LOG") != nullptr;
        std::chrono::steady_clock::time_point wcwT0;
        if (wcwHashLog) wcwT0 = std::chrono::steady_clock::now();
        uint64_t hash = TMEMHasher::hash(TMEM, loadTile, width, height, tlut, TMEMHasher::CurrentHashVersion);
        if (wcwHashLog) {
            static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
            static uint64_t calls = 0, fresh = 0, rowCalls = 0;
            static double totalUs = 0.0, maxUs = 0.0;
            static uint32_t maxW = 0, maxH = 0, maxLine = 0, maxSiz = 0, maxFmt = 0, maxTlut = 0, maxTmem = 0, maxRows = 0;
            const auto now = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(now - wcwT0).count();
            const bool rows = TMEMHasher::needsToHashRowsIndividually(loadTile, width);
            calls++;
            rowCalls += rows ? 1 : 0;
            fresh += (hashSet.find(hash) == hashSet.end()) ? 1 : 0;
            totalUs += us;
            if (us > maxUs) {
                maxUs = us; maxW = width; maxH = height; maxLine = loadTile.line; maxSiz = loadTile.siz; maxFmt = loadTile.fmt;
                maxTlut = tlut; maxTmem = loadTile.tmem; maxRows = rows;
            }
            if (std::chrono::duration<double>(now - last).count() >= 2.0) {
                fprintf(stderr, "[wcw][texhash] %llu calls (%llu row-by-row, %llu new) %.1f ms; slowest %.0f us: %ux%u line %u siz %u fmt %u tlut %u tmem %u rows %u\n",
                    (unsigned long long)calls, (unsigned long long)rowCalls, (unsigned long long)fresh, totalUs / 1000.0, maxUs,
                    maxW, maxH, maxLine, maxSiz, maxFmt, maxTlut, maxTmem, maxRows);
                last = now; calls = fresh = rowCalls = 0; totalUs = maxUs = 0.0;
            }
        }
        if (hashSet.find(hash) == hashSet.end()) {
            hashSet.insert(hash);
            const TextureNameKey nameKey = computeTextureNames(state, loadTile, width, height, tlut);
            textureCache->queueGPUUploadTMEM(hash, creationFrame, TMEM, RDP_TMEM_BYTES, width, height, tlut, loadTile, true, nameKey);
        }

        // [wcw] aki-roadmap#105: the listed mats also get a copy with their painted rope shadows filled in, which the
        // renderer uses with RT on. It has its own cache key, so it is evicted and uploaded again like any texture.
        if ((loadTile.fmt == WcwTexDump::FmtCI) && (loadTile.siz == WcwTexDump::Siz4b) && (tlut != 0) && TextureCache::wcwDelineListed(hash)) {
            const uint64_t delinedHash = WcwLogic::delinedHash(hash);
            if (hashSet.find(delinedHash) == hashSet.end()) {
                hashSet.insert(delinedHash);
                uint32_t changedTexels = 0;
                const std::vector<uint8_t> delined = wcwDelineTMEM(TMEM, loadTile, width, height, tlut, changedTexels);
                if (!delined.empty()) {
                    textureCache->queueGPUUploadTMEM(delinedHash, creationFrame, delined.data(), int(delined.size()), width, height, tlut, loadTile, true);
                }

                static bool logged = false;
                if (!logged) {
                    logged = true;
                    fprintf(stderr, "[wcw][deline] %016" PRIx64 ": %u texels filled in (RT on uses the copy)\n", hash, changedTexels);
                }
            }
        }

        // Dump memory contents into a file if the process is active.
        if (!state->dumpingTexturesDirectory.empty()) {
            dumpTexture(hash, state, loadTile, width, height, tlut);
        }

        return hash;
    }

    void TextureManager::dumpTexture(uint64_t hash, State *state, const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlut) {
        // [wcw] Once per session, and never over a file the pack folder already had (aki-roadmap#98).
        const WcwLogic::TexDumpWrite write = WcwLogic::texDumpWrite(dumpedSet.find(hash) != dumpedSet.end(), packSet.find(hash) != packSet.end());
        if (write == WcwLogic::TexDumpWrite::Nothing) {
            return;
        }

        // Insert into set regardless of whether the dump is successful or not.
        dumpedSet.insert(hash);

        // [wcw] The texture as the game draws it, as <hash>.png in the dump's pack folder: edit it and load the folder
        // as a texture pack (aki-roadmap#48). Raw TMEM textures (uploadTMEM) decode with the tile they were loaded with.
        // Written to a temp file and renamed, so a quit mid-write never leaves a broken PNG that later dumps would keep.
        if (write == WcwLogic::TexDumpWrite::All) {
            WcwTexDump::TileFormat tileFormat;
            tileFormat.fmt = loadTile.fmt;
            tileFormat.siz = loadTile.siz;
            tileFormat.address = uint32_t(loadTile.tmem) << 3;
            tileFormat.stride = uint32_t(loadTile.line) << 3;
            tileFormat.tlut = tlut;
            tileFormat.palette = loadTile.palette;
            const std::vector<uint8_t> rgba = WcwTexDump::decodeTexture(reinterpret_cast<const uint8_t *>(state->rdp->TMEM), width, height, tileFormat);
            const std::vector<uint8_t> png = WcwTexDump::encodePNG(rgba.data(), width, height);
            if (!png.empty()) {
                const std::filesystem::path pngPath = state->dumpingTexturesDirectory / WcwTexDump::PackFolderName / WcwTexDump::hashFileName(hash);
                std::filesystem::path tempPath = pngPath;
                tempPath += ".tmp";
                std::ofstream pngStream(tempPath, std::ios::binary);
                if (pngStream.is_open()) {
                    pngStream.write(reinterpret_cast<const char *>(png.data()), png.size());
                    pngStream.close();
                    std::error_code ec;
                    if (!pngStream.fail()) {
                        std::filesystem::rename(tempPath, pngPath, ec);
                    }

                    if (pngStream.fail() || ec) {
                        std::filesystem::remove(tempPath, ec);
                    }
                }
            }
        }
        
        // Dump the entirety of TMEM.
        char baseName[64];
        snprintf(baseName, sizeof(baseName), "%016" PRIx64 ".v%u", hash, TMEMHasher::CurrentHashVersion);
        std::filesystem::path dumpTmemPath = state->dumpingTexturesDirectory / (std::string(baseName) + ".tmem");
        std::ofstream dumpTmemStream(dumpTmemPath, std::ios::binary);
        if (dumpTmemStream.is_open()) {
            const char *TMEM = reinterpret_cast<const char *>(state->rdp->TMEM);
            dumpTmemStream.write(TMEM, RDP_TMEM_BYTES);
            dumpTmemStream.close();
        }

        // Dump the RDRAM last loaded into the TMEM address pointed to by the tile. Required for generating hashes used by Rice.
        const LoadOperation &loadOp = state->rdp->rice.lastLoadOpByTMEM[loadTile.tmem];
        uint32_t rdramStart = loadOp.texture.address;
        uint32_t rdramCount = 0;
        uint32_t commonBytesOffset = (loadOp.tile.uls >> 2) << loadOp.texture.siz >> 1;
        uint32_t commonBytesPerRow = loadOp.texture.width << loadOp.texture.siz >> 1;
        if (loadOp.type == LoadOperation::Type::Block) {
            uint32_t wordCount = ((loadOp.tile.lrs - loadOp.tile.uls) >> (4 - loadOp.tile.siz)) + 1;
            rdramStart = loadOp.texture.address + commonBytesOffset + commonBytesPerRow * loadOp.tile.ult;
            rdramCount = (wordCount << 3);

            // Increase the amount of RDRAM dumped by textures that require padding when using load block.
            commonBytesPerRow = std::max(commonBytesPerRow, uint32_t(loadTile.line) << 3U);
        }
        else if (loadOp.type == LoadOperation::Type::Tile) {
            uint32_t rowCount = 1 + ((loadOp.tile.lrt >> 2) - (loadOp.tile.ult >> 2));
            uint32_t tileWidth = ((loadOp.tile.lrs >> 2) - (loadOp.tile.uls >> 2));
            uint32_t wordsPerRow = (tileWidth >> (4 - loadOp.tile.siz)) + 1;
            rdramStart = loadOp.texture.address + commonBytesOffset + commonBytesPerRow * (loadOp.tile.ult >> 2);
            rdramCount = rowCount * commonBytesPerRow;
        }
        
        // Dump more RDRAM if necessary if it doesn't cover what the tile could possibly sample.
        uint32_t loadTileBpr = width << loadTile.siz >> 1;
        rdramCount = std::max(rdramCount, std::max(loadTileBpr, commonBytesPerRow) * height);

        if (rdramCount > 0) {
            std::filesystem::path dumpRdramPath = state->dumpingTexturesDirectory / (std::string(baseName) + ".rice.rdram");
            std::ofstream dumpRdramStream(dumpRdramPath, std::ios::binary);
            if (dumpRdramStream.is_open()) {
                const char *RDRAM = reinterpret_cast<const char *>(state->RDRAM);
                dumpRdramStream.write(&RDRAM[rdramStart], rdramCount);
                dumpRdramStream.close();
            }

            std::filesystem::path dumpRdramInfoPath = state->dumpingTexturesDirectory / (std::string(baseName) + ".rice.json");
            std::ofstream dumpRdramInfoStream(dumpRdramInfoPath);
            if (dumpRdramInfoStream.is_open()) {
                json jroot;
                jroot["tile"] = loadOp.tile;
                jroot["type"] = loadOp.type;
                jroot["texture"] = loadOp.texture;
                dumpRdramInfoStream << std::setw(4) << jroot << std::endl;
                dumpRdramInfoStream.close();
            }
        }
        
        // Repeat a similar process for dumping the palette.
        if (tlut > 0) {
            const bool CI4 = (loadTile.siz == G_IM_SIZ_4b);
            const int32_t paletteTMEM = (RDP_TMEM_WORDS >> 1) + (CI4 ? (loadTile.palette << 4) : 0);
            const LoadOperation &paletteLoadOp = state->rdp->rice.lastLoadOpByTMEM[paletteTMEM];
            uint32_t paletteBytesOffset = (paletteLoadOp.tile.uls >> 2) << paletteLoadOp.texture.siz >> 1;
            uint32_t paletteBytesPerRow = paletteLoadOp.texture.width << paletteLoadOp.texture.siz >> 1;
            const uint32_t rowCount = 1 + ((paletteLoadOp.tile.lrt >> 2) - (paletteLoadOp.tile.ult >> 2));
            const uint32_t wordsPerRow = ((paletteLoadOp.tile.lrs >> 2) - (paletteLoadOp.tile.uls >> 2)) + 1;
            uint32_t paletteRdramStart = paletteLoadOp.texture.address + paletteBytesOffset + paletteBytesPerRow * (paletteLoadOp.tile.ult >> 2);
            uint32_t paletteRdramCount = (rowCount - 1) * paletteBytesPerRow + (wordsPerRow << 3);
            if (paletteRdramCount > 0) {
                std::filesystem::path dumpPaletteRdramPath = state->dumpingTexturesDirectory / (std::string(baseName) + ".rice.palette.rdram");
                std::ofstream dumpPaletteRdramStream(dumpPaletteRdramPath, std::ios::binary);
                if (dumpPaletteRdramStream.is_open()) {
                    const char *RDRAM = reinterpret_cast<const char *>(state->RDRAM);
                    dumpPaletteRdramStream.write(&RDRAM[paletteRdramStart], paletteRdramCount);
                    dumpPaletteRdramStream.close();
                }
            }

            std::filesystem::path dumpPaletteRdramInfoPath = state->dumpingTexturesDirectory / (std::string(baseName) + ".rice.palette.json");
            std::ofstream dumpPaletteRdramInfoStream(dumpPaletteRdramInfoPath);
            if (dumpPaletteRdramInfoStream.is_open()) {
                json jroot;
                jroot["tile"] = paletteLoadOp.tile;
                jroot["type"] = paletteLoadOp.type;
                jroot["texture"] = paletteLoadOp.texture;
                dumpPaletteRdramInfoStream << std::setw(4) << jroot << std::endl;
                dumpPaletteRdramInfoStream.close();
            }
        }

        // Dump the parameters of the tile into a JSON file.
        std::filesystem::path dumpTilePath = state->dumpingTexturesDirectory / (std::string(baseName) + ".tile.json");
        std::ofstream dumpTileStream(dumpTilePath);
        if (dumpTileStream.is_open()) {
            json jroot;
            jroot["tile"] = loadTile;
            jroot["width"] = width;
            jroot["height"] = height;

            // Serialize the TLUT into an enum instead.
            if (tlut == G_TT_RGBA16) {
                jroot["tlut"] = LoadTLUT::RGBA16;
            }
            else if (tlut == G_TT_IA16) {
                jroot["tlut"] = LoadTLUT::IA16;
            }
            else {
                jroot["tlut"] = LoadTLUT::None;
            }

            dumpTileStream << std::setw(4) << jroot << std::endl;
            dumpTileStream.close();
        }
    }

    bool TextureManager::prepareDumpDirectory(const std::filesystem::path &directory) {
        std::error_code ec;
        const std::filesystem::path packDirectory = directory / WcwTexDump::PackFolderName;
        std::filesystem::create_directories(packDirectory, ec);
        if (ec) {
            fprintf(stderr, "[wcw texdump] can't create %s: %s\n", packDirectory.u8string().c_str(), ec.message().c_str());
            return false;
        }

        // Keep an rt64.json the player may have edited.
        const std::filesystem::path databasePath = packDirectory / "rt64.json";
        if (!std::filesystem::exists(databasePath, ec)) {
            std::ofstream databaseStream(databasePath, std::ios::binary);
            if (databaseStream.is_open()) {
                const std::string text = WcwTexDump::packDatabaseJson(TMEMHasher::CurrentHashVersion);
                databaseStream.write(text.data(), text.size());
            }
        }

        // Keep every texture the pack folder already has: the player may have edited it (aki-roadmap#98).
        dumpedSet.clear();
        packSet.clear();
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(packDirectory, ec)) {
            uint64_t hash = 0;
            if (entry.is_regular_file(ec) && WcwTexDump::hashFromFileName(entry.path().filename().u8string(), hash)) {
                packSet.insert(hash);
            }
        }

        fprintf(stderr, "[wcw texdump] dumping textures to %s (keeping %zu already in pack)\n", directory.u8string().c_str(), packSet.size());
        return true;
    }

    void TextureManager::removeHashes(const std::vector<uint64_t> &hashes) {
        for (uint64_t hash : hashes) {
            hashSet.erase(hash);
        }
    }
};