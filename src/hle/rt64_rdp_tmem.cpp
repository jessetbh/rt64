//
// RT64
//

#include "rt64_rdp_tmem.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>

#include "xxHash/xxh3.h"

#include "common/rt64_tmem_hasher.h"
#include "common/rt64_wcw_logic.h"
#include "common/rt64_wcw_texdump.h"
#include "common/rt64_wcw_jabo.h"
#include "common/rt64_wcw_rice.h"

#include "rt64_state.h"

namespace RT64 {
    // [wcw] The TLUT loads recorded at the palette half of TMEM (entry = TMEM word - 256), for the Rice and Jabo keys.
    static std::vector<WcwRice::PaletteLoad> recordedPaletteLoads(const State *state) {
        std::vector<WcwRice::PaletteLoad> paletteLoads;
        const uint32_t paletteBase = RDP_TMEM_WORDS >> 1;
        for (uint32_t t = paletteBase; t < RDP_TMEM_WORDS; t++) {
            const LoadOperation &op = state->rdp->rice.lastLoadOpByTMEM[t];
            if ((op.type != LoadOperation::Type::TLUT) || (op.texture.address == 0)) {
                continue;
            }

            const int columns = std::max((op.tile.lrs >> 2) - (op.tile.uls >> 2), 0) + 1;
            const int rows = std::max((op.tile.lrt >> 2) - (op.tile.ult >> 2), 0) + 1;
            WcwRice::PaletteLoad paletteLoad;
            paletteLoad.firstEntry = t - paletteBase;
            paletteLoad.count = std::min(uint32_t(columns * rows), 256U - paletteLoad.firstEntry);
            paletteLoad.address = op.texture.address;
            paletteLoad.sequence = state->rdp->rice.lastLoadSeqByTMEM[t];
            paletteLoads.emplace_back(paletteLoad);
        }

        return paletteLoads;
    }

