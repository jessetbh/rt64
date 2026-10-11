//
// RT64
//

// [wcw rt] Unit tests for common/rt64_wcw_venues (venue light presets: parsing, writing, matching). Built and run with
// the other wcw tests on every rt64 build.

#include "common/rt64_wcw_venues.h"

#include <set>
#include <string>

#include "wcw_test.h"

using namespace RT64::WcwLogic;

// The shape of No Mercy's assets/rt_lights.json: comments, a default with explicit lights, a room fallback, venues
// that inherit from default and venues that override fields, glows and a scene anchor.
static const char *SampleFile = R"json(
{
    // Default: TV lighting.
    "default": {
        "center": [0, 650, 0],
        "lights": [[0, 650, 0, 2], [-320, 600, -320, 1], [320, 600, -320, 1]],
        "size": 20,
        "radius": 900,
        "shadow": 0.6,
        "characterShadow": 0.35
    },
    "room": { "maxExtent": 3000, "count": 3, "height": 0.85, "falloff": 300, "size": 40, "shadow": 0.8 },
    "venues": [
        { "name": "Raw", "match": ["B1E01BE92A6F5BE1", "5581B2F76A29CE38", "00000000000000AA"],
          "glows": [[0, 300, 2900, 0.8, 900]] },
        { "name": "No Mercy", "match": ["66578B3E4816988B", "00000000000000AA"] },
        { "name": "Backstage hallway", "match": ["EF135CC7C26673E5"], "anchor": "scene",
          "lights": [[0, 400, 0, 1], [0, 400, 600, 1]], "radius": 1500, "shadow": 0.75 }
    ]
}
)json";

// Same effective colors: the preset's, the rig's, each explicit light's (own or followed) and each glow's.
static bool sameColors(const LightPreset &a, const LightPreset &b) {
    if ((a.color != b.color) || (a.keyColor != b.keyColor) || (a.fillColor != b.fillColor) || (a.lightOwnColor != b.lightOwnColor)) {
        return false;
    }

    for (size_t i = 0; i < a.lights.size(); i++) {
        if (presetLightColor(a, i) != presetLightColor(b, i)) {
            return false;
        }
    }

    for (size_t i = 0; i < a.glows.size(); i++) {
        if (presetGlowColor(a, i) != presetGlowColor(b, i)) {
            return false;
        }
    }

    return true;
}

static bool samePreset(const LightPreset &a, const LightPreset &b) {
    return (a.name == b.name) && (a.match == b.match) && (a.center[0] == b.center[0]) && (a.center[1] == b.center[1]) &&
        (a.center[2] == b.center[2]) && (a.spread == b.spread) && (a.fills == b.fills) && (a.keyWeight == b.keyWeight) &&
        (a.lights == b.lights) && (a.size == b.size) && (a.radius == b.radius) && (a.clearance == b.clearance) &&
        (a.falloff == b.falloff) && (a.glows == b.glows) && (a.anchorScene == b.anchorScene) && (a.shadow == b.shadow) &&
        (a.characterShadow == b.characterShadow) && sameColors(a, b);
}

TEST_CASE("venues: parsing applies default to venues that don't override it") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    CHECK_EQ(file.venues.size(), size_t(3));
    CHECK((file.venues[1].name) == std::string("No Mercy"));
    CHECK_EQ(file.venues[1].lights.size(), size_t(3));
    CHECK_NEAR(file.venues[1].radius, 900.0f, 1e-6f);
    CHECK_NEAR(file.venues[2].radius, 1500.0f, 1e-6f);
    CHECK(file.venues[2].anchorScene);
    CHECK_EQ(file.venues[0].glows.size(), size_t(1));
    CHECK(file.roomEnabled);
    CHECK_EQ(file.roomCount, 3u);
    CHECK_NEAR(file.roomPreset.size, 40.0f, 1e-6f);
    CHECK_NEAR(file.roomPreset.radius, 900.0f, 1e-6f);
}

