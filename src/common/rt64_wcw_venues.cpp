//
// RT64
//

#include "rt64_wcw_venues.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "contrib/json/json.hpp"

namespace RT64::WcwLogic {
    using Json = nlohmann::ordered_json;

    static uint32_t presentFields(const Json &j) {
        static const std::pair<const char *, uint32_t> keys[] = {
            { "center", FieldCenter }, { "spread", FieldSpread }, { "fills", FieldFills }, { "keyWeight", FieldKeyWeight },
            { "lights", FieldLights }, { "size", FieldSize }, { "radius", FieldRadius }, { "clearance", FieldClearance },
            { "falloff", FieldFalloff }, { "glows", FieldGlows }, { "anchor", FieldAnchor }, { "shadow", FieldShadow },
            { "characterShadow", FieldCharacterShadow }, { "outsideDarkness", FieldOutsideDarkness }, { "outsideEdge", FieldOutsideEdge }
        };

        uint32_t fields = 0;
        for (const auto &k : keys) {
            fields |= j.contains(k.first) ? k.second : 0u;
        }

        return fields;
    }

    static LightPreset parsePreset(const Json &j, const LightPreset &base) {
        LightPreset p = base;
        p.setFields = presentFields(j);
        p.name = j.value("name", std::string());
        p.match.clear();
        p.uniqueMatch.clear();
        if (j.contains("match")) {
            for (const auto &h : j["match"]) {
                p.match.push_back(strtoull(h.get<std::string>().c_str(), nullptr, 16));
            }
        }

        if (j.contains("center")) {
            for (uint32_t i = 0; i < 3; i++) {
                p.center[i] = j["center"][i].get<float>();
            }
        }

        p.spread = j.value("spread", p.spread);
        p.fills = j.value("fills", p.fills);
        p.keyWeight = j.value("keyWeight", p.keyWeight);
        p.size = j.value("size", p.size);
        p.radius = j.value("radius", p.radius);
        p.clearance = j.value("clearance", p.clearance);
        p.falloff = j.value("falloff", p.falloff);
        if (j.contains("glows")) {
            p.glows.clear();
            for (const auto &g : j["glows"]) {
                p.glows.push_back({ g[0].get<float>(), g[1].get<float>(), g[2].get<float>(), g[3].get<float>(), g[4].get<float>() });
            }
        }

        p.shadow = j.value("shadow", p.shadow);
        p.characterShadow = j.value("characterShadow", p.characterShadow);
        p.outsideDarkness = std::clamp(j.value("outsideDarkness", p.outsideDarkness), 0.0f, 1.0f);
        p.outsideEdge = std::max(j.value("outsideEdge", p.outsideEdge), 0.0f);
        p.anchorScene = (j.value("anchor", std::string(p.anchorScene ? "scene" : "world")) == "scene");
        if (j.contains("lights")) {
            p.lights.clear();
            for (const auto &l : j["lights"]) {
                p.lights.push_back({ l[0].get<float>(), l[1].get<float>(), l[2].get<float>(), (l.size() > 3) ? l[3].get<float>() : 1.0f });
            }
        }

        return p;
    }

    LightPresetFile parseLightPresets(const std::string &text) {
        const Json root = Json::parse(text, nullptr, true, true);
        LightPresetFile file;
        if (root.contains("debugMode")) {
            file.hasDebugMode = true;
            file.debugMode = root["debugMode"].get<uint32_t>();
        }

        if (root.contains("default")) {
            file.defaultPreset = parsePreset(root["default"], LightPreset());
        }

        file.defaultPreset.name = "default";
        if (root.contains("room")) {
            const Json &room = root["room"];
            file.roomPreset = parsePreset(room, file.defaultPreset);
            file.roomPreset.anchorScene = false;
            file.roomMaxExtent = room.value("maxExtent", file.roomMaxExtent);
            file.roomCount = std::clamp(room.value("count", file.roomCount), 1u, 8u);
            file.roomHeight = room.value("height", file.roomHeight);
            file.roomEnabled = true;
        }

        file.roomPreset.name = "room (fallback)";
        if (root.contains("venues")) {
            for (const auto &v : root["venues"]) {
                file.venues.push_back(parsePreset(v, file.defaultPreset));
            }
        }

        computeUniqueMatches(file.venues);
        return file;
    }

    std::string formatTextureHash(uint64_t hash) {
        char text[17];
        snprintf(text, sizeof(text), "%016" PRIX64, hash);
        return text;
    }

