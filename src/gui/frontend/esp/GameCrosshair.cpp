#include "GameCrosshair.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace {
    namespace fs = std::filesystem;

    constexpr auto CHECK_INTERVAL = std::chrono::seconds(2);

    // Steam folder from the registry, empty when Steam is not installed
    fs::path SteamPath() {
        wchar_t path[MAX_PATH]{};
        DWORD size = sizeof(path);

        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
            return {};

        return fs::path(path);
    }

    // Convars of the account that played last
    fs::path NewestConvars() {
        auto userdata = SteamPath() / "userdata";
        std::error_code error;

        fs::path newest;
        fs::file_time_type newest_time{};

        for (const auto& account : fs::directory_iterator(userdata, error)) {
            auto file = account.path() / "730" / "local" / "cfg" / "cs2_user_convars_0_slot0.vcfg";
            auto time = fs::last_write_time(file, error);

            if (!error && (newest.empty() || time > newest_time)) {
                newest = file;
                newest_time = time;
            }
        }

        return newest;
    }

    // "name"		"value" lines of the KeyValues file
    std::unordered_map<std::string, std::string> ReadConvars(const fs::path& file) {
        std::unordered_map<std::string, std::string> convars;
        std::ifstream in(file);
        std::string line;

        while (std::getline(in, line)) {
            size_t q[4];
            size_t at = 0;
            int found = 0;

            for (; found < 4; found++) {
                at = line.find('"', at);
                if (at == std::string::npos)
                    break;
                q[found] = at++;
            }

            if (found == 4)
                convars[line.substr(q[0] + 1, q[1] - q[0] - 1)] = line.substr(q[2] + 1, q[3] - q[2] - 1);
        }

        return convars;
    }

    GameCrosshair Parse(const std::unordered_map<std::string, std::string>& convars) {
        GameCrosshair c;
        c.found = true;

        auto number = [&](const char* name, float fallback) {
            auto it = convars.find(name);
            if (it == convars.end())
                return fallback;

            char* end = nullptr;
            float value = strtof(it->second.c_str(), &end);
            return end != it->second.c_str() ? value : fallback;
        };

        auto boolean = [&](const char* name, bool fallback) {
            auto it = convars.find(name);
            if (it == convars.end())
                return fallback;
            return it->second == "true" || it->second == "1";
        };

        c.size = number("cl_crosshairsize", 5.f);
        c.thickness = number("cl_crosshairthickness", 0.5f);
        c.gap = number("cl_crosshairgap", 1.f);
        c.outline_thickness = number("cl_crosshair_outlinethickness", 1.f);
        c.dot = boolean("cl_crosshairdot", false);
        c.t_style = boolean("cl_crosshair_t", false);
        c.outline = boolean("cl_crosshair_drawoutline", false);

        // The game still draws with the settings above, files can have newer ones in pixels next to them that
        // are not used (length 8 while the crosshair is a dot). Only taken when the ones above are missing
        if (!convars.contains("cl_crosshairsize") && convars.contains("cl_crosshair_length")) {
            c.pixels = true;
            c.pixel_length = number("cl_crosshair_length", 8.f);
            c.pixel_gap = number("cl_crosshair_gap", 4.f);
            c.pixel_thickness = number("cl_crosshair_thickness", 2.f);
            c.pixel_outline = number("cl_crosshair_outline_thickness", 1.f);
            c.pixel_screen_height = std::max(1.f, number("cl_crosshair_screen_height", 1080.f));
        }

        // Preset colors of the game, 5 is the own color. Newer settings have no preset, only the own color
        bool own_color = convars.contains("cl_crosshaircolor_r");
        int preset = static_cast<int>(number("cl_crosshaircolor", own_color ? 5.f : 1.f));
        int r = 50, g = 250, b = 50;

        switch (preset) {
        case 0: r = 250; g = 50;  b = 50;  break;
        case 1: r = 50;  g = 250; b = 50;  break;
        case 2: r = 250; g = 250; b = 50;  break;
        case 3: r = 50;  g = 50;  b = 250; break;
        case 4: r = 50;  g = 250; b = 250; break;
        default:
            r = static_cast<int>(number("cl_crosshaircolor_r", 50.f));
            g = static_cast<int>(number("cl_crosshaircolor_g", 250.f));
            b = static_cast<int>(number("cl_crosshaircolor_b", 50.f));
            break;
        }

        // cl_crosshairalpha like the sizes, the alpha of the newer color only without it
        int alpha = !convars.contains("cl_crosshairalpha") && convars.contains("cl_crosshaircolor_a")
            ? static_cast<int>(number("cl_crosshaircolor_a", 255.f))
            : boolean("cl_crosshairusealpha", true) ? static_cast<int>(number("cl_crosshairalpha", 200.f)) : 255;
        alpha = std::clamp(alpha, 0, 255);

        int outline_alpha = convars.contains("cl_crosshairoutline_a")
            ? std::clamp(static_cast<int>(number("cl_crosshairoutline_a", 255.f)), 0, 255)
            : alpha;

        c.color = IM_COL32(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), alpha);
        c.outline_color = IM_COL32(
            std::clamp(static_cast<int>(number("cl_crosshairoutline_r", 0.f)), 0, 255),
            std::clamp(static_cast<int>(number("cl_crosshairoutline_g", 0.f)), 0, 255),
            std::clamp(static_cast<int>(number("cl_crosshairoutline_b", 0.f)), 0, 255),
            outline_alpha);

        return c;
    }
}