TEST_CASE("venues: write then parse gives back the same presets") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    CHECK(samePreset(again.defaultPreset, file.defaultPreset));
    CHECK(samePreset(again.roomPreset, file.roomPreset));
    CHECK_EQ(again.roomMaxExtent, file.roomMaxExtent);
    CHECK_EQ(again.roomHeight, file.roomHeight);
    CHECK_EQ(again.venues.size(), file.venues.size());
    for (size_t i = 0; i < file.venues.size(); i++) {
        CHECK(samePreset(again.venues[i], file.venues[i]));
    }
}

TEST_CASE("venues: writing leaves out inherited fields, so editing default still reaches those venues") {
    LightPresetFile file = parseLightPresets(SampleFile);
    const std::string text = writeLightPresets(file);
    // "No Mercy" sets nothing but its name and match list.
    const size_t at = text.find("\"No Mercy\"");
    CHECK(at != std::string::npos);
    const size_t end = text.find('}', at);
    CHECK(text.substr(at, end - at).find("radius") == std::string::npos);

    file.defaultPreset.radius = 1200.0f;
    const LightPresetFile edited = parseLightPresets(writeLightPresets(file));
    CHECK_NEAR(edited.venues[1].radius, 1200.0f, 1e-6f);
    CHECK_NEAR(edited.venues[2].radius, 1500.0f, 1e-6f);

    // In memory, inheritDefault brings the presets in use in line with the file.
    inheritDefault(file);
    CHECK_NEAR(file.venues[1].radius, 1200.0f, 1e-6f);
    CHECK_NEAR(file.venues[2].radius, 1500.0f, 1e-6f);
    CHECK_NEAR(file.roomPreset.radius, 1200.0f, 1e-6f);
}

TEST_CASE("venues: a field edited on a venue is written even when it equals default") {
    LightPresetFile file = parseLightPresets(SampleFile);
    file.venues[1].radius = 900.0f;
    file.venues[1].setFields |= FieldRadius;
    file.defaultPreset.radius = 1200.0f;
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    CHECK_NEAR(again.venues[1].radius, 900.0f, 1e-6f);
}

TEST_CASE("venues: an edited light position round-trips at file precision") {
    LightPresetFile file = parseLightPresets(SampleFile);
    file.venues[2].lights[1][2] = 612.25f;
    file.venues[2].shadow = 0.7f;
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    CHECK_NEAR(again.venues[2].lights[1][2], 612.25f, 1e-4f);
    CHECK_NEAR(again.venues[2].shadow, 0.7f, 1e-6f);
}

TEST_CASE("venues: texture hashes are written as 16 uppercase hex digits") {
    CHECK((formatTextureHash(0xAAull)) == std::string("00000000000000AA"));
    CHECK((formatTextureHash(0xB1E01BE92A6F5BE1ull)) == std::string("B1E01BE92A6F5BE1"));
}

TEST_CASE("venues: textures listed by two venues don't count towards either") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    CHECK_EQ(file.venues[0].uniqueMatch.size(), size_t(2));
    CHECK_EQ(file.venues[1].uniqueMatch.size(), size_t(1));
}

TEST_CASE("venues: the best venue needs half its unique textures, later venues win ties") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    const std::set<uint64_t> raw = { 0xB1E01BE92A6F5BE1ull, 0xAAull };
    auto seenRaw = [&](uint64_t h) { return raw.count(h) > 0; };
    CHECK_NEAR(venueMatchScore(file.venues[0], seenRaw), 0.5f, 1e-6f);
    CHECK_EQ(bestVenue(file.venues, seenRaw), 0);

    // Only the shared texture: no venue reaches half of its unique textures.
    auto seenShared = [](uint64_t h) { return h == 0xAAull; };
    CHECK_EQ(bestVenue(file.venues, seenShared), -1);

    // Raw and No Mercy both fully seen: the later one wins, as before.
    const std::set<uint64_t> both = { 0xB1E01BE92A6F5BE1ull, 0x5581B2F76A29CE38ull, 0x66578B3E4816988Bull };
    CHECK_EQ(bestVenue(file.venues, [&](uint64_t h) { return both.count(h) > 0; }), 1);
}

