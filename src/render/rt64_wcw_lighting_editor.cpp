//
// RT64
//

// [wcw rt] Lighting editor: a "Lighting" window in the RT64 inspector (developer mode: WCW_INSPECTOR=1 or F1) for
// rigging the ray tracing lights of each venue while the game runs. Edits apply on the next frame; Save writes
// rt_lights.json (the previous file is kept as .bak). The presets data model and file format are WcwLogic's
// (common/rt64_wcw_venues, unit tested).

#include "rt64_wcw_raytracer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "imgui/imgui.h"

namespace RT64 {
    std::vector<std::pair<uint64_t, uint32_t>> WcwRaytracer::stableTextures() const {
        std::vector<std::pair<uint64_t, uint32_t>> stable;
        for (const auto &it : textureCounts) {
            if (it.second >= (VenueWindow * 9) / 10) {
                auto maxTris = textureMaxTris.find(it.first);
                stable.emplace_back(it.first, (maxTris != textureMaxTris.end()) ? maxTris->second : 0u);
            }
        }

        std::sort(stable.begin(), stable.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return stable;
    }

    bool WcwRaytracer::savePresets() {
        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        if (presetsPath.empty()) {
            editorMessage = "No presets path.";
            return false;
        }

        std::error_code ec;
        if (std::filesystem::exists(presetsPath, ec)) {
            std::filesystem::copy_file(presetsPath, presetsPath + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                editorMessage = "Could not write " + presetsPath + ".bak: " + ec.message();
                return false;
            }
        }

        const std::string text = WcwLogic::writeLightPresets(presetFile);
        {
            std::ofstream file(presetsPath, std::ios::binary | std::ios::trunc);
            if (!file) {
                editorMessage = "Could not write " + presetsPath + ".";
                return false;
            }

            file << text;
        }

        // Don't hot reload what was just written (that would also drop a forced venue).
        const auto stampTime = std::filesystem::last_write_time(presetsPath, ec);
        if (!ec) {
            presetsStamp = int64_t(stampTime.time_since_epoch().count());
        }

        presetsEdited = false;
        editorMessage = "Saved " + presetsPath + " (previous file: .bak).";
        fprintf(stderr, "[wcw rt] lighting editor: saved %s\n", presetsPath.c_str());
        return true;
    }

    int WcwRaytracer::newVenueFromScene(const std::string &name) {
        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        const std::vector<uint64_t> match = WcwLogic::newVenueMatch(stableTextures(), presetFile.venues);
        if (match.empty()) {
            editorMessage = "No world textures unique to this scene (is ray tracing on, and the camera steady for 2 s?).";
            return -1;
        }

        WcwLogic::LightPreset venue = presetFile.defaultPreset;
        venue.setFields = 0;
        venue.name = name.empty() ? ("Venue " + std::to_string(presetFile.venues.size() + 1)) : name;
        venue.match = match;
        presetFile.venues.push_back(venue);
        applyPresetFile();
        venueOverride = int(presetFile.venues.size()) - 1;
        presetsEdited = true;
        editorMessage = "Added \"" + venue.name + "\" with " + std::to_string(match.size()) + " match textures (forced; Save to keep it).";
        return venueOverride;
    }

    // Marks a field as set by the preset when its widget changed.
    static bool edited(bool changed, WcwLogic::LightPreset &p, uint32_t field) {
        if (changed) {
            p.setFields |= field;
        }

        return changed;
    }

    static const char *fieldMark(const WcwLogic::LightPreset &p, uint32_t field, bool isDefault) {
        return (isDefault || (p.setFields & field)) ? "" : " (inherited)";
    }

    void WcwRaytracer::drawLightingEditor() {
        if (!ImGui::Begin("Lighting")) {
            ImGui::End();
            return;
        }

        const std::lock_guard<std::recursive_mutex> lock(presetsMutex);
        if (!active() || !supported) {
            ImGui::TextWrapped("Ray tracing is %s. Turn on Graphics > Ray Tracing (needs D3D12 and DirectX Raytracing) to see the lights.",
                supported ? "off" : "not supported here");
        }

        const char *detected = roomActive ? "room (fallback)" : ((activeVenue >= 0) ? venuePresets[activeVenue].name.c_str() : "default");
        ImGui::Text("Detected venue: %s", detected);
        ImGui::TextDisabled("Presets: %s%s", presetsPath.c_str(), presetsEdited ? "  (unsaved changes)" : "");

        // Which preset is in use and edited: detection, or a forced one.
        std::string current = (venueOverride == -2) ? ("Detected (" + std::string(detected) + ")") :
            ((venueOverride == -1) ? std::string("default") : presetFile.venues[venueOverride].name);
        if (ImGui::BeginCombo("Preset", current.c_str())) {
            if (ImGui::Selectable("Detected", venueOverride == -2)) {
                venueOverride = -2;
            }

            if (ImGui::Selectable("default", venueOverride == -1)) {
                venueOverride = -1;
            }

            for (int v = 0; v < int(presetFile.venues.size()); v++) {
                ImGui::PushID(v);
                if (ImGui::Selectable(presetFile.venues[v].name.c_str(), venueOverride == v)) {
                    venueOverride = v;
                }

                ImGui::PopID();
            }

            ImGui::EndCombo();
        }

        // The preset the sliders edit.
        WcwLogic::LightPreset *preset = nullptr;
        bool isDefault = false;
        bool isRoom = false;
        if (venueOverride >= 0) {
            preset = &presetFile.venues[venueOverride];
        }
        else if ((venueOverride == -2) && (activeVenue >= 0)) {
            preset = &presetFile.venues[activeVenue];
        }
        else if ((venueOverride == -2) && roomActive) {
            preset = &presetFile.roomPreset;
            isRoom = true;
        }
        else {
            preset = &presetFile.defaultPreset;
            isDefault = true;
        }

        ImGui::Separator();
        bool changed = false;
        WcwLogic::LightPreset &p = *preset;
        if (!isDefault && !isRoom) {
            char name[64];
            snprintf(name, sizeof(name), "%s", p.name.c_str());
            if (ImGui::InputText("Name", name, sizeof(name))) {
                p.name = name;
                changed = true;
            }

            ImGui::TextDisabled("%zu match textures (%zu unique to this venue)", p.match.size(), p.uniqueMatch.size());
        }
        else if (isRoom) {
            ImGui::TextWrapped("Room fallback: its lights are generated from the room's bounds (count, height).");
            changed |= ImGui::SliderInt("Count", reinterpret_cast<int *>(&presetFile.roomCount), 1, WCW_RT_MAX_LIGHTS);
            changed |= ImGui::SliderFloat("Height", &presetFile.roomHeight, 0.1f, 1.0f);
            changed |= ImGui::DragFloat("Max extent", &presetFile.roomMaxExtent, 10.0f, 100.0f, 20000.0f);
        }

        // Lights: explicit list, or the generated rig (center + fills).
        if (!isRoom && ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Positions are world units%s.%s", p.anchorScene ? ", relative to the scene's center and floor" : "",
                fieldMark(p, WcwLogic::FieldLights, isDefault));
            if (p.lights.empty()) {
                ImGui::TextWrapped("Generated rig: a key light at the center and fill lights around it.");
                changed |= edited(ImGui::DragFloat3("Center", p.center, 5.0f), p, WcwLogic::FieldCenter);
                changed |= edited(ImGui::DragFloat("Spread", &p.spread, 5.0f, 0.0f, 5000.0f), p, WcwLogic::FieldSpread);
                changed |= edited(ImGui::SliderInt("Fills", reinterpret_cast<int *>(&p.fills), 0, WCW_RT_MAX_LIGHTS - 1), p, WcwLogic::FieldFills);
                changed |= edited(ImGui::SliderFloat("Key weight", &p.keyWeight, 0.0f, 1.0f), p, WcwLogic::FieldKeyWeight);
                if (ImGui::Button("Switch to explicit lights")) {
                    p.lights.push_back({ p.center[0], p.center[1], p.center[2], 2.0f });
                    p.setFields |= WcwLogic::FieldLights;
                    changed = true;
                }
            }
            else {
                int remove = -1;
                for (int i = 0; i < int(p.lights.size()); i++) {
                    ImGui::PushID(i);
                    ImGui::Text("Light %d", i + 1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        remove = i;
                    }

                    changed |= edited(ImGui::DragFloat3("Position", p.lights[i].data(), 5.0f), p, WcwLogic::FieldLights);
                    changed |= edited(ImGui::SliderFloat("Weight", &p.lights[i][3], 0.0f, 4.0f), p, WcwLogic::FieldLights);
                    ImGui::PopID();
                }

                if (remove >= 0) {
                    p.lights.erase(p.lights.begin() + remove);
                    p.setFields |= WcwLogic::FieldLights;
                    changed = true;
                }

                if ((p.lights.size() < WCW_RT_MAX_LIGHTS) && ImGui::Button("Add light")) {
                    const auto &last = p.lights.back();
                    p.lights.push_back({ last[0] + 100.0f, last[1], last[2], 1.0f });
                    p.setFields |= WcwLogic::FieldLights;
                    changed = true;
                }

                ImGui::SameLine();
                changed |= edited(ImGui::DragFloat3("Center", p.center, 5.0f), p, WcwLogic::FieldCenter);
            }
        }

        if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Shadow strength on the world and on the wrestlers; receiver radius = how far from the center shadows land.");
            changed |= edited(ImGui::SliderFloat((std::string("Shadow") + fieldMark(p, WcwLogic::FieldShadow, isDefault)).c_str(), &p.shadow, 0.0f, 1.0f), p, WcwLogic::FieldShadow);
            changed |= edited(ImGui::SliderFloat((std::string("Wrestler shadow") + fieldMark(p, WcwLogic::FieldCharacterShadow, isDefault)).c_str(), &p.characterShadow, 0.0f, 1.0f), p, WcwLogic::FieldCharacterShadow);
            changed |= edited(ImGui::DragFloat((std::string("Light size") + fieldMark(p, WcwLogic::FieldSize, isDefault)).c_str(), &p.size, 0.5f, 0.0f, 500.0f), p, WcwLogic::FieldSize);
            if (!isRoom) {
                changed |= edited(ImGui::DragFloat((std::string("Receiver radius") + fieldMark(p, WcwLogic::FieldRadius, isDefault)).c_str(), &p.radius, 10.0f, 0.0f, 20000.0f), p, WcwLogic::FieldRadius);
            }

            changed |= edited(ImGui::DragFloat((std::string("Clearance") + fieldMark(p, WcwLogic::FieldClearance, isDefault)).c_str(), &p.clearance, 1.0f, 0.0f, 1000.0f), p, WcwLogic::FieldClearance);
            changed |= edited(ImGui::DragFloat((std::string("Falloff") + fieldMark(p, WcwLogic::FieldFalloff, isDefault)).c_str(), &p.falloff, 5.0f, 0.0f, 10000.0f), p, WcwLogic::FieldFalloff);
            if (!isRoom) {
                bool anchor = p.anchorScene;
                if (edited(ImGui::Checkbox((std::string("Anchor to scene") + fieldMark(p, WcwLogic::FieldAnchor, isDefault)).c_str(), &anchor), p, WcwLogic::FieldAnchor)) {
                    p.anchorScene = anchor;
                    changed = true;
                }
            }
        }