    // Numbers are written with 6 significant digits, so a float read from the file writes back unchanged.
    static Json number(float value) {
        char text[32];
        snprintf(text, sizeof(text), "%.6g", double(value));
        const double parsed = strtod(text, nullptr);
        if (parsed == double(int64_t(parsed))) {
            return Json(int64_t(parsed));
        }

        return Json(parsed);
    }

    // Writes the fields p sets itself, plus (for "default") any that differ from base.
    static Json writePreset(const LightPreset &p, const LightPreset &base, bool writeName, bool writeMatch, bool writeChanged) {
        auto wanted = [&](uint32_t field, bool changed) { return ((p.setFields & field) != 0) || (writeChanged && changed); };
        Json j = Json::object();
        if (writeName) {
            j["name"] = p.name;
        }

        if (writeMatch && !p.match.empty()) {
            Json m = Json::array();
            for (uint64_t h : p.match) {
                m.push_back(formatTextureHash(h));
            }

            j["match"] = m;
        }

        if (wanted(FieldCenter, (p.center[0] != base.center[0]) || (p.center[1] != base.center[1]) || (p.center[2] != base.center[2]))) {
            j["center"] = Json::array({ number(p.center[0]), number(p.center[1]), number(p.center[2]) });
        }

        if (wanted(FieldLights, p.lights != base.lights)) {
            Json l = Json::array();
            for (const auto &light : p.lights) {
                l.push_back(Json::array({ number(light[0]), number(light[1]), number(light[2]), number(light[3]) }));
            }

            j["lights"] = l;
        }

        auto field = [&](const char *key, uint32_t bit, float value, float baseValue) {
            if (wanted(bit, value != baseValue)) {
                j[key] = number(value);
            }
        };

        field("spread", FieldSpread, p.spread, base.spread);
        if (wanted(FieldFills, p.fills != base.fills)) {
            j["fills"] = p.fills;
        }

        field("keyWeight", FieldKeyWeight, p.keyWeight, base.keyWeight);
        field("size", FieldSize, p.size, base.size);
        field("radius", FieldRadius, p.radius, base.radius);
        field("shadow", FieldShadow, p.shadow, base.shadow);
        field("characterShadow", FieldCharacterShadow, p.characterShadow, base.characterShadow);
        field("clearance", FieldClearance, p.clearance, base.clearance);
        field("falloff", FieldFalloff, p.falloff, base.falloff);
        field("outsideDarkness", FieldOutsideDarkness, p.outsideDarkness, base.outsideDarkness);
        field("outsideEdge", FieldOutsideEdge, p.outsideEdge, base.outsideEdge);
        if (wanted(FieldGlows, p.glows != base.glows)) {
            Json g = Json::array();
            for (const auto &glow : p.glows) {
                g.push_back(Json::array({ number(glow[0]), number(glow[1]), number(glow[2]), number(glow[3]), number(glow[4]) }));
            }

            j["glows"] = g;
        }

        if (wanted(FieldAnchor, p.anchorScene != base.anchorScene)) {
            j["anchor"] = p.anchorScene ? "scene" : "world";
        }

        return j;
    }

    std::string writeLightPresets(const LightPresetFile &file) {
        Json root = Json::object();
        if (file.hasDebugMode) {
            root["debugMode"] = file.debugMode;
        }

        root["default"] = writePreset(file.defaultPreset, LightPreset(), false, false, true);
        if (file.roomEnabled) {
            Json room = writePreset(file.roomPreset, file.defaultPreset, false, false, false);
            room.erase("anchor");
            room["maxExtent"] = number(file.roomMaxExtent);
            room["count"] = file.roomCount;
            room["height"] = number(file.roomHeight);
            root["room"] = room;
        }

        Json venues = Json::array();
        for (const LightPreset &v : file.venues) {
            venues.push_back(writePreset(v, file.defaultPreset, true, true, false));
        }

        root["venues"] = venues;
        return "// Ray tracing light presets, written by the RT64 Lighting editor (WCW_INSPECTOR, F1).\n"
            "// Venues inherit every field they don't set from \"default\". Earlier comments are in the .bak copy.\n" +
            root.dump(4) + "\n";
    }