TEST_CASE("outside darkness: none inside the radius, eases over the edge, full beyond it") {
    CHECK_NEAR(outsideDarknessFactor(800.0f, 900.0f, 400.0f, 0.8f), 1.0f, 1e-6f);
    CHECK_NEAR(outsideDarknessFactor(900.0f, 900.0f, 400.0f, 0.8f), 1.0f, 1e-6f);
    CHECK_NEAR(outsideDarknessFactor(1100.0f, 900.0f, 400.0f, 0.8f), 0.6f, 1e-6f);
    CHECK_NEAR(outsideDarknessFactor(1300.0f, 900.0f, 400.0f, 0.8f), 0.2f, 1e-6f);
    CHECK_NEAR(outsideDarknessFactor(5000.0f, 900.0f, 400.0f, 0.8f), 0.2f, 1e-6f);
    // No edge: a hard cut at the radius.
    CHECK_NEAR(outsideDarknessFactor(901.0f, 900.0f, 0.0f, 0.5f), 0.5f, 1e-6f);
}

TEST_CASE("outside darkness: 0 is off") {
    CHECK_NEAR(outsideDarknessFactor(3000.0f, 900.0f, 400.0f, 0.0f), 1.0f, 1e-6f);
}

TEST_CASE("outside darkness: set per venue, inherited from default otherwise, and round-trips") {
    LightPresetFile file = parseLightPresets(SampleFile);
    CHECK_NEAR(file.venues[1].outsideDarkness, 0.0f, 1e-6f);
    file.defaultPreset.outsideDarkness = 0.7f;
    file.defaultPreset.setFields |= FieldOutsideDarkness;
    file.venues[0].outsideDarkness = 0.3f;
    file.venues[0].setFields |= FieldOutsideDarkness;
    inheritDefault(file);
    CHECK_NEAR(file.venues[1].outsideDarkness, 0.7f, 1e-6f);
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    CHECK_NEAR(again.venues[0].outsideDarkness, 0.3f, 1e-6f);
    CHECK_NEAR(again.venues[1].outsideDarkness, 0.7f, 1e-6f);
    CHECK_NEAR(again.venues[1].outsideEdge, 400.0f, 1e-6f);
}

TEST_CASE("outside darkness: a miss over the ring mat stays lit, a miss towards the stands darkens (mat went grey)") {
    // Camera 600 up, 1500 back from the ring center, floor at 0.
    const float eye[3] = { 0.0f, 600.0f, -1500.0f };
    // Looking down at the ring center: lands at the center.
    const float atRing[3] = { 0.0f, -0.371391f, 0.928477f };
    CHECK_NEAR(missFloorDistance(eye, atRing, 0.0f, 0.0f, 0.0f), 0.0f, 1.0f);
    CHECK_NEAR(outsideDarknessFactor(missFloorDistance(eye, atRing, 0.0f, 0.0f, 0.0f), 650.0f, 500.0f, 0.55f), 1.0f, 1e-6f);
    // Level or upward rays (the stands across the arena) never reach the floor: fully dark.
    const float level[3] = { 0.0f, 0.0f, 1.0f };
    const float up[3] = { 0.0f, 0.3f, 0.95f };
    CHECK_NEAR(outsideDarknessFactor(missFloorDistance(eye, level, 0.0f, 0.0f, 0.0f), 650.0f, 500.0f, 0.55f), 0.45f, 1e-6f);
    CHECK_NEAR(outsideDarknessFactor(missFloorDistance(eye, up, 0.0f, 0.0f, 0.0f), 650.0f, 500.0f, 0.55f), 0.45f, 1e-6f);
    // A shallow downward ray landing beyond the ring and the edge: fully dark.
    const float shallow[3] = { 0.0f, -0.1f, 0.995f };
    CHECK(missFloorDistance(eye, shallow, 0.0f, 0.0f, 0.0f) > 1150.0f);
}

