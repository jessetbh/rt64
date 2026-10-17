//
// RT64
//

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// [wcw rt] Per-surface materials (assets/rt_materials.json, aki-roadmap#95): the wrestler sheen generalized to surface
// classes (ropes, posts, mat, apron, ...). Classes come from a per-game table of texture hashes (the same key texture
// packs use) plus a geometry rule for the ropes, which AKI draws untextured. Each class has a material (highlight,
// shininess, rim, tint, roundness); the table and each venue can override any field. Kept free of renderer state so
// it can be unit tested (tests/wcw/rt64_wcw_materials_tests.cpp); the ray tracer calls these and the trace shader
// (WcwRtTraceCS) mirrors ropeBentNormal and highlightGain.
namespace RT64::WcwLogic {
    // Values fit in WCW_RT_TILE_CLASS_BITS bits of the trace's per-mesh tile word.
    enum class SurfaceClass : uint32_t {
        // Everything else: no highlights (the RT look before materials).
        Other = 0,
        // Character draws (wrestlers): the sheen.
        Wrestler = 1,
        Ropes = 2,
        // Ring posts and turnbuckles.
        Posts = 3,
        Mat = 4,
        Apron = 5,
        // Steel steps and barricades.
        Steps = 6,
        // Metal set pieces (entrance stage trusses, ramps).
        Metal = 7,
        Count = 8
    };

    constexpr uint32_t SurfaceClassCount = uint32_t(SurfaceClass::Count);

    // The JSON name of a class ("other", "wrestler", "ropes", "posts", "mat", "apron", "steps", "metal").
    const char *surfaceClassName(SurfaceClass surfaceClass);

    // A class from its JSON name or an alias: "turnbuckles" = posts, "barricades" / "barricade" = steps, and the
    // art-styles tags that carry no material ("set", "crowd", "hud", "text") = other. false for unknown names.
    bool parseSurfaceClass(const std::string &name, SurfaceClass &surfaceClass);

    // How a surface catches the venue lights. Highlights and rim come from the same lights and shadow rays as the
    // shadows (no extra rays) and brighten the shadow factor, which goes up to 2.
    struct SurfaceMaterial {
        // Peak Blinn-Phong highlight of a fully lit pixel (0 = none).
        float specular = 0.0f;
        // Blinn-Phong exponent: higher is a tighter, glossier highlight.
        float shininess = 1.0f;
        // Peak outline toward the lights at grazing angles, and how quickly it fades inward (higher is thinner).
        float rim = 0.0f;
        float rimPower = 1.0f;
        // 0 = dielectric (vinyl, canvas, satin): the highlight reads in the light's color, so it is lifted on dark
        // surfaces (highlightGain); 1 = metal: the highlight takes the surface's color (also the wrestler sheen, as
        // it always was).
        float tint = 0.0f;
        // Rope normal bend: 0 = the strip's flat normal, 1 = bent across the strip's width like a cylinder
        // (ropeBentNormal), so the highlight runs along the rope as the camera moves.
        float roundness = 0.0f;

        bool operator==(const SurfaceMaterial &other) const;
    };

    // Which fields a material override sets (MaterialOverride::setFields).
    enum MaterialField : uint32_t {
        MaterialSpecular = 1u << 0,
        MaterialShininess = 1u << 1,
        MaterialRim = 1u << 2,
        MaterialRimPower = 1u << 3,
        MaterialTint = 1u << 4,
        MaterialRoundness = 1u << 5
    };

    // Some fields of a material: the game table or a venue changing only what it names.
    struct MaterialOverride {
        uint32_t setFields = 0;
        SurfaceMaterial values;
    };

    // The built-in material of a class. Wrestler is sheenSettings(); Other has no highlights.
    SurfaceMaterial defaultSurfaceMaterial(SurfaceClass surfaceClass);

    // base with the fields the override sets replaced, then clamped to valid ranges (strengths >= 0, exponents >= 1,
    // tint and roundness 0..1).
    SurfaceMaterial applyMaterialOverride(const SurfaceMaterial &base, const MaterialOverride &override);

