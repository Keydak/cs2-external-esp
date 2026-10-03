#pragma once

#include "Current.hpp"

using json = nlohmann::json;

class Config {
public:
    ~Config() = default;
    Config(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(const Config&) = delete;
    Config& operator=(Config&&) = delete;

    static bool Read();
    static bool Write();

    // Saved copies in export/configs (everything but the skins) & export/skins (the skin changer), apart so a
    // skin loadout can be shared without the rest. Files dropped in there by hand show up in the list too
    enum class Preset { CONFIG, SKINS };

    static std::filesystem::path PresetFolder(Preset kind);
    static std::vector<std::string> ListPresets(Preset kind);           // Names, newest first

    // All return an empty string when it worked, what went wrong otherwise
    static std::string SavePreset(Preset kind, const std::string& name);   // Export, overwrites one of that name
    static std::string LoadPreset(Preset kind, const std::string& name);   // Import into the current settings
    static std::string RenamePreset(Preset kind, const std::string& from, const std::string& to);
    static std::string DeletePreset(Preset kind, const std::string& name);

    // Name without the characters a file name cannot have, empty when nothing is left
    static std::string CleanPresetName(const std::string& name);

    // Imported on every start, kept in export/defaults.json. Empty name for none
    static std::string GetDefaultPreset(Preset kind);
    static void SetDefaultPreset(Preset kind, const std::string& name);
    static void LoadDefaultPresets();
private:
    Config() {};

    static Config& GetInstance()
    {
        static Config i{};
        return i;
    }

    bool ReadImpl();
    bool WriteImpl();

public:
    static color_t JsonToColor(const json& parent, const std::string& key, const color_t& def);
    static void ColorToJson(json& parent, const std::string& key, const color_t& color);
    static void Vec2ToJson(json& parent, const std::string& key, const Vec2_t& vec);
    static Vec2_t JsonToVec2(const json& parent, const std::string& key, const Vec2_t& def);

private:

    // The two parts of the config, read from & written to json
    static void ApplySettings(json data);
    static void ApplySkins(json skins);
    static json SettingsJson();
    static json SkinsJson();

    static void ReadGroup(const json& from, cfg::esp::group_t& group, const cfg::esp::group_t& def);
    static void WriteGroup(json& to, const cfg::esp::group_t& group);
};