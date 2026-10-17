//
// RT64
//

#include "rt64_wcw_materials.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "contrib/json/json.hpp"

#include "rt64_wcw_logic.h"

namespace RT64::WcwLogic {
    using Json = nlohmann::ordered_json;

    static const char *ClassNames[SurfaceClassCount] = { "other", "wrestler", "ropes", "posts", "mat", "apron", "steps", "metal" };

    const char *surfaceClassName(SurfaceClass surfaceClass) {
        const uint32_t index = uint32_t(surfaceClass);
        return (index < SurfaceClassCount) ? ClassNames[index] : "other";
    }

    bool parseSurfaceClass(const std::string &name, SurfaceClass &surfaceClass) {
        for (uint32_t i = 0; i < SurfaceClassCount; i++) {
            if (name == ClassNames[i]) {
                surfaceClass = SurfaceClass(i);
                return true;
            }
        }

        static const std::pair<const char *, SurfaceClass> aliases[] = {
            { "turnbuckles", SurfaceClass::Posts }, { "turnbuckle", SurfaceClass::Posts }, { "post", SurfaceClass::Posts },
            { "rope", SurfaceClass::Ropes }, { "barricades", SurfaceClass::Steps }, { "barricade", SurfaceClass::Steps },
            { "set", SurfaceClass::Other }, { "crowd", SurfaceClass::Other }, { "hud", SurfaceClass::Other }, { "text", SurfaceClass::Other }
        };

        for (const auto &alias : aliases) {
            if (name == alias.first) {
                surfaceClass = alias.second;
                return true;
            }
        }

        return false;
    }

    bool SurfaceMaterial::operator==(const SurfaceMaterial &other) const {
        return (specular == other.specular) && (shininess == other.shininess) && (rim == other.rim) && (rimPower == other.rimPower) &&
            (tint == other.tint) && (roundness == other.roundness);
    }

    static SurfaceMaterial makeMaterial(float specular, float shininess, float rim, float rimPower, float tint, float roundness) {
        SurfaceMaterial m;
        m.specular = specular;
        m.shininess = shininess;
        m.rim = rim;
        m.rimPower = rimPower;
        m.tint = tint;
        m.roundness = roundness;
        return m;
    }