TEST_CASE("outside darkness: misses darken on last frame's arena pass, not the first pass (crowd stayed lit)") {
    MissDarkeningPass passes;
    // Frame 1 (No Mercy shape): passes 0..4 with 4, 8, 9, 76, 3 meshes. No choice yet.
    const size_t meshes[] = { 4, 8, 9, 76, 3 };
    for (uint32_t i = 0; i < 5; i++) {
        CHECK(!passes.darkenThisPass(100, i, meshes[i]));
    }

    // Frame 2: only the arena pass (3) darkens.
    for (uint32_t i = 0; i < 5; i++) {
        CHECK_EQ(passes.darkenThisPass(101, i, meshes[i]), i == 3);
    }
}

TEST_CASE("outside darkness: two near-equal passes don't trade the darkening frame to frame (background flickered)") {
    MissDarkeningPass passes;
    uint32_t switches = 0;
    bool lastFirst = false;
    for (uint64_t frame = 0; frame < 120; frame++) {
        // Pass 1 and pass 3 alternate between 44 and 45 meshes.
        const size_t a = (frame % 2) ? 44 : 45;
        const size_t b = (frame % 2) ? 45 : 44;
        const bool first = passes.darkenThisPass(frame, 1, a);
        const bool second = passes.darkenThisPass(frame, 3, b);
        CHECK(!(first && second));
        if ((frame > 1) && (first != lastFirst)) {
            switches++;
        }

        lastFirst = first;
    }

    CHECK_EQ(switches, 0u);
}

TEST_CASE("outside darkness: the arena pass follows a real scene change after a short delay") {
    MissDarkeningPass passes;
    passes.darkenThisPass(1, 0, 50);
    passes.darkenThisPass(1, 1, 10);
    CHECK(passes.darkenThisPass(2, 0, 50));
    // The scene changes: pass 1 becomes the arena pass. The choice holds for SwitchFrames frames, then moves.
    uint64_t frame = 3;
    for (uint32_t i = 0; i < MissDarkeningPass::SwitchFrames; i++, frame++) {
        passes.darkenThisPass(frame, 0, 5);
        passes.darkenThisPass(frame, 1, 60);
    }

    CHECK(!passes.darkenThisPass(frame, 0, 5));
    CHECK(passes.darkenThisPass(frame, 1, 60));
}

TEST_CASE("outside darkness: when the chosen pass stops running, the busiest pass takes over next frame") {
    MissDarkeningPass passes;
    passes.darkenThisPass(1, 2, 80);
    CHECK(passes.darkenThisPass(2, 2, 80));
    passes.darkenThisPass(3, 0, 30);
    CHECK(passes.darkenThisPass(4, 0, 30));
}

TEST_CASE("venues: a new venue from the scene uses its largest textures no venue lists") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    const std::vector<std::pair<uint64_t, uint32_t>> stable = {
        { 0x1ull, 40 }, { 0xB1E01BE92A6F5BE1ull, 500 }, { 0x2ull, 300 }, { 0x3ull, 120 }, { 0xAAull, 900 }
    };

    const std::vector<uint64_t> match = newVenueMatch(stable, file.venues, 2);
    CHECK_EQ(match.size(), size_t(2));
    CHECK_EQ(match[0], 0x2ull);
    CHECK_EQ(match[1], 0x3ull);
}