    // [wcw] The texture's Jabo Direct3D8 name (common/rt64_wcw_jabo.h): its texels at the drawn width and height,
    // read from the RDRAM last loaded into its TMEM address (rows as Gadge64's computeJaboKey derives them), with the
    // CI palette from the recorded TLUT loads. RDRAM is byte-swapped in 32-bit words, so byte a is RDRAM[a ^ 3].
    static void computeJaboKey(const State *state, const LoadTile &drawTile, uint16_t width, uint16_t height, WcwRice::TextureKey &key) {
        if ((width == 0) || (height == 0) || (width > 1024) || (height > 1024) || (drawTile.siz > G_IM_SIZ_16b)) {
            return;
        }

        const uint64_t RDRAMBytes = uint64_t(RDRAMSize) + 1;
        const uint8_t *RDRAM = reinterpret_cast<const uint8_t *>(state->RDRAM);
        const LoadOperation &loadOp = state->rdp->rice.lastLoadOpByTMEM[drawTile.tmem & (RDP_TMEM_WORDS - 1)];
        if (loadOp.texture.address == 0) {
            return;
        }

        const uint32_t loadBytesPerRow = uint32_t(loadOp.texture.width) << loadOp.texture.siz >> 1;
        const uint32_t startOffset = uint32_t(loadOp.tile.uls >> 2) << loadOp.texture.siz >> 1;
        uint32_t start = 0;
        uint32_t rowStride = 0;
        if (loadOp.type == LoadOperation::Type::Block) {
            start = loadOp.texture.address + startOffset + loadBytesPerRow * loadOp.tile.ult;
            rowStride = uint32_t(drawTile.line) << 3;
        }
        else if (loadOp.type == LoadOperation::Type::Tile) {
            start = loadOp.texture.address + startOffset + loadBytesPerRow * (loadOp.tile.ult >> 2);
            rowStride = loadBytesPerRow;
        }
        else {
            return;
        }

        const uint32_t rowBytes = std::max(uint32_t(width) << drawTile.siz >> 1, 1U);
        if ((uint64_t(start) + uint64_t(rowStride) * (height - 1) + rowBytes) > RDRAMBytes) {
            return;
        }

        std::vector<uint8_t> texels(size_t(rowBytes) * height);
        for (uint32_t y = 0; y < height; y++) {
            const uint32_t row = start + y * rowStride;
            for (uint32_t x = 0; x < rowBytes; x++) {
                texels[size_t(y) * rowBytes + x] = RDRAM[(row + x) ^ 3];
            }
        }

        uint8_t palette[512] = {};
        const uint8_t *tilePalette = nullptr;
        if (drawTile.fmt == G_IM_FMT_CI) {
            const std::vector<WcwRice::PaletteLoad> paletteLoads = recordedPaletteLoads(state);
            for (uint32_t e = 0; e < 256; e++) {
                uint32_t address = 0;
                if (WcwRice::paletteEntryAddress(paletteLoads, e, address) && ((uint64_t(address) + 2) <= RDRAMBytes)) {
                    palette[e * 2] = RDRAM[address ^ 3];
                    palette[e * 2 + 1] = RDRAM[(address + 1) ^ 3];
                }
            }

            tilePalette = (drawTile.siz == G_IM_SIZ_4b) ? &palette[uint32_t(drawTile.palette & 0xF) * 32] : palette;
        }

        WcwJabo::Digest digest;
        if (!WcwJabo::textureDigest(drawTile.fmt, drawTile.siz, texels.data(), width, height, rowBytes, tilePalette, digest)) {
            return;
        }

        memcpy(key.jabo, digest.bytes, sizeof(key.jabo));
        key.jaboValid = true;
        if (WcwRice::logEnabled()) {
            static uint32_t logged = 0;
            if (++logged <= 5000) {
                fprintf(stderr, "[wcw jabo] key %s (%ux%u, fmt %u, siz %u)\n", WcwJabo::toHex(digest).c_str(), width, height, drawTile.fmt, drawTile.siz);
            }
        }
    }

