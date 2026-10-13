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
            { "characterShadow", FieldCharacterShadow }, { "outsideDarkness", FieldOutsideDarkness }, { "outsideEdge", FieldOutsideEdge },
            { "color", FieldColor }, { "keyColor", FieldKeyColor }, { "fillColor", FieldFillColor }, { "litAreas", FieldLitAreas }
        };

        uint32_t fields = 0;
        for (const auto &k : keys) {
            fields |= j.contains(k.first) ? k.second : 0u;
        }

        return fields;
    }

    static bool readColor(const Json &j, const char *key, LightColor &color) {
        if (!j.contains(key) || !j[key].is_array() || (j[key].size() < 3)) {
            return false;
        }

        color = { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>() };
        return true;
    }

    LightColor presetLightColor(const LightPreset &preset, size_t lightIndex) {
        if ((lightIndex < preset.lightOwnColor.size()) && (lightIndex < preset.lightColors.size()) && (preset.lightOwnColor[lightIndex] != 0)) {
            return preset.lightColors[lightIndex];
        }

        return preset.color;
    }

    LightColor presetGlowColor(const LightPreset &preset, size_t glowIndex) {
        return (glowIndex < preset.glowColors.size()) ? preset.glowColors[glowIndex] : WhiteLight;
    }

    float presetGlowFlicker(const LightPreset &preset, size_t glowIndex) {
        return (glowIndex < preset.glowFlicker.size()) ? preset.glowFlicker[glowIndex] : 0.0f;
    }

    static uint32_t flickerHash(uint32_t a, uint32_t b) {
        uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        return h;
    }

    float glowFlicker(double timeSeconds, uint32_t seed) {
        if (timeSeconds < 0.0) {
            timeSeconds = 0.0;
        }

        // 2 s windows; about a third hold an event: one to three quick blinks off (70 ms each, 80 ms apart). The
        // tube is on the rest of the time (the owner wanted mostly on, flickering off occasionally).
        const uint32_t window = uint32_t(timeSeconds / 2.0);
        const uint32_t windowHash = flickerHash(seed, window);
        if ((windowHash % 100u) >= 33u) {
            return 1.0f;
        }

        const double start = double((windowHash >> 8) % 100u) / 100.0 * 1.5;
        const uint32_t blinks = 1u + ((windowHash >> 16) % 3u);
        const double offset = timeSeconds - double(window) * 2.0 - start;
        if (offset < 0.0) {
            return 1.0f;
        }

        const double period = 0.15;
        const uint32_t blink = uint32_t(offset / period);
        if (blink >= blinks) {
            return 1.0f;
        }

        return ((offset - double(blink) * period) < 0.07) ? 0.0f : 1.0f;
    }

    float glowFlickerScale(double timeSeconds, uint32_t seed, float amount) {
        amount = std::clamp(amount, 0.0f, 1.0f);
        return 1.0f - amount * (1.0f - glowFlicker(timeSeconds, seed));
    }

    void syncColorLists(LightPreset &preset) {
        preset.lightColors.resize(preset.lights.size(), preset.color);
        preset.lightOwnColor.resize(preset.lights.size(), 0);
        preset.glowColors.resize(preset.glows.size(), WhiteLight);
        preset.glowFlicker.resize(preset.glows.size(), 0.0f);
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
        // Colors: keyColor / fillColor default to this preset's color when it sets one, else to the base's.
        const bool ownColor = readColor(j, "color", p.color);
        if (!readColor(j, "keyColor", p.keyColor) && ownColor) {
            p.keyColor = p.color;
        }

        if (!readColor(j, "fillColor", p.fillColor) && ownColor) {
            p.fillColor = p.color;
        }

        if (j.contains("glows")) {
            p.glows.clear();
            p.glowColors.clear();
            p.glowFlicker.clear();
            for (const auto &g : j["glows"]) {
                p.glows.push_back({ g[0].get<float>(), g[1].get<float>(), g[2].get<float>(), g[3].get<float>(), g[4].get<float>() });
                p.glowColors.push_back((g.size() >= 8) ? LightColor{ g[5].get<float>(), g[6].get<float>(), g[7].get<float>() } : WhiteLight);
                p.glowFlicker.push_back((g.size() >= 9) ? std::clamp(g[8].get<float>(), 0.0f, 1.0f) : 0.0f);
            }
        }

        p.shadow = j.value("shadow", p.shadow);
        p.characterShadow = j.value("characterShadow", p.characterShadow);
        p.outsideDarkness = std::clamp(j.value("outsideDarkness", p.outsideDarkness), 0.0f, 1.0f);
        p.outsideEdge = std::max(j.value("outsideEdge", p.outsideEdge), 0.0f);
        if (j.contains("litAreas")) {
            p.litAreas.clear();
            for (const auto &a : j["litAreas"]) {
                if ((a.size() >= 4) && (p.litAreas.size() < MaxLitAreas)) {
                    p.litAreas.push_back({ a[0].get<float>(), a[1].get<float>(), std::max(a[2].get<float>(), 0.0f), std::max(a[3].get<float>(), 0.0f) });
                }
            }
        }
        p.anchorScene = (j.value("anchor", std::string(p.anchorScene ? "scene" : "world")) == "scene");
        if (j.contains("lights")) {
            p.lights.clear();
            p.lightColors.clear();
            p.lightOwnColor.clear();
            for (const auto &l : j["lights"]) {
                p.lights.push_back({ l[0].get<float>(), l[1].get<float>(), l[2].get<float>(), (l.size() > 3) ? l[3].get<float>() : 1.0f });
                const bool own = (l.size() >= 7);
                p.lightColors.push_back(own ? LightColor{ l[4].get<float>(), l[5].get<float>(), l[6].get<float>() } : p.color);
                p.lightOwnColor.push_back(own ? 1 : 0);
            }
        }

        syncColorLists(p);
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

        auto color = [&](const char *key, uint32_t bit, const LightColor &value, const LightColor &baseValue) {
            if (wanted(bit, value != baseValue)) {
                j[key] = Json::array({ number(value[0]), number(value[1]), number(value[2]) });
            }
        };

        color("color", FieldColor, p.color, base.color);
        color("keyColor", FieldKeyColor, p.keyColor, base.keyColor);
        color("fillColor", FieldFillColor, p.fillColor, base.fillColor);
        if (wanted(FieldLights, (p.lights != base.lights) || (p.lightOwnColor != base.lightOwnColor) || (p.lightColors != base.lightColors))) {
            Json l = Json::array();
            for (size_t i = 0; i < p.lights.size(); i++) {
                const auto &light = p.lights[i];
                Json entry = Json::array({ number(light[0]), number(light[1]), number(light[2]), number(light[3]) });
                if ((i < p.lightOwnColor.size()) && (p.lightOwnColor[i] != 0)) {
                    const LightColor c = presetLightColor(p, i);
                    entry.push_back(number(c[0]));
                    entry.push_back(number(c[1]));
                    entry.push_back(number(c[2]));
                }

                l.push_back(entry);
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
        if (wanted(FieldLitAreas, p.litAreas != base.litAreas)) {
            Json a = Json::array();
            for (const auto &area : p.litAreas) {
                a.push_back(Json::array({ number(area[0]), number(area[1]), number(area[2]), number(area[3]) }));
            }

            j["litAreas"] = a;
        }
        if (wanted(FieldGlows, (p.glows != base.glows) || (p.glowColors != base.glowColors) || (p.glowFlicker != base.glowFlicker))) {
            Json g = Json::array();
            for (size_t i = 0; i < p.glows.size(); i++) {
                const auto &glow = p.glows[i];
                Json entry = Json::array({ number(glow[0]), number(glow[1]), number(glow[2]), number(glow[3]), number(glow[4]) });
                const LightColor c = presetGlowColor(p, i);
                const float flicker = presetGlowFlicker(p, i);
                if ((c != WhiteLight) || (flicker > 0.0f)) {
                    entry.push_back(number(c[0]));
                    entry.push_back(number(c[1]));
                    entry.push_back(number(c[2]));
                }

                if (flicker > 0.0f) {
                    entry.push_back(number(flicker));
                }

                g.push_back(entry);
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
        if (!(s & FieldColor)) { p.color = d.color; }
        if (!(s & FieldKeyColor)) { p.keyColor = (s & FieldColor) ? p.color : d.keyColor; }
        if (!(s & FieldFillColor)) { p.fillColor = (s & FieldColor) ? p.color : d.fillColor; }
        if (!(s & FieldLights)) { p.lights = d.lights; p.lightColors = d.lightColors; p.lightOwnColor = d.lightOwnColor; }
        if (!(s & FieldSize)) { p.size = d.size; }
        if (!(s & FieldRadius)) { p.radius = d.radius; }
        if (!(s & FieldClearance)) { p.clearance = d.clearance; }
        if (!(s & FieldFalloff)) { p.falloff = d.falloff; }
        if (!(s & FieldGlows)) { p.glows = d.glows; p.glowColors = d.glowColors; p.glowFlicker = d.glowFlicker; }
        if (!(s & FieldAnchor)) { p.anchorScene = d.anchorScene; }
        if (!(s & FieldShadow)) { p.shadow = d.shadow; }
        if (!(s & FieldCharacterShadow)) { p.characterShadow = d.characterShadow; }
        if (!(s & FieldOutsideDarkness)) { p.outsideDarkness = d.outsideDarkness; }
        if (!(s & FieldOutsideEdge)) { p.outsideEdge = d.outsideEdge; }
        if (!(s & FieldLitAreas)) { p.litAreas = d.litAreas; }
        syncColorLists(p);
    }

    void inheritDefault(LightPresetFile &file) {
        // "default"'s own key / fill colors follow its color unless it sets them (an edited color reaches its rig).
        LightPreset &d = file.defaultPreset;
        if (!(d.setFields & FieldKeyColor)) { d.keyColor = d.color; }
        if (!(d.setFields & FieldFillColor)) { d.fillColor = d.color; }
        syncColorLists(d);
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

    VenueSelector::Change VenueSelector::update(const std::vector<LightPreset> &venues, const std::vector<uint64_t> &textures, double seconds) {
        std::vector<uint64_t> unique = textures;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        for (uint64_t h : unique) {
            windowCounts[h]++;
        }

        window.push_back({ seconds, unique });
        while (!window.empty() && (window.front().seconds <= seconds - WindowSeconds)) {
            for (uint64_t h : window.front().textures) {
                auto it = windowCounts.find(h);
                if ((it != windowCounts.end()) && (--it->second == 0)) {
                    windowCounts.erase(it);
                }
            }

            window.pop_front();
        }

        // Frames without world textures (a black frame, a 2D screen) don't count as a cut either way.
        if (!unique.empty()) {
            recentFrames.push_back(std::move(unique));
            if (recentFrames.size() > CutFrames) {
                recentFrames.pop_front();
            }
        }

        auto seenRecently = [this](uint64_t h) {
            for (const std::vector<uint64_t> &f : recentFrames) {
                if (std::binary_search(f.begin(), f.end(), h)) {
                    return true;
                }
            }

            return false;
        };

        // The new venue must show on every recent frame (one stray frame isn't a cut); the old one on none of them.
        auto seenOnEach = [this](uint64_t h) {
            for (const std::vector<uint64_t> &f : recentFrames) {
                if (!std::binary_search(f.begin(), f.end(), h)) {
                    return false;
                }
            }

            return true;
        };

        recentVenue = (recentFrames.size() == CutFrames) ? bestVenue(venues, seenOnEach, MinScore) : -1;
        if (activeVenue >= int(venues.size())) {
            activeVenue = -1;
        }

        if (cutsEnabled && (recentVenue >= 0) && (recentVenue != activeVenue) &&
            ((activeVenue < 0) || (venueMatchScore(venues[activeVenue], seenRecently) <= CutActiveMax)))
        {
            restartWindow(seconds);
            activeVenue = recentVenue;
            candidateVenue = recentVenue;
            return Change::Cut;
        }

        if (cutsEnabled && (activeVenue >= 0) && (recentFrames.size() == CutFrames) && (recentVenue < 0) &&
            (venueMatchScore(venues[activeVenue], seenRecently) == 0.0f))
        {
            const std::pair<uint32_t, uint32_t> fresh = recentNew();
            if ((fresh.first >= NewSceneMin) && (float(fresh.first) >= NewSceneShare * float(fresh.second))) {
                restartWindow(seconds);
                activeVenue = -1;
                candidateVenue = -1;
                return Change::Cut;
            }
        }

        const int best = bestVenue(venues, [this](uint64_t h) { return windowCounts.count(h) > 0; }, MinScore);
        if (best == activeVenue) {
            candidateVenue = best;
            return Change::None;
        }

        if (best != candidateVenue) {
            candidateVenue = best;
            candidateSince = seconds;
        }

        if (seconds - candidateSince >= HoldSeconds) {
            activeVenue = best;
            return Change::Window;
        }

        return Change::None;
    }

    void VenueSelector::restartWindow(double seconds) {
        // The shot before a cut no longer counts: left in the window, its textures would switch back to the old venue
        // once the hold ran out. (Both recent frames are after the cut: they show almost none of the old venue.)
        window.clear();
        windowCounts.clear();
        for (const std::vector<uint64_t> &f : recentFrames) {
            window.push_back({ seconds, f });
            for (uint64_t h : f) {
                windowCounts[h]++;
            }
        }
    }

    std::pair<uint32_t, uint32_t> VenueSelector::recentNew() const {
        if (recentFrames.empty()) {
            return { 0, 0 };
        }

        // Textures on every recent frame; new when the window has them only from those frames.
        uint32_t onEach = 0;
        uint32_t fresh = 0;
        for (uint64_t h : recentFrames.back()) {
            bool everywhere = true;
            for (const std::vector<uint64_t> &f : recentFrames) {
                everywhere = everywhere && std::binary_search(f.begin(), f.end(), h);
            }

            if (!everywhere) {
                continue;
            }

            onEach++;
            auto it = windowCounts.find(h);
            fresh += ((it != windowCounts.end()) && (it->second <= recentFrames.size())) ? 1 : 0;
        }

        return { fresh, onEach };
    }

    std::pair<int, float> VenueSelector::recentLeader(const std::vector<LightPreset> &venues) const {
        std::pair<int, float> leader = { -1, 0.0f };
        for (size_t v = 0; v < venues.size(); v++) {
            const float score = venueMatchScore(venues[v], [this](uint64_t h) {
                for (const std::vector<uint64_t> &f : recentFrames) {
                    if (!std::binary_search(f.begin(), f.end(), h)) {
                        return false;
                    }
                }

                return !recentFrames.empty();
            });

            if ((leader.first < 0) || (score > leader.second)) {
                leader = { int(v), score };
            }
        }

        return leader;
    }

    void VenueSelector::reset() {
        window.clear();
        windowCounts.clear();
        recentFrames.clear();
        activeVenue = -1;
        recentVenue = -1;
        candidateVenue = -1;
        candidateSince = 0.0;
    }

    float outsideDarknessFactor(float receiverDistance, float radius, float edge, float darkness) {
        if ((darkness <= 0.0f) || (receiverDistance <= radius)) {
            return 1.0f;
        }

        float t = 1.0f;
        if (edge > 0.0f) {
            t = std::clamp((receiverDistance - radius) / edge, 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
        }

        return 1.0f - std::min(darkness, 1.0f) * t;
    }

    float darknessRamp(float distance, float radius, float edge) {
        if (distance <= radius) {
            return 0.0f;
        }

        if (edge <= 0.0f) {
            return 1.0f;
        }

        const float t = std::clamp((distance - radius) / edge, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float outsideDarknessAt(float x, float z, float centerX, float centerZ, float radius, float edge, float darkness,
        const std::vector<std::array<float, 4>> &litAreas)
    {
        if (darkness <= 0.0f) {
            return 1.0f;
        }

        float ramp = darknessRamp(std::hypot(x - centerX, z - centerZ), radius, edge);
        for (size_t i = 0; (i < litAreas.size()) && (i < MaxLitAreas); i++) {
            const auto &a = litAreas[i];
            ramp = std::min(ramp, darknessRamp(std::hypot(x - a[0], z - a[1]), a[2], a[3]));
        }

        return 1.0f - std::min(darkness, 1.0f) * ramp;
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

    void MissDarkeningPass::finishFrame() {
        if (bestPass == UINT32_MAX) {
            return;
        }

        if ((chosenPass == UINT32_MAX) || (chosenMeshes == 0)) {
            // Nothing chosen yet, or the chosen pass didn't run: take the busiest one.
            chosenPass = bestPass;
            switchStreak = 0;
            streakPass = UINT32_MAX;
            return;
        }

        if ((bestPass != chosenPass) && (float(bestMeshes) >= float(chosenMeshes) * SwitchRatio)) {
            switchStreak = (streakPass == bestPass) ? (switchStreak + 1) : 1;
            streakPass = bestPass;
            if (switchStreak >= SwitchFrames) {
                chosenPass = bestPass;
                switchStreak = 0;
                streakPass = UINT32_MAX;
            }
        }
        else {
            switchStreak = 0;
            streakPass = UINT32_MAX;
        }
    }

    bool MissDarkening::darkenThisPass(const void *target, uint64_t presentId, size_t meshCount) {
        Target &t = targets[target];
        if (presentId != t.present) {
            t.present = presentId;
            t.darkened = false;
            t.recentMax *= MaxDecay;
        }

        t.recentMax = std::max(t.recentMax, float(meshCount));
        if (t.darkened || (float(meshCount) < ArenaShare * t.recentMax)) {
            return false;
        }

        t.darkened = true;
        return true;
    }

    bool MissDarkeningPass::darkenThisPass(uint64_t presentId, uint32_t passIndex, size_t meshCount) {
        if (presentId != present) {
            if (present != UINT64_MAX) {
                finishFrame();
            }

            present = presentId;
            bestPass = UINT32_MAX;
            bestMeshes = 0;
            chosenMeshes = 0;
        }

        if ((bestPass == UINT32_MAX) || (meshCount > bestMeshes)) {
            bestPass = passIndex;
            bestMeshes = meshCount;
        }

        if (passIndex == chosenPass) {
            chosenMeshes = meshCount;
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