TEST_CASE("outside darkness: a present carrying both swap targets' frames keeps each target's arena pass (background flickered)") {
    // Raw's pattern: each frame is 94, 17, 9, 130, 2 meshes, on targets A and B in turn. Usually one frame per
    // present, but sometimes a present carries two frames (the second target's passes are then its 6th..10th).
    MissDarkening darkening;
    const int targets[2] = { 0, 0 };
    const size_t frame[] = { 94, 17, 9, 130, 2 };
    uint32_t gameFrame = 0;
    uint32_t framesWithoutDarkening = 0;
    for (uint64_t present = 0; present < 200; present++) {
        const int frames = ((present % 7) == 3) ? 2 : 1;
        for (int f = 0; f < frames; f++, gameFrame++) {
            uint32_t darkened = 0;
            for (size_t meshes : frame) {
                darkened += darkening.darkenThisPass(&targets[gameFrame % 2], present, meshes) ? 1 : 0;
            }

            CHECK(darkened <= 1u);
            if ((gameFrame > 4) && (darkened == 0)) {
                framesWithoutDarkening++;
            }
        }
    }

    CHECK_EQ(framesWithoutDarkening, 0u);
}

// Colored light (rt-color): "color", "keyColor", "fillColor", [x, y, z, weight, r, g, b] lights and
// [x, y, z, intensity, range, r, g, b] glows.

static const char *ColorFile = R"json(
{
    "default": {
        "color": [1, 0.9, 0.75],
        "lights": [[0, 650, 0, 2], [-320, 600, -320, 1, 1, 0, 0]]
    },
    "venues": [
        { "name": "Blue", "match": ["0000000000000001"], "color": [0.6, 0.7, 1] },
        { "name": "Plain", "match": ["0000000000000002"] },
        { "name": "Rig", "match": ["0000000000000003"], "lights": [], "keyColor": [1, 0.2, 0.2] },
        { "name": "Glow", "match": ["0000000000000004"], "glows": [[0, 500, 2700, 1, 1400], [0, 300, 0, 0.5, 900, 0.2, 0.4, 1]] }
    ]
}
)json";

TEST_CASE("colored light: presets without colors stay white and write no colors") {
    const LightPresetFile file = parseLightPresets(SampleFile);
    CHECK(file.defaultPreset.color == WhiteLight);
    CHECK(file.venues[0].keyColor == WhiteLight);
    CHECK(presetLightColor(file.venues[2], 1) == WhiteLight);
    CHECK(presetGlowColor(file.venues[0], 0) == WhiteLight);
    const std::string text = writeLightPresets(file);
    CHECK(text.find("olor") == std::string::npos);
    // Explicit lights without a color are written as [x, y, z, weight], as before.
    CHECK(parseLightPresets(text).defaultPreset.lightOwnColor == std::vector<uint8_t>({ 0, 0, 0 }));
}

TEST_CASE("colored light: a venue's color reaches its key and fill lights unless it sets them") {
    const LightPresetFile file = parseLightPresets(ColorFile);
    const LightColor warm = { 1.0f, 0.9f, 0.75f };
    const LightColor blue = { 0.6f, 0.7f, 1.0f };
    CHECK(file.defaultPreset.keyColor == warm);
    CHECK(file.venues[0].color == blue);
    CHECK(file.venues[0].keyColor == blue);
    CHECK(file.venues[0].fillColor == blue);
    CHECK(file.venues[1].color == warm);
    CHECK(file.venues[1].keyColor == warm);
    // Sets only its key color: the fills keep the inherited (warm) color.
    CHECK(file.venues[2].keyColor == LightColor({ 1.0f, 0.2f, 0.2f }));
    CHECK(file.venues[2].fillColor == warm);
}

TEST_CASE("colored light: explicit lights use their own color or follow the preset's, also when inherited") {
    const LightPresetFile file = parseLightPresets(ColorFile);
    const LightColor red = { 1.0f, 0.0f, 0.0f };
    CHECK(presetLightColor(file.defaultPreset, 0) == LightColor({ 1.0f, 0.9f, 0.75f }));
    CHECK(presetLightColor(file.defaultPreset, 1) == red);
    // "Blue" inherits default's lights: the one without its own color takes Blue's color, the red one stays red.
    CHECK(presetLightColor(file.venues[0], 0) == LightColor({ 0.6f, 0.7f, 1.0f }));
    CHECK(presetLightColor(file.venues[0], 1) == red);
    // Out of range (generated rig, room lights): the preset's color.
    CHECK(presetLightColor(file.venues[0], 7) == file.venues[0].color);
}