    SurfaceMaterial defaultSurfaceMaterial(SurfaceClass surfaceClass) {
        switch (surfaceClass) {
        case SurfaceClass::Wrestler: {
            // The sheen as it was before materials (surface-colored highlights).
            const SheenSettings sheen = sheenSettings();
            return makeMaterial(sheen.specular, sheen.shininess, sheen.rim, sheen.rimPower, 1.0f, 0.0f);
        }
        // Glossy vinyl, round. Strong: the composite can at most double a rope's (often dark) color.
        case SurfaceClass::Ropes:
            return makeMaterial(1.0f, 14.0f, 0.3f, 3.0f, 0.0f, 1.0f);
        // Turnbuckle pads and steel posts: satin-to-metal.
        case SurfaceClass::Posts:
            return makeMaterial(0.45f, 24.0f, 0.2f, 4.0f, 0.6f, 0.0f);
        // Canvas: mostly matte, a faint sheen under each light. A broad lobe (exponent 4) just brightened the whole
        // white mat (No Mercy Raw, split screen).
        case SurfaceClass::Mat:
            return makeMaterial(0.08f, 24.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        // Satin: a soft highlight and a little rim.
        case SurfaceClass::Apron:
            return makeMaterial(0.3f, 10.0f, 0.15f, 3.0f, 0.0f, 0.0f);
        // Steel: tight highlights in the surface color.
        case SurfaceClass::Steps:
            return makeMaterial(0.55f, 40.0f, 0.2f, 4.0f, 1.0f, 0.0f);
        case SurfaceClass::Metal:
            return makeMaterial(0.6f, 48.0f, 0.25f, 4.0f, 1.0f, 0.0f);
        case SurfaceClass::Other:
        default:
            return SurfaceMaterial();
        }
    }

    SurfaceMaterial applyMaterialOverride(const SurfaceMaterial &base, const MaterialOverride &override) {
        SurfaceMaterial m = base;
        const uint32_t f = override.setFields;
        m.specular = (f & MaterialSpecular) ? override.values.specular : m.specular;
        m.shininess = (f & MaterialShininess) ? override.values.shininess : m.shininess;
        m.rim = (f & MaterialRim) ? override.values.rim : m.rim;
        m.rimPower = (f & MaterialRimPower) ? override.values.rimPower : m.rimPower;
        m.tint = (f & MaterialTint) ? override.values.tint : m.tint;
        m.roundness = (f & MaterialRoundness) ? override.values.roundness : m.roundness;
        m.specular = std::max(m.specular, 0.0f);
        m.shininess = std::max(m.shininess, 1.0f);
        m.rim = std::max(m.rim, 0.0f);
        m.rimPower = std::max(m.rimPower, 1.0f);
        m.tint = std::clamp(m.tint, 0.0f, 1.0f);
        m.roundness = std::clamp(m.roundness, 0.0f, 1.0f);
        return m;
    }

    SurfaceMaterial blendMaterials(const SurfaceMaterial &a, const SurfaceMaterial &b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        auto mix = [t](float x, float y) { return x + (y - x) * t; };
        return makeMaterial(mix(a.specular, b.specular), mix(a.shininess, b.shininess), mix(a.rim, b.rim), mix(a.rimPower, b.rimPower),
            mix(a.tint, b.tint), mix(a.roundness, b.roundness));
    }

    static MaterialOverride readOverride(const Json &j) {
        static const std::pair<const char *, uint32_t> keys[] = {
            { "specular", MaterialSpecular }, { "shininess", MaterialShininess }, { "rim", MaterialRim }, { "rimPower", MaterialRimPower },
            { "tint", MaterialTint }, { "roundness", MaterialRoundness }
        };

        MaterialOverride o;
        if (!j.is_object()) {
            return o;
        }

        for (const auto &k : keys) {
            if (!j.contains(k.first) || !j[k.first].is_number()) {
                continue;
            }

            const float v = j[k.first].get<float>();
            o.setFields |= k.second;
            switch (k.second) {
            case MaterialSpecular: o.values.specular = v; break;
            case MaterialShininess: o.values.shininess = v; break;
            case MaterialRim: o.values.rim = v; break;
            case MaterialRimPower: o.values.rimPower = v; break;
            case MaterialTint: o.values.tint = v; break;
            case MaterialRoundness: o.values.roundness = v; break;
            default: break;
            }
        }

        return o;
    }

    // "classes"-shaped objects: { "<class>": { fields }, ... }; unknown class names are ignored.
    static void readClassOverrides(const Json &j, std::array<MaterialOverride, SurfaceClassCount> &classes) {
        if (!j.is_object()) {
            return;
        }

        for (auto it = j.begin(); it != j.end(); ++it) {
            SurfaceClass c;
            if (parseSurfaceClass(it.key(), c)) {
                classes[uint32_t(c)] = readOverride(it.value());
            }
        }
    }

    MaterialTable parseMaterialTable(const std::string &text, uint32_t *skippedTextures) {
        MaterialTable table;
        uint32_t skipped = 0;
        const Json root = Json::parse(text, nullptr, true, true);
        if (root.contains("classes")) {
            readClassOverrides(root["classes"], table.classes);
        }

        if (root.contains("textures") && root["textures"].is_object()) {
            for (auto it = root["textures"].begin(); it != root["textures"].end(); ++it) {
                SurfaceClass c;
                const uint64_t hash = strtoull(it.key().c_str(), nullptr, 16);
                if ((hash == 0) || !it.value().is_string() || !parseSurfaceClass(it.value().get<std::string>(), c)) {
                    skipped++;
                    continue;
                }

                // Other entries (art-styles tags like "crowd") only document the texture.
                if (c != SurfaceClass::Other) {
                    table.textures[hash] = c;
                }
            }
        }

        if (root.contains("ropes") && root["ropes"].is_object()) {
            const Json &r = root["ropes"];
            table.ropes.enabled = r.value("enabled", true);
            table.ropes.minLength = r.value("minLength", table.ropes.minLength);
            table.ropes.maxThickness = r.value("maxThickness", table.ropes.maxThickness);
            table.ropes.maxHeight = r.value("maxHeight", table.ropes.maxHeight);
            table.ropes.minY = r.value("minY", table.ropes.minY);
        }

        if (root.contains("venues") && root["venues"].is_object()) {
            for (auto it = root["venues"].begin(); it != root["venues"].end(); ++it) {
                VenueMaterials v;
                v.name = it.key();
                readClassOverrides(it.value(), v.classes);
                table.venues.push_back(std::move(v));
            }
        }

        if (skippedTextures != nullptr) {
            *skippedTextures = skipped;
        }

        return table;
    }

    std::array<SurfaceMaterial, SurfaceClassCount> resolveMaterials(const MaterialTable &table, const std::string &venueName) {
        std::array<SurfaceMaterial, SurfaceClassCount> materials;
        const VenueMaterials *venue = nullptr;
        for (const VenueMaterials &v : table.venues) {
            if (!venueName.empty() && (v.name == venueName)) {
                venue = &v;
            }
        }

        for (uint32_t i = 0; i < SurfaceClassCount; i++) {
            SurfaceMaterial m = applyMaterialOverride(defaultSurfaceMaterial(SurfaceClass(i)), table.classes[i]);
            if (venue != nullptr) {
                m = applyMaterialOverride(m, venue->classes[i]);
            }

            materials[i] = m;
        }

        return materials;
    }

    bool ropeShaped(const RopeRule &rule, const DrawShape &shape) {
        if (!rule.enabled || shape.character || (shape.textureHash != 0)) {
            return false;
        }

        const float dx = shape.boundsMax[0] - shape.boundsMin[0];
        const float dy = shape.boundsMax[1] - shape.boundsMin[1];
        const float dz = shape.boundsMax[2] - shape.boundsMin[2];
        if ((dx < 0.0f) || (dy < 0.0f) || (dz < 0.0f)) {
            return false;
        }

        const float length = std::max(dx, dz);
        const float thickness = std::min(dx, dz);
        return (length >= rule.minLength) && (thickness <= rule.maxThickness) && (dy <= rule.maxHeight) && (shape.boundsMin[1] >= rule.minY);
    }

    SurfaceClass surfaceClassFor(const MaterialTable &table, const DrawShape &shape) {
        if (shape.character) {
            return SurfaceClass::Wrestler;
        }

        if (shape.textureHash != 0) {
            const auto it = table.textures.find(shape.textureHash);
            return (it != table.textures.end()) ? it->second : SurfaceClass::Other;
        }

        return ropeShaped(table.ropes, shape) ? SurfaceClass::Ropes : SurfaceClass::Other;
    }

    static Vec3 sub(const Vec3 &a, const Vec3 &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    static float dot3(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static Vec3 scale3(const Vec3 &a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }

    Vec3 ropeBentNormal(const Vec3 &v0, const Vec3 &v1, const Vec3 &v2, const Vec3 &hit, const Vec3 &faceNormal, float roundness) {
        roundness = std::clamp(roundness, 0.0f, 1.0f);
        if (roundness <= 0.0f) {
            return faceNormal;
        }

        // Edge k runs from vertex k to vertex k + 1; the vertex opposite it is k + 2.
        const Vec3 v[3] = { v0, v1, v2 };
        float len[3];
        for (uint32_t k = 0; k < 3; k++) {
            len[k] = std::sqrt(dot3(sub(v[(k + 1) % 3], v[k]), sub(v[(k + 1) % 3], v[k])));
        }

        // The side edge has the middle length: the diagonal is longer, the strip's width shorter.
        uint32_t order[3] = { 0, 1, 2 };
        std::sort(order, order + 3, [&len](uint32_t a, uint32_t b) { return len[a] < len[b]; });
        const uint32_t side = order[1];
        if ((len[side] <= 1e-6f) || (len[order[0]] > 0.5f * len[side])) {
            return faceNormal;
        }

        const Vec3 &p0 = v[side];
        const Vec3 axis = scale3(sub(v[(side + 1) % 3], p0), 1.0f / len[side]);
        const Vec3 toApex = sub(v[(side + 2) % 3], p0);
        const Vec3 across = sub(toApex, scale3(axis, dot3(toApex, axis)));
        const float width = std::sqrt(dot3(across, across));
        if (width <= 1e-6f) {
            return faceNormal;
        }

        const Vec3 acrossDir = scale3(across, 1.0f / width);
        // -1 on the side edge, +1 at the far side; the normal turns toward whichever edge is nearer.
        const float t = std::clamp(dot3(sub(hit, p0), acrossDir) / width, 0.0f, 1.0f);
        const float u = std::clamp((2.0f * t - 1.0f) * roundness, -0.95f, 0.95f);
        const float up = std::sqrt(1.0f - u * u);
        Vec3 n = { faceNormal[0] * up + acrossDir[0] * u, faceNormal[1] * up + acrossDir[1] * u, faceNormal[2] * up + acrossDir[2] * u };
        const float nLen = std::sqrt(dot3(n, n));
        return (nLen > 1e-6f) ? scale3(n, 1.0f / nLen) : faceNormal;
    }

    float highlightGain(float tint, float surfaceLuma) {
        tint = std::clamp(tint, 0.0f, 1.0f);
        const float dielectric = 1.0f / std::max(std::clamp(surfaceLuma, 0.0f, 1.0f), MinHighlightLuma);
        return dielectric + (1.0f - dielectric) * tint;
    }
};
