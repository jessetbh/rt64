//
// RT64
//

// [wcw rt] Unit tests for common/rt64_wcw_materials (per-surface RT materials: classes, the table file, venue overrides,
// the rope rule and normal bend, the highlight gain). Built and run with the other wcw tests on every rt64 build.

#include "common/rt64_wcw_materials.h"
#include "common/rt64_wcw_logic.h"

#include <cmath>
#include <string>

#include "wcw_test.h"

using namespace RT64::WcwLogic;

// The shape of No Mercy's assets/rt_materials.json.
static const char *SampleTable = R"json(
{
    // Class defaults for this game.
    "classes": {
        "ropes": { "specular": 0.7, "roundness": 0.8 },
        "mat": { "specular": 0.05 },
        "bogus": { "specular": 9 }
    },
    "textures": {
        "1B97A9B8D43D12A1": "mat",
        "3c496c10a0bec746": "apron",
        "6154243C96505CB8": "turnbuckles",
        "13F4382E360BE647": "crowd",
        "0000000000000000": "mat",
        "222D755186CF8DCE": "lava"
    },
    "ropes": { "minLength": 150, "maxThickness": 10, "maxHeight": 100, "minY": 5 },
    "venues": {
        "Raw": { "ropes": { "specular": 0.9, "tint": 0.25 }, "turnbuckles": { "tint": 1 } },
        "SmackDown": { "mat": { "shininess": 0.5 } }
    }
}
)json";