TEST_CASE("colored light: glows are white unless they carry a color") {
    const LightPresetFile file = parseLightPresets(ColorFile);
    const LightPreset &g = file.venues[3];
    CHECK_EQ(g.glows.size(), size_t(2));
    CHECK(presetGlowColor(g, 0) == WhiteLight);
    CHECK(presetGlowColor(g, 1) == LightColor({ 0.2f, 0.4f, 1.0f }));
    CHECK(presetGlowColor(g, 5) == WhiteLight);
}

TEST_CASE("colored light: write then parse keeps every color, and only the colors a preset sets are written") {
    const LightPresetFile file = parseLightPresets(ColorFile);
    const std::string text = writeLightPresets(file);
    const LightPresetFile again = parseLightPresets(text);
    CHECK(samePreset(again.defaultPreset, file.defaultPreset));
    CHECK_EQ(again.venues.size(), file.venues.size());
    for (size_t i = 0; i < file.venues.size(); i++) {
        CHECK(samePreset(again.venues[i], file.venues[i]));
        CHECK_EQ(again.venues[i].setFields, file.venues[i].setFields);
    }

    // "Plain" inherits its colors, so its entry has none and a later edit to default's color still reaches it.
    const size_t plain = text.find("\"Plain\"");
    const size_t rig = text.find("\"Rig\"");
    CHECK(text.substr(plain, rig - plain).find("olor") == std::string::npos);
}

TEST_CASE("colored light: editing default's color reaches the venues that don't set one (inheritDefault)") {
    LightPresetFile file = parseLightPresets(ColorFile);
    file.defaultPreset.color = { 0.5f, 1.0f, 0.5f };
    inheritDefault(file);
    CHECK(file.defaultPreset.keyColor == LightColor({ 0.5f, 1.0f, 0.5f }));
    CHECK(file.venues[1].keyColor == LightColor({ 0.5f, 1.0f, 0.5f }));
    CHECK(presetLightColor(file.venues[1], 0) == LightColor({ 0.5f, 1.0f, 0.5f }));
    CHECK(file.venues[0].keyColor == LightColor({ 0.6f, 0.7f, 1.0f }));
}

TEST_CASE("colored light: adding lights and glows pads the color lists (no own color, white glow)") {
    LightPreset p;
    p.color = { 1.0f, 0.5f, 0.5f };
    p.lights.push_back({ 0.0f, 600.0f, 0.0f, 1.0f });
    p.glows.push_back({ 0.0f, 0.0f, 0.0f, 1.0f, 100.0f });
    syncColorLists(p);
    CHECK_EQ(p.lightOwnColor.size(), size_t(1));
    CHECK_EQ(p.lightOwnColor[0], uint8_t(0));
    CHECK(presetLightColor(p, 0) == p.color);
    CHECK(presetGlowColor(p, 0) == WhiteLight);
}

// Lit areas: the entrance set keeps its own lighting under outside darkness.

TEST_CASE("lit areas: no darkness inside a lit area, the ring rule elsewhere") {
    const std::vector<std::array<float, 4>> areas = { { 0.0f, 2700.0f, 600.0f, 400.0f } };
    // The entrance set (far from the ring, inside the lit area) keeps its brightness.
    CHECK_NEAR(outsideDarknessAt(0.0f, 2700.0f, 0.0f, 0.0f, 650.0f, 500.0f, 0.55f, areas), 1.0f, 1e-6f);
    // Halfway through the lit area's edge: half way (smoothstep 0.5) into the darkness.
    CHECK_NEAR(outsideDarknessAt(0.0f, 2700.0f - 800.0f, 0.0f, 0.0f, 650.0f, 500.0f, 0.55f, areas), 1.0f - 0.55f * 0.5f, 1e-5f);
    // The stands at the side: full darkness, as without lit areas.
    CHECK_NEAR(outsideDarknessAt(1800.0f, 0.0f, 0.0f, 0.0f, 650.0f, 500.0f, 0.55f, areas), 0.45f, 1e-6f);
}