        if (!isRoom && ImGui::CollapsingHeader("Glows")) {
            int remove = -1;
            for (int i = 0; i < int(p.glows.size()); i++) {
                ImGui::PushID(1000 + i);
                ImGui::Text("Glow %d", i + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    remove = i;
                }

                changed |= edited(ImGui::DragFloat3("Position", p.glows[i].data(), 5.0f), p, WcwLogic::FieldGlows);
                changed |= edited(ImGui::SliderFloat("Intensity", &p.glows[i][3], 0.0f, 4.0f), p, WcwLogic::FieldGlows);
                changed |= edited(ImGui::DragFloat("Range", &p.glows[i][4], 10.0f, 0.0f, 20000.0f), p, WcwLogic::FieldGlows);
                ImGui::PopID();
            }

            if (remove >= 0) {
                p.glows.erase(p.glows.begin() + remove);
                p.setFields |= WcwLogic::FieldGlows;
                changed = true;
            }

            if ((p.glows.size() < WCW_RT_MAX_GLOWS) && ImGui::Button("Add glow")) {
                p.glows.push_back({ p.center[0], p.center[1], p.center[2], 1.0f, 500.0f });
                p.setFields |= WcwLogic::FieldGlows;
                changed = true;
            }
        }

        if (changed) {
            if (isDefault) {
                WcwLogic::inheritDefault(presetFile);
            }

            applyPresetFile();
            presetsEdited = true;
        }

        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
            static const char *modeNames[] = { "Hit normal", "Hit depth", "Shadows (normal)", "Hit mesh", "Shadow occluder",
                "Hit position", "Shadow distance", "Ambient occlusion", "Light map (light positions)", "Raw AO" };
            int modeIndex = int(std::min<uint32_t>(mode, uint32_t(std::size(modeNames) - 1)));
            if (ImGui::Combo("Debug view", &modeIndex, modeNames, int(std::size(modeNames)))) {
                mode = uint32_t(modeIndex);
            }

            static int split = -1;
            bool splitOn = splitScreen();
            if (ImGui::Checkbox("Split screen (ray tracing on the right half)", &splitOn)) {
                split = splitOn ? 1 : 0;
                setSplitScreenOverride(split);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Save")) {
            savePresets();
        }

        ImGui::SameLine();
        if (ImGui::Button("Reload file")) {
            presetsStamp = INT64_MIN + 2;
            loadPresets(true);
            editorMessage = "Reloaded " + presetsPath + ".";
        }

        ImGui::InputText("##newvenue", editorNewVenueName, sizeof(editorNewVenueName));
        ImGui::SameLine();
        if (ImGui::Button("New venue from current scene")) {
            newVenueFromScene(editorNewVenueName);
        }

        if (!editorMessage.empty()) {
            ImGui::TextWrapped("%s", editorMessage.c_str());
        }

        ImGui::End();
    }
};