    // [wcw] The texture's Rice / GLideN64 key (aki-roadmap#93), from the RDRAM last loaded into its TMEM address, as
    // GLideN64's _loadHiresTexture computes it (see common/rt64_wcw_rice.h). Invalid when there's no such load or it
    // reaches outside RDRAM.
    static WcwRice::TextureKey computeRiceKey(const State *state, const LoadTile &drawTile, uint32_t tlut) {
        WcwRice::TextureKey result;
        const uint64_t RDRAMBytes = uint64_t(RDRAMSize) + 1;
        const uint8_t *RDRAM = reinterpret_cast<const uint8_t *>(state->RDRAM);
        const LoadOperation &loadOp = state->rdp->rice.lastLoadOpByTMEM[drawTile.tmem & (RDP_TMEM_WORDS - 1)];
        if (loadOp.texture.address == 0) {
            return result;
        }

        WcwRice::Load load;
        switch (loadOp.type) {
        case LoadOperation::Type::Tile:
            load.type = WcwRice::LoadType::Tile;
            break;
        case LoadOperation::Type::Block:
            load.type = WcwRice::LoadType::Block;
            break;
        default:
            return result;
        }

        load.siz = loadOp.tile.siz;
        load.masks = loadOp.tile.masks;
        load.maskt = loadOp.tile.maskt;
        load.uls = loadOp.tile.uls;
        load.ult = loadOp.tile.ult;
        load.lrs = loadOp.tile.lrs;
        load.lrt = loadOp.tile.lrt;
        load.address = loadOp.texture.address;
        load.textureSiz = loadOp.texture.siz;
        load.textureWidth = loadOp.texture.width;

        WcwRice::DrawTile draw;
        draw.fmt = drawTile.fmt;
        draw.siz = drawTile.siz;
        draw.line = drawTile.line;
        draw.palette = drawTile.palette;
        draw.cms = drawTile.cms;
        draw.cmt = drawTile.cmt;
        draw.masks = drawTile.masks;
        draw.maskt = drawTile.maskt;
        draw.uls = drawTile.uls;
        draw.ult = drawTile.ult;
        draw.lrs = drawTile.lrs;
        draw.lrt = drawTile.lrt;

        WcwRice::Geometry geometry;
        if (!WcwRice::computeGeometry(load, draw, geometry)) {
            return result;
        }

        // The CRC may read up to 4 bytes before the start (rows under 4 bytes, see riceCRC32).
        if ((geometry.rdramStart < 4) || ((uint64_t(geometry.rdramStart) + geometry.bytesRead + 4) > RDRAMBytes)) {
            return result;
        }

        const uint8_t *src = &RDRAM[geometry.rdramStart];
        result.crc = WcwRice::riceCRC32(src, geometry.width, geometry.height, drawTile.siz, geometry.bytesPerRow);
        result.fmt = drawTile.fmt;
        result.siz = drawTile.siz;
        result.hasPalette = WcwRice::usesPalette(drawTile.fmt, drawTile.siz, tlut != 0);
        if (result.hasPalette) {
            const bool ci4 = (drawTile.siz == 0);
            const uint32_t ciMax = ci4 ? WcwRice::maxCI4(src, geometry.width, geometry.height, geometry.bytesPerRow) : WcwRice::maxCI8(src, geometry.width, geometry.height, geometry.bytesPerRow);

            const std::vector<WcwRice::PaletteLoad> paletteLoads = recordedPaletteLoads(state);

            // GLideN64's 256-entry palette copy, with a zero guard in front, for the entries the CRC reads.
            const uint32_t firstEntry = ci4 ? (uint32_t(drawTile.palette & 0xF) << 4) : 0;
            const uint32_t entryCount = std::min(ciMax + 1, 256U - firstEntry);
            uint8_t palette[4 + 512] = {};
            const uint32_t fromEntry = (firstEntry >= 2) ? (firstEntry - 2) : 0;
            for (uint32_t e = fromEntry; e < (firstEntry + entryCount); e++) {
                uint32_t address = 0;
                if (WcwRice::paletteEntryAddress(paletteLoads, e, address) && ((uint64_t(address) + 2) <= RDRAMBytes)) {
                    palette[4 + e * 2] = RDRAM[address];
                    palette[4 + e * 2 + 1] = RDRAM[address + 1];
                }
            }

            result.paletteCrc = WcwRice::paletteCRC(palette, firstEntry, entryCount, ci4);
        }

        result.valid = true;
        if (WcwRice::logEnabled()) {
            static uint32_t logged = 0;
            if (++logged <= 5000) {
                fprintf(stderr, "[wcw rice] key %s (%dx%d, bpl %d, rdram %08x, load %s)\n", WcwRice::textureKeyToString(result).c_str(),
                    geometry.width, geometry.height, geometry.bytesPerRow, geometry.rdramStart, (load.type == WcwRice::LoadType::Tile) ? "tile" : "block");
            }
        }

        return result;
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

    uint64_t TextureManager::uploadTexture(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint32_t tlut) {
        const uint8_t *TMEM = reinterpret_cast<const uint8_t *>(state->rdp->TMEM);
        uint64_t hash = TMEMHasher::hash(TMEM, loadTile, width, height, tlut, TMEMHasher::CurrentHashVersion);
        if (hashSet.find(hash) == hashSet.end()) {
            hashSet.insert(hash);
            WcwRice::TextureKey legacyKey = computeRiceKey(state, loadTile, tlut);
            computeJaboKey(state, loadTile, width, height, legacyKey);
            textureCache->queueGPUUploadTMEM(hash, creationFrame, TMEM, RDP_TMEM_BYTES, width, height, tlut, loadTile, true, legacyKey);
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