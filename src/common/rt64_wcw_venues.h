//
// RT64
//

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// [wcw rt] Venue light presets (assets/rt_lights.json): the file's data model, parsing and writing, and venue matching.
// Kept free of renderer state so it can be unit tested (tests/wcw/rt64_wcw_venues_tests.cpp); the ray tracer and the
// Lighting editor call these.
namespace RT64::WcwLogic {
    // The fields a preset sets itself (LightPreset::setFields); the others are inherited from "default".
    enum PresetField : uint32_t {
        FieldCenter = 1u << 0,
        FieldSpread = 1u << 1,
        FieldFills = 1u << 2,
        FieldKeyWeight = 1u << 3,
        FieldLights = 1u << 4,
        FieldSize = 1u << 5,
        FieldRadius = 1u << 6,
        FieldClearance = 1u << 7,
        FieldFalloff = 1u << 8,
        FieldGlows = 1u << 9,
        FieldAnchor = 1u << 10,
        FieldShadow = 1u << 11,
        FieldCharacterShadow = 1u << 12
    };

    // One light preset: "default", "room" or a venue.
    struct LightPreset {
        std::string name;
        // World texture hashes that recognize the venue.
        std::vector<uint64_t> match;
        // The match textures no other venue lists (scored instead of match when not empty; derived, never written).
        std::vector<uint64_t> uniqueMatch;
        // Lights' center (receiver center on X/Z, key light height on Y).
        float center[3] = { 0.0f, 460.0f, 0.0f };
        // Generated rig: a key light at the center plus "fills" lights at center +- spread on X/Z; or an explicit
        // "lights" list (xyz + relative weight) replacing it.
        float spread = 200.0f;
        uint32_t fills = 4;
        float keyWeight = 0.5f;
        std::vector<std::array<float, 4>> lights;
        float size = 20.0f;
        float radius = 750.0f;
        float clearance = 0.0f;
        float falloff = 0.0f;
        // "glows": [[x, y, z, intensity, range], ...] (offsets from the anchor like lights).
        std::vector<std::array<float, 5>> glows;
        // "anchor": "scene": center and explicit lights are offsets from the drawn world geometry's center on X/Z
        // and its floor (lowest point) on Y, so one preset fits rooms placed anywhere in the world.
        bool anchorScene = false;
        float shadow = 0.6f;
        float characterShadow = 0.35f;
        // PresetField bits of the fields this preset sets itself (read from the file, or edited).
        uint32_t setFields = 0;
    };

    // The whole rt_lights.json.
    struct LightPresetFile {
        // "debugMode" (WCW_RT_MODE override) when present.
        bool hasDebugMode = false;
        uint32_t debugMode = 0;
        LightPreset defaultPreset;
        // "room": fallback for unmatched small scenes (extent on X/Z at most maxExtent): count ceiling lights along
        // the long axis at height (fraction of floor to ceiling). Its other fields default to "default"'s.
        bool roomEnabled = false;
        float roomMaxExtent = 3000.0f;
        uint32_t roomCount = 3;
        float roomHeight = 0.85f;
        LightPreset roomPreset;
        // Venues inherit every field they don't set from "default".
        std::vector<LightPreset> venues;
    };

    // Parses rt_lights.json text (comments allowed). Fills each venue's uniqueMatch. Throws std::exception on
    // malformed JSON.
    LightPresetFile parseLightPresets(const std::string &text);

    // Writes the file back as JSON that parses to the same presets. Only the fields a preset sets itself are written
    // (setFields), so later edits to "default" still reach the venues that never overrode them. Comments in the
    // original are not kept.
    std::string writeLightPresets(const LightPresetFile &file);

    // Copies "default"'s values into the fields the room and the venues don't set themselves (after editing
    // "default", so the presets in use match what the file will say).
    void inheritDefault(LightPresetFile &file);

    // Venue texture hashes are written as 16 uppercase hex digits.
    std::string formatTextureHash(uint64_t hash);

    // Sets each venue's uniqueMatch: its match textures no other venue lists.
    void computeUniqueMatches(std::vector<LightPreset> &venues);

    // The fraction (0..1) of a venue's match textures (its unique ones when it has any) for which seen() is true.
    float venueMatchScore(const LightPreset &venue, const std::function<bool(uint64_t)> &seen);

    // The best scoring venue at or above minScore (-1: none). Ties go to the later venue (the long-standing rule).
    int bestVenue(const std::vector<LightPreset> &venues, const std::function<bool(uint64_t)> &seen, float minScore = 0.5f);

    // Match textures for a new venue made from the current scene: the stable world textures (hash, max triangles)
    // that no existing venue lists, largest first, at most maxCount.
    std::vector<uint64_t> newVenueMatch(const std::vector<std::pair<uint64_t, uint32_t>> &stableTextures,
        const std::vector<LightPreset> &venues, size_t maxCount = 15);
};