    static void inheritInto(LightPreset &p, const LightPreset &d) {
        const uint32_t s = p.setFields;
        if (!(s & FieldCenter)) { p.center[0] = d.center[0]; p.center[1] = d.center[1]; p.center[2] = d.center[2]; }
        if (!(s & FieldSpread)) { p.spread = d.spread; }
        if (!(s & FieldFills)) { p.fills = d.fills; }
        if (!(s & FieldKeyWeight)) { p.keyWeight = d.keyWeight; }
        if (!(s & FieldLights)) { p.lights = d.lights; }
        if (!(s & FieldSize)) { p.size = d.size; }
        if (!(s & FieldRadius)) { p.radius = d.radius; }
        if (!(s & FieldClearance)) { p.clearance = d.clearance; }
        if (!(s & FieldFalloff)) { p.falloff = d.falloff; }
        if (!(s & FieldGlows)) { p.glows = d.glows; }
        if (!(s & FieldAnchor)) { p.anchorScene = d.anchorScene; }
        if (!(s & FieldShadow)) { p.shadow = d.shadow; }
        if (!(s & FieldCharacterShadow)) { p.characterShadow = d.characterShadow; }
        if (!(s & FieldOutsideDarkness)) { p.outsideDarkness = d.outsideDarkness; }
        if (!(s & FieldOutsideEdge)) { p.outsideEdge = d.outsideEdge; }
    }

    void inheritDefault(LightPresetFile &file) {
        inheritInto(file.roomPreset, file.defaultPreset);
        file.roomPreset.anchorScene = false;
        for (LightPreset &v : file.venues) {
            inheritInto(v, file.defaultPreset);
        }
    }

    void computeUniqueMatches(std::vector<LightPreset> &venues) {
        for (LightPreset &p : venues) {
            p.uniqueMatch.clear();
            for (uint64_t h : p.match) {
                bool shared = false;
                for (const LightPreset &o : venues) {
                    if ((&o != &p) && (std::find(o.match.begin(), o.match.end(), h) != o.match.end())) {
                        shared = true;
                        break;
                    }
                }

                if (!shared) {
                    p.uniqueMatch.push_back(h);
                }
            }
        }
    }

    float venueMatchScore(const LightPreset &venue, const std::function<bool(uint64_t)> &seen) {
        const std::vector<uint64_t> &scored = venue.uniqueMatch.empty() ? venue.match : venue.uniqueMatch;
        if (scored.empty()) {
            return 0.0f;
        }

        uint32_t count = 0;
        for (uint64_t h : scored) {
            count += seen(h) ? 1 : 0;
        }

        return float(count) / float(scored.size());
    }

    int bestVenue(const std::vector<LightPreset> &venues, const std::function<bool(uint64_t)> &seen, float minScore) {
        int best = -1;
        float bestScore = minScore;
        for (size_t v = 0; v < venues.size(); v++) {
            if (venues[v].match.empty()) {
                continue;
            }

            const float score = venueMatchScore(venues[v], seen);
            if (score >= bestScore) {
                bestScore = score;
                best = int(v);
            }
        }

        return best;
    }

    float outsideDarknessFactor(float receiverDistance, float radius, float edge, float darkness, bool character) {
        if (character || (darkness <= 0.0f) || (receiverDistance <= radius)) {
            return 1.0f;
        }

        float t = 1.0f;
        if (edge > 0.0f) {
            t = std::clamp((receiverDistance - radius) / edge, 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
        }

        return 1.0f - std::min(darkness, 1.0f) * t;
    }

    float missFloorDistance(const float origin[3], const float direction[3], float floorY, float centerX, float centerZ) {
        if (direction[1] >= -1e-4f) {
            return 1e9f;
        }

        const float t = (floorY - origin[1]) / direction[1];
        if (t <= 0.0f) {
            return 1e9f;
        }

        const float dx = origin[0] + direction[0] * t - centerX;
        const float dz = origin[2] + direction[2] * t - centerZ;
        return std::sqrt(dx * dx + dz * dz);
    }

    bool MissDarkeningPass::darkenThisPass(uint64_t presentId, uint32_t passIndex, size_t meshCount) {
        if (presentId != present) {
            chosenPass = bestPass;
            present = presentId;
            bestPass = UINT32_MAX;
            bestMeshes = 0;
        }

        if ((bestPass == UINT32_MAX) || (meshCount > bestMeshes)) {
            bestPass = passIndex;
            bestMeshes = meshCount;
        }

        return passIndex == chosenPass;
    }

    std::vector<uint64_t> newVenueMatch(const std::vector<std::pair<uint64_t, uint32_t>> &stableTextures,
        const std::vector<LightPreset> &venues, size_t maxCount)
    {
        std::vector<std::pair<uint64_t, uint32_t>> candidates;
        for (const auto &t : stableTextures) {
            bool listed = false;
            for (const LightPreset &v : venues) {
                if (std::find(v.match.begin(), v.match.end(), t.first) != v.match.end()) {
                    listed = true;
                    break;
                }
            }

            if (!listed) {
                candidates.push_back(t);
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::vector<uint64_t> match;
        for (size_t i = 0; (i < candidates.size()) && (match.size() < maxCount); i++) {
            match.push_back(candidates[i].first);
        }

        return match;
    }
};