    // A field-wise mix of two materials (t = 0: a, 1: b, clamped), e.g. when a venue fades in.
    SurfaceMaterial blendMaterials(const SurfaceMaterial &a, const SurfaceMaterial &b, float t);

    // Ropes are drawn untextured, so they are recognized by shape: an untextured world draw whose bounds are long on
    // one horizontal axis, thin on the other and not tall (one side's ropes, stacked). All in world units.
    struct RopeRule {
        bool enabled = false;
        float minLength = 200.0f;
        float maxThickness = 12.0f;
        float maxHeight = 120.0f;
        // Bounds below this height (the floor, the mat edge) are not ropes.
        float minY = -1.0e9f;
    };

    // Per-venue overrides; name matches a venue of rt_lights.json.
    struct VenueMaterials {
        std::string name;
        std::array<MaterialOverride, SurfaceClassCount> classes;
    };

    // The whole rt_materials.json.
    struct MaterialTable {
        std::array<MaterialOverride, SurfaceClassCount> classes;
        // Texture hash (16 hex digits in the file, as in texture packs) to class.
        std::unordered_map<uint64_t, SurfaceClass> textures;
        std::vector<VenueMaterials> venues;
        RopeRule ropes;
    };

    // Parses rt_materials.json text (comments allowed):
    //   { "classes": { "ropes": { "specular": 0.6, "shininess": 40, "rim": 0.2, "rimPower": 4, "tint": 0, "roundness": 1 }, ... },
    //     "textures": { "1B97A9B8D43D12A1": "mat", ... },
    //     "ropes": { "minLength": 200, "maxThickness": 12, "maxHeight": 120, "minY": 0 },
    //     "venues": { "Raw": { "ropes": { "specular": 0.8 } }, ... } }
    // Unknown class names in "textures" are skipped (counted in skippedTextures when given). Throws std::exception on
    // malformed JSON.
    MaterialTable parseMaterialTable(const std::string &text, uint32_t *skippedTextures = nullptr);

    // The materials in use for a venue (empty name or no match: none): defaults, then the table's classes, then the
    // venue's overrides.
    std::array<SurfaceMaterial, SurfaceClassCount> resolveMaterials(const MaterialTable &table, const std::string &venueName);

    // A draw's world-space bounds and what it draws, for classifying it.
    struct DrawShape {
        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
        // The draw's first texture (0 = untextured).
        uint64_t textureHash = 0;
        bool character = false;
    };

    // Whether an untextured world draw is shaped like ropes (RopeRule).
    bool ropeShaped(const RopeRule &rule, const DrawShape &shape);

    // A draw's class: character draws are Wrestler; then the texture table; then the rope rule; else Other.
    SurfaceClass surfaceClassFor(const MaterialTable &table, const DrawShape &shape);

    using Vec3 = std::array<float, 3>;

    // The shading normal at hit on a strip triangle (v0, v1, v2), bent across the strip's width as if the strip were
    // the visible half of a cylinder: flat along the middle, turning toward each long edge. The long edge that isn't
    // the diagonal is one side of the strip and the opposite vertex lies on the other, so both triangles of a quad
    // bend the same way. roundness 0..1 scales the bend; faceNormal (unit, facing the viewer) comes back unchanged for
    // roundness 0 and for triangles that aren't thin (shortest edge over half the side edge).
    Vec3 ropeBentNormal(const Vec3 &v0, const Vec3 &v1, const Vec3 &v2, const Vec3 &hit, const Vec3 &faceNormal, float roundness);

    // The highlight's gain on a surface of this luminance (0..1; untextured draws pass 1): the composite multiplies
    // the surface color by the shadow factor, so a dielectric's highlight (tint 0) is divided by the luminance (at
    // least MinHighlightLuma) to approach the light's color on a dark surface; tint 1 keeps gain 1 and takes the
    // surface color. In between, the gain is interpolated.
    constexpr float MinHighlightLuma = 0.25f;
    float highlightGain(float tint, float surfaceLuma);
};