static float length3(const Vec3 &v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

TEST_CASE("surface class names round-trip; aliases map to classes; art-styles tags without a material are other") {
    for (uint32_t i = 0; i < SurfaceClassCount; i++) {
        SurfaceClass c = SurfaceClass::Count;
        CHECK(parseSurfaceClass(surfaceClassName(SurfaceClass(i)), c));
        CHECK_EQ(uint32_t(c), i);
    }

    SurfaceClass c = SurfaceClass::Other;
    CHECK(parseSurfaceClass("turnbuckles", c) && (c == SurfaceClass::Posts));
    CHECK(parseSurfaceClass("barricades", c) && (c == SurfaceClass::Steps));
    CHECK(parseSurfaceClass("set", c) && (c == SurfaceClass::Other));
    CHECK(parseSurfaceClass("crowd", c) && (c == SurfaceClass::Other));
    CHECK(!parseSurfaceClass("lava", c));
    CHECK(std::string(surfaceClassName(SurfaceClass::Count)) == "other");
}

TEST_CASE("the wrestler material is the sheen as before materials (surface-colored); other has no highlights") {
    const SheenSettings sheen = sheenSettings();
    const SurfaceMaterial w = defaultSurfaceMaterial(SurfaceClass::Wrestler);
    CHECK_EQ(w.specular, sheen.specular);
    CHECK_EQ(w.shininess, sheen.shininess);
    CHECK_EQ(w.rim, sheen.rim);
    CHECK_EQ(w.rimPower, sheen.rimPower);
    CHECK_EQ(w.tint, 1.0f);
    CHECK_EQ(w.roundness, 0.0f);
    const SurfaceMaterial o = defaultSurfaceMaterial(SurfaceClass::Other);
    CHECK_EQ(o.specular, 0.0f);
    CHECK_EQ(o.rim, 0.0f);
}

TEST_CASE("class defaults: ropes round glossy dielectric, metal classes tint, mat the faintest highlight") {
    CHECK_EQ(defaultSurfaceMaterial(SurfaceClass::Ropes).roundness, 1.0f);
    CHECK_EQ(defaultSurfaceMaterial(SurfaceClass::Ropes).tint, 0.0f);
    CHECK_EQ(defaultSurfaceMaterial(SurfaceClass::Steps).tint, 1.0f);
    CHECK_EQ(defaultSurfaceMaterial(SurfaceClass::Metal).tint, 1.0f);
    for (uint32_t i = 0; i < SurfaceClassCount; i++) {
        const SurfaceMaterial m = defaultSurfaceMaterial(SurfaceClass(i));
        CHECK(m.shininess >= 1.0f);
        CHECK(m.rimPower >= 1.0f);
        // The mat is the biggest bright surface: its sheen must stay faint, or the whole mat brightens.
        if ((SurfaceClass(i) != SurfaceClass::Mat) && (SurfaceClass(i) != SurfaceClass::Other)) {
            CHECK(m.specular > defaultSurfaceMaterial(SurfaceClass::Mat).specular);
        }

        CHECK(defaultSurfaceMaterial(SurfaceClass::Mat).specular <= 0.1f);
    }
}

TEST_CASE("an override replaces only the fields it sets, then clamps") {
    const SurfaceMaterial base = defaultSurfaceMaterial(SurfaceClass::Apron);
    MaterialOverride none;
    CHECK(applyMaterialOverride(base, none) == base);

    MaterialOverride o;
    o.setFields = MaterialSpecular | MaterialTint;
    o.values.specular = 0.8f;
    o.values.tint = 0.5f;
    o.values.shininess = 99.0f;
    const SurfaceMaterial m = applyMaterialOverride(base, o);
    CHECK_EQ(m.specular, 0.8f);
    CHECK_EQ(m.tint, 0.5f);
    CHECK_EQ(m.shininess, base.shininess);
    CHECK_EQ(m.rim, base.rim);

    MaterialOverride bad;
    bad.setFields = MaterialSpecular | MaterialShininess | MaterialRim | MaterialRimPower | MaterialTint | MaterialRoundness;
    bad.values.specular = -1.0f;
    bad.values.shininess = 0.0f;
    bad.values.rim = -2.0f;
    bad.values.rimPower = 0.5f;
    bad.values.tint = 3.0f;
    bad.values.roundness = -0.5f;
    const SurfaceMaterial c = applyMaterialOverride(base, bad);
    CHECK_EQ(c.specular, 0.0f);
    CHECK_EQ(c.shininess, 1.0f);
    CHECK_EQ(c.rim, 0.0f);
    CHECK_EQ(c.rimPower, 1.0f);
    CHECK_EQ(c.tint, 1.0f);
    CHECK_EQ(c.roundness, 0.0f);
}

TEST_CASE("blending materials is field-wise and clamps t") {
    const SurfaceMaterial a = defaultSurfaceMaterial(SurfaceClass::Mat);
    const SurfaceMaterial b = defaultSurfaceMaterial(SurfaceClass::Metal);
    CHECK(blendMaterials(a, b, 0.0f) == a);
    CHECK(blendMaterials(a, b, 1.0f) == b);
    CHECK(blendMaterials(a, b, -3.0f) == a);
    CHECK(blendMaterials(a, b, 7.0f) == b);
    const SurfaceMaterial h = blendMaterials(a, b, 0.5f);
    CHECK_NEAR(h.specular, (a.specular + b.specular) * 0.5f, 1e-6f);
    CHECK_NEAR(h.shininess, (a.shininess + b.shininess) * 0.5f, 1e-5f);
    CHECK_NEAR(h.tint, 0.5f, 1e-6f);
}

TEST_CASE("the table parses classes, textures (case-insensitive hex, aliases), the rope rule and venues") {
    uint32_t skipped = 99;
    const MaterialTable t = parseMaterialTable(SampleTable, &skipped);
    // A zero hash and an unknown class are skipped; "crowd" documents the texture but isn't stored.
    CHECK_EQ(skipped, 2u);
    CHECK_EQ(t.textures.size(), size_t(3));
    CHECK(t.textures.at(0x1B97A9B8D43D12A1ull) == SurfaceClass::Mat);
    CHECK(t.textures.at(0x3C496C10A0BEC746ull) == SurfaceClass::Apron);
    CHECK(t.textures.at(0x6154243C96505CB8ull) == SurfaceClass::Posts);
    CHECK(t.textures.count(0x13F4382E360BE647ull) == 0);
    CHECK_EQ(t.classes[uint32_t(SurfaceClass::Ropes)].setFields, uint32_t(MaterialSpecular | MaterialRoundness));
    CHECK_EQ(t.classes[uint32_t(SurfaceClass::Ropes)].values.specular, 0.7f);
    CHECK(t.ropes.enabled);
    CHECK_EQ(t.ropes.minLength, 150.0f);
    CHECK_EQ(t.ropes.maxThickness, 10.0f);
    CHECK_EQ(t.ropes.maxHeight, 100.0f);
    CHECK_EQ(t.ropes.minY, 5.0f);
    CHECK_EQ(t.venues.size(), size_t(2));
    CHECK(t.venues[0].name == "Raw");
    CHECK_EQ(t.venues[0].classes[uint32_t(SurfaceClass::Posts)].setFields, uint32_t(MaterialTint));
}

TEST_CASE("an empty table: no textures, rope rule off, defaults in use") {
    const MaterialTable t = parseMaterialTable("{}");
    CHECK(t.textures.empty());
    CHECK(!t.ropes.enabled);
    const auto m = resolveMaterials(t, "Raw");
    for (uint32_t i = 0; i < SurfaceClassCount; i++) {
        CHECK(m[i] == defaultSurfaceMaterial(SurfaceClass(i)));
    }
}

TEST_CASE("malformed table JSON throws") {
    bool threw = false;
    try {
        parseMaterialTable("{ \"classes\": ");
    }
    catch (const std::exception &) {
        threw = true;
    }

    CHECK(threw);
}

TEST_CASE("materials resolve defaults, then the game table, then the venue") {
    const MaterialTable t = parseMaterialTable(SampleTable);
    const auto none = resolveMaterials(t, "");
    const auto raw = resolveMaterials(t, "Raw");
    const auto smack = resolveMaterials(t, "SmackDown");
    const auto unknown = resolveMaterials(t, "Backlash");
    const uint32_t ropes = uint32_t(SurfaceClass::Ropes);
    const uint32_t posts = uint32_t(SurfaceClass::Posts);
    const uint32_t mat = uint32_t(SurfaceClass::Mat);

    // Game table over the defaults.
    CHECK_EQ(none[ropes].specular, 0.7f);
    CHECK_EQ(none[ropes].roundness, 0.8f);
    CHECK_EQ(none[ropes].shininess, defaultSurfaceMaterial(SurfaceClass::Ropes).shininess);
    CHECK_EQ(none[mat].specular, 0.05f);

    // Venue over the game table, only for its fields.
    CHECK_EQ(raw[ropes].specular, 0.9f);
    CHECK_EQ(raw[ropes].tint, 0.25f);
    CHECK_EQ(raw[ropes].roundness, 0.8f);
    CHECK_EQ(raw[posts].tint, 1.0f);
    CHECK_EQ(raw[posts].specular, defaultSurfaceMaterial(SurfaceClass::Posts).specular);

    // Venue values clamp too; other venues don't see Raw's overrides.
    CHECK_EQ(smack[mat].shininess, 1.0f);
    CHECK_EQ(smack[ropes].specular, 0.7f);
    for (uint32_t i = 0; i < SurfaceClassCount; i++) {
        CHECK(unknown[i] == none[i]);
    }
}

static DrawShape shape(float x0, float y0, float z0, float x1, float y1, float z1, uint64_t hash = 0, bool character = false) {
    DrawShape s;
    s.boundsMin[0] = x0; s.boundsMin[1] = y0; s.boundsMin[2] = z0;
    s.boundsMax[0] = x1; s.boundsMax[1] = y1; s.boundsMax[2] = z1;
    s.textureHash = hash;
    s.character = character;
    return s;
}

TEST_CASE("rope rule: long, thin, low untextured world draws on either horizontal axis") {
    RopeRule r;
    r.enabled = true;
    r.minLength = 200.0f;
    r.maxThickness = 12.0f;
    r.maxHeight = 120.0f;
    r.minY = 10.0f;
    // One side's three ropes along X, then along Z.
    CHECK(ropeShaped(r, shape(-300, 40, 295, 300, 140, 303)));
    CHECK(ropeShaped(r, shape(295, 40, -300, 303, 140, 300)));
    // Too short, too thick, too tall, too low (the mat edge), textured, a character.
    CHECK(!ropeShaped(r, shape(-90, 40, 295, 90, 140, 303)));
    CHECK(!ropeShaped(r, shape(-300, 40, 280, 300, 140, 303)));
    CHECK(!ropeShaped(r, shape(-300, 0, 295, 300, 200, 303)));
    CHECK(!ropeShaped(r, shape(-300, 0, 295, 300, 4, 303)));
    CHECK(!ropeShaped(r, shape(-300, 40, 295, 300, 140, 303, 0x1234)));
    CHECK(!ropeShaped(r, shape(-300, 40, 295, 300, 140, 303, 0, true)));
    // Empty bounds, and the rule switched off.
    CHECK(!ropeShaped(r, shape(1, 1, 1, 0, 0, 0)));
    r.enabled = false;
    CHECK(!ropeShaped(r, shape(-300, 40, 295, 300, 140, 303)));
}

TEST_CASE("draw classes: characters first, then the texture table, then the rope rule") {
    const MaterialTable t = parseMaterialTable(SampleTable);
    // A character wearing a mat-classed texture is still a wrestler.
    CHECK(surfaceClassFor(t, shape(0, 0, 0, 10, 10, 10, 0x1B97A9B8D43D12A1ull, true)) == SurfaceClass::Wrestler);
    CHECK(surfaceClassFor(t, shape(0, 0, 0, 10, 10, 10, 0x1B97A9B8D43D12A1ull)) == SurfaceClass::Mat);
    CHECK(surfaceClassFor(t, shape(0, 0, 0, 10, 10, 10, 0x13F4382E360BE647ull)) == SurfaceClass::Other);
    CHECK(surfaceClassFor(t, shape(-300, 40, 295, 300, 120, 303)) == SurfaceClass::Ropes);
    // A rope-shaped textured draw goes by its texture.
    CHECK(surfaceClassFor(t, shape(-300, 40, 295, 300, 120, 303, 0x3C496C10A0BEC746ull)) == SurfaceClass::Apron);
    CHECK(surfaceClassFor(t, shape(-300, 40, 295, 300, 120, 303, 0x99)) == SurfaceClass::Other);
}

// A rope strip along X, 4 units wide on Z, facing up (+Y), as two triangles of the quad (0,0,0) (100,0,0) (100,0,4) (0,0,4).
static const Vec3 Q0 = { 0, 0, 0 }, Q1 = { 100, 0, 0 }, Q2 = { 100, 0, 4 }, Q3 = { 0, 0, 4 };
static const Vec3 Up = { 0, 1, 0 };

TEST_CASE("rope bend: flat in the middle, turned toward the nearer long edge, the same on both triangles of a quad") {
    // Triangle A (Q0, Q1, Q2): side edge Q0-Q1 (z = 0). Triangle B (Q0, Q2, Q3): side edge Q2-Q3 (z = 4).
    for (int tri = 0; tri < 2; tri++) {
        const Vec3 &a = Q0;
        const Vec3 &b = tri ? Q2 : Q1;
        const Vec3 &c = tri ? Q3 : Q2;
        // A middle point that lies in this triangle (x = 60 is below the diagonal at z = 2 for A, above for B).
        const Vec3 mid = { tri ? 40.0f : 60.0f, 0.0f, 2.0f };
        const Vec3 nMid = ropeBentNormal(a, b, c, mid, Up, 1.0f);
        CHECK_NEAR(nMid[1], 1.0f, 1e-5f);

        const Vec3 nearZ0 = { tri ? 5.0f : 95.0f, 0.0f, 0.2f };
        const Vec3 nearZ4 = { tri ? 5.0f : 95.0f, 0.0f, 3.8f };
        const Vec3 n0 = ropeBentNormal(a, b, c, nearZ0, Up, 1.0f);
        const Vec3 n4 = ropeBentNormal(a, b, c, nearZ4, Up, 1.0f);
        // Toward -Z near z = 0, toward +Z near z = 4, never along the rope.
        CHECK(n0[2] < -0.7f);
        CHECK(n4[2] > 0.7f);
        CHECK(n0[1] > 0.0f);
        CHECK_NEAR(n0[0], 0.0f, 1e-5f);
        CHECK_NEAR(length3(n0), 1.0f, 1e-5f);
        CHECK_NEAR(length3(n4), 1.0f, 1e-5f);
    }
}

TEST_CASE("rope bend: roundness scales it; 0 and non-thin triangles keep the face normal") {
    const Vec3 hit = { 95, 0, 0.2f };
    const Vec3 full = ropeBentNormal(Q0, Q1, Q2, hit, Up, 1.0f);
    const Vec3 half = ropeBentNormal(Q0, Q1, Q2, hit, Up, 0.5f);
    CHECK(half[2] < 0.0f);
    CHECK(half[2] > full[2]);
    const Vec3 flat = ropeBentNormal(Q0, Q1, Q2, hit, Up, 0.0f);
    CHECK_EQ(flat[1], 1.0f);
    // A fat triangle (not a strip): unchanged.
    const Vec3 f0 = { 0, 0, 0 }, f1 = { 10, 0, 0 }, f2 = { 5, 0, 8 };
    const Vec3 fat = ropeBentNormal(f0, f1, f2, Vec3{ 5, 0, 1 }, Up, 1.0f);
    CHECK_EQ(fat[1], 1.0f);
    // Degenerate: unchanged.
    const Vec3 deg = ropeBentNormal(f0, f0, f0, f0, Up, 1.0f);
    CHECK_EQ(deg[1], 1.0f);
}

TEST_CASE("rope bend: the vertex order doesn't matter") {
    const Vec3 hit = { 70, 0, 0.5f };
    const Vec3 a = ropeBentNormal(Q0, Q1, Q2, hit, Up, 1.0f);
    const Vec3 b = ropeBentNormal(Q1, Q2, Q0, hit, Up, 1.0f);
    const Vec3 c = ropeBentNormal(Q2, Q1, Q0, hit, Up, 1.0f);
    for (int k = 0; k < 3; k++) {
        CHECK_NEAR(a[k], b[k], 1e-5f);
        CHECK_NEAR(a[k], c[k], 1e-5f);
    }
}

TEST_CASE("highlight gain: dielectrics lift dark surfaces up to 1 / MinHighlightLuma, tint 1 keeps 1") {
    CHECK_NEAR(highlightGain(0.0f, 1.0f), 1.0f, 1e-6f);
    CHECK_NEAR(highlightGain(0.0f, 0.5f), 2.0f, 1e-6f);
    CHECK_NEAR(highlightGain(0.0f, 0.0f), 1.0f / MinHighlightLuma, 1e-5f);
    CHECK_NEAR(highlightGain(0.0f, -1.0f), 1.0f / MinHighlightLuma, 1e-5f);
    CHECK_NEAR(highlightGain(1.0f, 0.1f), 1.0f, 1e-6f);
    CHECK_NEAR(highlightGain(0.5f, 0.5f), 1.5f, 1e-6f);
    CHECK_NEAR(highlightGain(2.0f, 0.5f), 1.0f, 1e-6f);
}
