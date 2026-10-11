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

static bool samePreset(const LightPreset &a, const LightPreset &b) {
    return (a.name == b.name) && (a.match == b.match) && (a.center[0] == b.center[0]) && (a.center[1] == b.center[1]) &&
        (a.center[2] == b.center[2]) && (a.spread == b.spread) && (a.fills == b.fills) && (a.keyWeight == b.keyWeight) &&
        (a.lights == b.lights) && (a.size == b.size) && (a.radius == b.radius) && (a.clearance == b.clearance) &&
        (a.falloff == b.falloff) && (a.glows == b.glows) && (a.anchorScene == b.anchorScene) && (a.shadow == b.shadow) &&
        (a.characterShadow == b.characterShadow);
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
