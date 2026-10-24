//
// RT64
//

#pragma once

#include <filesystem>
#include <set>

#include "hle/rt64_draw_call.h"
#include "render/rt64_texture_cache.h"

namespace RT64 {
    struct State;

    struct TextureManager {
        std::set<uint64_t> hashSet;
        std::set<uint64_t> dumpedSet;

        // [wcw] Hashes with a hash-named file in the dump's pack folder when the dump started: their PNGs are never
        // rewritten (aki-roadmap#98).
        std::set<uint64_t> packSet;

        void uploadEmpty(State *state, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint64_t replacementHash);
        uint64_t uploadTMEM(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t byteOffset, uint16_t byteCount, uint16_t width, uint16_t height, uint32_t tlut);
        uint64_t uploadTexture(State *state, const LoadTile &loadTile, TextureCache *textureCache, uint64_t creationFrame, uint16_t width, uint16_t height, uint32_t tlut);
        void dumpTexture(uint64_t hash, State *state, const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlut);
        void removeHashes(const std::vector<uint64_t> &hashes);

        // [wcw] Start a dump: create the directory and its ready-to-zip pack folder (PNGs + rt64.json), aki-roadmap#48,
        // and note the textures the pack folder already has (aki-roadmap#98).
        bool prepareDumpDirectory(const std::filesystem::path &directory);
    };
};