TEST_CASE("lit areas: without any, the darkness matches outsideDarknessFactor") {
    for (float d = 0.0f; d < 2000.0f; d += 37.0f) {
        CHECK_NEAR(outsideDarknessAt(d, 0.0f, 0.0f, 0.0f, 650.0f, 500.0f, 0.6f, {}), outsideDarknessFactor(d, 650.0f, 500.0f, 0.6f), 1e-6f);
    }
}

TEST_CASE("lit areas: parse (at most two), inherit from default, and round-trip") {
    const char *text = R"json({
        "default": { "outsideDarkness": 0.55, "litAreas": [[0, 2700, 600, 400]] },
        "venues": [
            { "name": "A", "match": ["0000000000000001"] },
            { "name": "B", "match": ["0000000000000002"], "litAreas": [[1, 2, 3, 4], [5, 6, 7, 8], [9, 9, 9, 9]] },
            { "name": "C", "match": ["0000000000000003"], "litAreas": [] }
        ]
    })json";
    const LightPresetFile file = parseLightPresets(text);
    CHECK_EQ(file.venues[0].litAreas.size(), size_t(1));
    CHECK_EQ(file.venues[1].litAreas.size(), MaxLitAreas);
    CHECK(file.venues[2].litAreas.empty());
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    for (size_t i = 0; i < file.venues.size(); i++) {
        CHECK(again.venues[i].litAreas == file.venues[i].litAreas);
        CHECK_EQ(again.venues[i].setFields, file.venues[i].setFields);
    }
}

// Flickering glows: a failing fluorescent tube.

TEST_CASE("flicker: deterministic, within 0..1, mostly on, with dropouts") {
    uint32_t on = 0, off = 0, total = 0;
    for (double t = 0.0; t < 60.0; t += 0.01, total++) {
        const float v = glowFlicker(t, 3);
        CHECK(v >= 0.0f && v <= 1.0f);
        CHECK_EQ(v, glowFlicker(t, 3));
        on += (v == 1.0f) ? 1 : 0;
        off += (v == 0.0f) ? 1 : 0;
    }

    // A failing tube, not a strobe: on most of the time, but it does drop out within a minute.
    CHECK(on > total * 7 / 10);
    CHECK(off > 0u);
}

TEST_CASE("flicker: amount 0 is steady, tubes get their own rhythm") {
    bool differ = false;
    for (double t = 0.0; t < 30.0; t += 0.05) {
        CHECK_EQ(glowFlickerScale(t, 5, 0.0f), 1.0f);
        const float half = glowFlickerScale(t, 5, 0.5f);
        CHECK(half >= 0.5f && half <= 1.0f);
        differ = differ || (glowFlicker(t, 1) != glowFlicker(t, 2));
    }

    CHECK(differ);
}

TEST_CASE("flicker: parsed as a 9th glow value and written back (colors written too)") {
    const char *text = R"json({
        "venues": [ { "name": "Hall", "match": ["0000000000000001"],
            "glows": [[0, 300, 0, 2, 480], [0, 300, 400, 2, 480, 1, 1, 1, 0.8]] } ]
    })json";
    const LightPresetFile file = parseLightPresets(text);
    CHECK_EQ(presetGlowFlicker(file.venues[0], 0), 0.0f);
    CHECK_NEAR(presetGlowFlicker(file.venues[0], 1), 0.8f, 1e-6f);
    const LightPresetFile again = parseLightPresets(writeLightPresets(file));
    CHECK(again.venues[0].glowFlicker == file.venues[0].glowFlicker);
    CHECK(again.venues[0].glowColors == file.venues[0].glowColors);
}