const GameCrosshair& GameCrosshair::Get() {
    static GameCrosshair current;
    static fs::path file;
    static fs::file_time_type file_time{};
    static std::chrono::steady_clock::time_point checked{};

    auto now = std::chrono::steady_clock::now();
    if (checked != std::chrono::steady_clock::time_point{} && now - checked < CHECK_INTERVAL)
        return current;
    checked = now;

    // Read again only when another account played or the game saved the file
    auto newest = NewestConvars();
    std::error_code error;
    auto time = newest.empty() ? fs::file_time_type{} : fs::last_write_time(newest, error);

    if (newest.empty()) {
        current = GameCrosshair{};
        file.clear();
    }
    else if (newest != file || time != file_time) {
        current = Parse(ReadConvars(newest));
        file = newest;
        file_time = time;
    }

    return current;
}

void GameCrosshair::Draw(ImDrawList* d, ImVec2 center, float screen_height) const {
    int length, thick, distance, border;

    if (pixels) {
        // Pixels on a screen of the saved height
        float scale = screen_height / pixel_screen_height;
        length = static_cast<int>(roundf(pixel_length * scale));
        thick = std::max(1, static_cast<int>(roundf(pixel_thickness * scale)));
        distance = std::max(0, static_cast<int>(roundf(pixel_gap * scale)));
        border = std::max(1, static_cast<int>(roundf(pixel_outline * scale)));
    }
    else {
        // Sizes of the game are for a 480 pixel tall screen
        float scale = screen_height / 480.f;
        length = static_cast<int>(roundf(size * scale));
        thick = std::max(1, static_cast<int>(roundf(thickness * scale)));
        distance = static_cast<int>(ceilf(4.f * screen_height / 1200.f + gap));
        border = std::max(1, static_cast<int>(roundf(outline_thickness)));
    }

    int cx = static_cast<int>(center.x), cy = static_cast<int>(center.y);

    struct Bar { int x0, y0, x1, y1; };
    Bar bars[5];
    int count = 0;

    if (length > 0) {
        int inner_left = cx - distance - thick / 2;
        int inner_right = inner_left + 2 * distance + thick;
        int inner_top = cy - distance - thick / 2;
        int inner_bottom = inner_top + 2 * distance + thick;

        int y0 = cy - thick / 2, x0 = cx - thick / 2;

        bars[count++] = { inner_left - length, y0, inner_left, y0 + thick };      // Left
        bars[count++] = { inner_right, y0, inner_right + length, y0 + thick };    // Right
        bars[count++] = { x0, inner_bottom, x0 + thick, inner_bottom + length };  // Bottom
        if (!t_style)
            bars[count++] = { x0, inner_top - length, x0 + thick, inner_top };    // Top
    }

    if (dot) {
        int x0 = cx - thick / 2, y0 = cy - thick / 2;
        bars[count++] = { x0, y0, x0 + thick, y0 + thick };
    }

    if (outline) {
        for (int i = 0; i < count; i++) {
            const auto& b = bars[i];
            d->AddRectFilled(ImVec2(float(b.x0 - border), float(b.y0 - border)), ImVec2(float(b.x1 + border), float(b.y1 + border)), outline_color);
        }
    }

    for (int i = 0; i < count; i++) {
        const auto& b = bars[i];
        d->AddRectFilled(ImVec2(float(b.x0), float(b.y0)), ImVec2(float(b.x1), float(b.y1)), color);
    }
}
