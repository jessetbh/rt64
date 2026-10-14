//
// RT64
//

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
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
        FieldCharacterShadow = 1u << 12,
        FieldOutsideDarkness = 1u << 13,
        FieldOutsideEdge = 1u << 14,
        FieldColor = 1u << 15,
        FieldKeyColor = 1u << 16,
        FieldFillColor = 1u << 17,
        FieldLitAreas = 1u << 18
    };

    // Lit areas per preset (WCW_RT_MAX_LIT_AREAS in the shader).
    constexpr size_t MaxLitAreas = 2;

    using LightColor = std::array<float, 3>;
    constexpr LightColor WhiteLight = { 1.0f, 1.0f, 1.0f };

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
        // Colored light: "color" tints every light (white: no tint; the renderer normalizes light colors to unit
        // luminance, WcwLogic::normalizedLightColor). The generated rig's key and fills take "keyColor" / "fillColor"
        // (default: the preset's color). An explicit light [x, y, z, weight, r, g, b] carries its own color
        // (lightOwnColor), otherwise it follows color, also when the lights are inherited from "default".
        // keyColor and fillColor always hold the effective colors (resolved when parsed and by inheritDefault).
        LightColor color = WhiteLight;
        LightColor keyColor = WhiteLight;
        LightColor fillColor = WhiteLight;
        // Parallel to lights: each light's own color, used where lightOwnColor is 1 (syncColorLists keeps the sizes).
        std::vector<LightColor> lightColors;
        std::vector<uint8_t> lightOwnColor;
        float size = 20.0f;
        float radius = 750.0f;
        float clearance = 0.0f;
        float falloff = 0.0f;
        // "glows": [[x, y, z, intensity, range], ...] (offsets from the anchor like lights).
        std::vector<std::array<float, 5>> glows;
        // Parallel to glows: [x, y, z, intensity, range, r, g, b] gives a glow its color (white otherwise; not
        // normalized, so a color also scales the glow's strength).
        std::vector<LightColor> glowColors;
        // Parallel to glows: flicker amount 0..1 (a 9th value, [..., r, g, b, flicker]): 0 steady, 1 a failing fluorescent
        // tube that drops out completely in its bursts (glowFlicker), 0.5 dims to half.
        std::vector<float> glowFlicker;
        // "anchor": "scene": center and explicit lights are offsets from the drawn world geometry's center on X/Z
        // and its floor (lowest point) on Y, so one preset fits rooms placed anywhere in the world.
        bool anchorScene = false;
        float shadow = 0.6f;
        float characterShadow = 0.35f;
        // "outsideDarkness" (0..1): how much world geometry beyond the receiver radius (the arena, the crowd) and
        // the background are darkened, so the ring sits in a pool of light; ramps up over "outsideEdge" world units
        // past the radius. Wrestlers darken too (an exemption for them made them look spotlit in dim
        // rooms, so the owner had it removed, 2026-10-11). 0 = off (the arena stays as the game draws it).
        float outsideDarkness = 0.0f;
        float outsideEdge = 400.0f;
        // "litAreas": [[x, z, radius, edge], ...] (offsets from the anchor like lights, at most MaxLitAreas): places with
        // their own lighting, typically the entrance set, where outside darkness eases off like around the ring (none
        // within radius, full beyond radius + edge). Without them the set and its titantron (geometry) sank into the
        // house-lights-down darkness and glows had to fight it.
        std::vector<std::array<float, 4>> litAreas;
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

    // An explicit light's effective color: its own, or the preset's color. Out of range: the preset's color.
    LightColor presetLightColor(const LightPreset &preset, size_t lightIndex);

    // A glow's color (white when it has none).
    LightColor presetGlowColor(const LightPreset &preset, size_t glowIndex);

    // A glow's flicker amount (0 when it has none).
    float presetGlowFlicker(const LightPreset &preset, size_t glowIndex);

    // The graphics menu's Venue Lighting option: how strongly a venue's tuned look applies. The tuned rigs are bold
    // (outside darkness 0.55-0.65 in No Mercy's arenas), and some players find arenas too dark.
    enum class VenueLighting : uint32_t {
        // The preset as tuned.
        Full = 0,
        // Outside darkness and glows at SubtleVenueLighting of their strength.
        Subtle = 1,
        // Neutral lighting: no outside darkness, no glows, white light. The lights stay where the preset puts them
        // and keep their shadow strengths, so the ray-traced shadows and AO are unchanged.
        Off = 2
    };

    constexpr float SubtleVenueLighting = 0.5f;

    // The preset as it applies at a Venue Lighting level (out-of-range levels apply as Full).
    LightPreset applyVenueLighting(const LightPreset &preset, VenueLighting level);

    // A failing fluorescent tube's brightness (0 or 1) at timeSeconds, deterministic per seed: on almost all the time,
    // blinking off one to three times (70 ms each) every few seconds. Seeds give each tube its own rhythm.
    float glowFlicker(double timeSeconds, uint32_t seed);

    // A glow's intensity scale with flicker amount: 1 - amount * (1 - glowFlicker(time, seed)).
    float glowFlickerScale(double timeSeconds, uint32_t seed, float amount);

    // Pads or trims the color lists to the lights and glows (new entries: no own color, white glow), after an edit
    // adds or removes lights or glows.
    void syncColorLists(LightPreset &preset);

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
    // How much a world surface at receiverDistance (XZ distance from the lights' center) keeps of its brightness with
    // outside darkness: 1 inside the radius, easing (smoothstep) to 1 - darkness at radius + edge and beyond.
    // Wrestlers included. The trace shader (WcwRtTraceCS) mirrors this.
    float outsideDarknessFactor(float receiverDistance, float radius, float edge, float darkness);

    // How much of the outside darkness applies at a distance from a lit region: 0 within radius, easing (smoothstep)
    // to 1 at radius + edge and beyond.
    float darknessRamp(float distance, float radius, float edge);

    // outsideDarknessFactor at a point on the floor plane (x, z), with lit areas: the darkness ramp is the smallest of
    // the ring's (distance from center, radius, edge) and each lit area's, so the ring and the lit areas are all lit.
    float outsideDarknessAt(float x, float z, float centerX, float centerZ, float radius, float edge, float darkness,
        const std::vector<std::array<float, 4>> &litAreas);

    // Outside darkness for a ray miss: misses carry no position, so the ray is projected onto the arena floor plane
    // (y = floorY) and judged by the XZ distance from the center (centerX, centerZ) where it lands. A miss over the
    // ring mat (drawn by another pass, so not in this pass's scene) lands inside the radius and stays lit; a ray
    // that points up or lands far out (the stands, the crowd) counts as infinitely far. Before this, every miss
    // darkened and the mat went grey.
    float missFloorDistance(const float origin[3], const float direction[3], float floorY, float centerX, float centerZ);

    // Which traced pass of a frame darkens the ray misses (outside darkness), for one color target. AKI frames are
    // several passes into one image; misses must darken once, on the arena pass (the one that traces the most
    // meshes), after the crowd is drawn and before the wrestlers and HUD. Darkening on the first pass instead left
    // the crowd lit (it is drawn by a later pass). The choice sticks: it only moves to another pass that has had
    // clearly more meshes (SwitchRatio) for SwitchFrames frames in a row, or when the chosen pass disappears.
    // Following the frame's busiest pass directly flickered the background: two passes with 44 and 45 meshes
    // traded places from frame to frame.
    class MissDarkeningPass {
    public:
        static constexpr float SwitchRatio = 1.25f;
        static constexpr uint32_t SwitchFrames = 15;

        // Called once per traced pass; true for the pass that darkens the misses this frame.
        bool darkenThisPass(uint64_t presentId, uint32_t passIndex, size_t meshCount);

    private:
        void finishFrame();

        uint64_t present = UINT64_MAX;
        uint32_t chosenPass = UINT32_MAX;
        // This frame: the busiest pass and the chosen pass's mesh count (0 = it didn't run).
        uint32_t bestPass = UINT32_MAX;
        size_t bestMeshes = 0;
        size_t chosenMeshes = 0;
        uint32_t switchStreak = 0;
        uint32_t streakPass = UINT32_MAX;
    };

    // Which pass darkens the misses, per color target: once per target per present, on the first pass that traces at
    // least ArenaShare of the target's recent largest pass (the arena pass: 130-136 meshes in Raw, against 94 for the
    // pass before it and under 20 for the rest). Choosing by position flickered the crowd: at 60 fps the real and
    // interpolated frames go to alternating targets with their passes interleaved, and when a present carried both swap
    // targets' frames the arena pass moved from pass 3 to pass 8, so whole frames had no darkening pass.
    class MissDarkening {
    public:
        static constexpr float ArenaShare = 0.8f;
        // The recent maximum decays by this much per present, so a scene change to smaller passes takes over.
        static constexpr float MaxDecay = 0.98f;

        // Called once per traced pass, in order; true for the pass that darkens the misses on this target.
        bool darkenThisPass(const void *target, uint64_t presentId, size_t meshCount);

    private:
        struct Target {
            uint64_t present = UINT64_MAX;
            bool darkened = false;
            float recentMax = 0.0f;
        };

        std::unordered_map<const void *, Target> targets;
    };

    // Which venue's lighting applies, from the world textures each game frame draws (update() once per present, which
    // is once per game frame: interpolated frames don't come through here). Two ways to switch:
    // - Window: a venue's score counts its match textures drawn in the last WindowSeconds; the best venue (at least
    //   MinScore, -1 for none) must hold for HoldSeconds before it takes over. This rides out close-ups that show few
    //   of a venue's textures. On its own it made every camera cut late: the old venue's textures stay in the window
    //   with a full score, so the new venue took over only once they aged out, about 70 game frames (2.3 s at 30) after
    //   the cut. The No Mercy intro cuts faster than that, so most shots were lit with the previous shot's rig.
    // - Cut: when each of the last CutFrames frames that drew world textures shows another venue (score at least
    //   MinScore) and together they show at most CutActiveMax of the active venue's textures, the camera cut there:
    //   switch at once.
    // - Cut to an unmatched scene: when no venue matches the last CutFrames frames, they show none of the active
    //   venue's textures, and nearly all the textures they share (at least NewSceneShare, and NewSceneMin of them) were
    //   drawn nowhere else in the window, the camera cut to a new scene: back to no venue at once. A close-up or a
    //   camera swing inside a venue still shows textures from the frames before it (the ring, the floor), so it waits
    //   for the window as before. Without this the No Mercy intro lit its Raw ring shots (no rig recognizes them) with
    //   the Backlash entrance rig for 2.3 s.
    // The window and hold are in seconds (they were 60 and 10 presents), so they don't depend on the game's rate.
    class VenueSelector {
    public:
        static constexpr double WindowSeconds = 2.0;
        static constexpr double HoldSeconds = 1.0 / 3.0;
        static constexpr uint32_t CutFrames = 2;
        // Textured frames older than this don't count towards a cut (they are from before a 2D screen or a pause).
        static constexpr double RecentSeconds = 0.25;
        static constexpr float CutActiveMax = 0.2f;
        static constexpr float MinScore = 0.5f;
        static constexpr float NewSceneShare = 0.8f;
        static constexpr uint32_t NewSceneMin = 8;

        enum class Change { None, Window, Cut };

        // One game frame's world textures (any order, duplicates allowed) at a time in seconds. Returns how the active
        // venue changed, if it did.
        Change update(const std::vector<LightPreset> &venues, const std::vector<uint64_t> &textures, double seconds);
        void reset();
        int active() const { return activeVenue; }
        // The venue the last CutFrames frames point to on their own (-1: none), for logs.
        int recent() const { return recentVenue; }
        // For logs: the venue with the highest score on the last frames alone, at any score (-1: no venues).
        std::pair<int, float> recentLeader(const std::vector<LightPreset> &venues) const;
        // For logs: of the textures on each of the last frames, how many were drawn nowhere else in the window, of how many.
        std::pair<uint32_t, uint32_t> recentNew() const;
        // false: switch only through the window (the behavior before cuts were detected; WCW_RT_VENUE_CUT=0).
        bool cutsEnabled = true;

    private:
        void restartWindow(double seconds);

        struct Frame {
            double seconds;
            std::vector<uint64_t> textures;
        };

        std::deque<Frame> window;
        std::unordered_map<uint64_t, uint32_t> windowCounts;
        std::deque<Frame> recentFrames;
        int activeVenue = -1;
        int recentVenue = -1;
        int candidateVenue = -1;
        double candidateSince = 0.0;
    };

    std::vector<uint64_t> newVenueMatch(const std::vector<std::pair<uint64_t, uint32_t>> &stableTextures,
        const std::vector<LightPreset> &venues, size_t maxCount = 15);
};
