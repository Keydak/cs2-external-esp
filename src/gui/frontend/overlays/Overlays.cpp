#include "Overlays.hpp"
#include "core/engine/world/MapCollision.hpp"

#include "updater/Updater.hpp"
#include "gui/renderer/Renderer.hpp" // Circular dependency
#include "gui/frontend/menu/Menu.hpp" // Circular dependency
#include "assets/fonts/WeaponIcons.h"
#include "core/features/Movement.hpp"

bool Overlays::Init() {
    return GetInstance().InitImpl();
}

void Overlays::Render() {
    return GetInstance().RenderImpl();
}

bool Overlays::InitImpl() {
    auto& io = ImGui::GetIO();

    ImFontConfig cfg{};
    cfg.FontDataOwnedByAtlas = false;

	this->font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 12.0f, &cfg);
	this->font_alt = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 14.0f, &cfg);
    this->font_icons = io.Fonts->AddFontFromMemoryTTF(weapon_icon_font, weapon_icon_font_len, 16.0f,  &cfg);
	
	ImFontConfig merge_icon_cfg{};
	merge_icon_cfg.FontDataOwnedByAtlas = false;
	merge_icon_cfg.MergeMode = true;

	static const ImWchar icon_ranges[] = { 0xE000, 0xE046, 0 };
	io.Fonts->AddFontFromMemoryTTF(weapon_icon_font, weapon_icon_font_len, 12.f, &merge_icon_cfg, icon_ranges);

    // Pre allocate buffer
    this->vel_buffer.resize(static_cast<size_t>(cfg::world::velocity::sample_rate * cfg::world::velocity::sample_length));

    return true;
}

void Overlays::RenderImpl() {
    ImGui::PushFont(this->font);
    {
        RenderWatermark();

        RenderNotice();
        RenderEspStatus();
        RenderSlideIndicator();

    #ifdef _DEBUG
        RenderDebugWindow();
    #endif

    }
    ImGui::PopFont();

    ImGui::PushFont(this->font_alt);
    {
        RenderSpectatorList();
        RenderSpeedChart();
        RenderRadar();
        RenderBomb();
        RenderMapProgress();
    }
    ImGui::PopFont();
}

void Overlays::RenderWatermark() {
    if (!cfg::settings::watermark)
        return;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    auto snapshot = Cache::CopySnapshot();
    auto& globals = snapshot.globals;

    static int margin = 10;
    static int padding = 10;
    std::string watermark_string = "Cs2 External";

    watermark_string += std::format(" | {}fps", (int)io.Framerate);

    if (globals.in_match)
        watermark_string += std::format(" | {}", globals.map_name);

    if (cfg::settings::frame_times) {
        auto& t = Renderer::GetFrameTimes();
        watermark_string += std::format("\nesp {:.2f} | overlays {:.2f} | menu {:.2f} | draw {:.2f} | present {:.2f} | window {:.2f} ms",
            t.esp, t.overlays, t.menu, t.draw, t.present, t.window);
    }

    auto size = ImGui::CalcTextSize(watermark_string.data());

    auto rect_start = ImVec2(io.DisplaySize.x - margin - padding * 2 - size.x, margin);
    auto rect_end = ImVec2(io.DisplaySize.x - margin, margin + size.y + padding);
    auto pos = ImVec2(rect_start.x + padding, rect_start.y + padding * 0.6/* compensate font */);

    d->AddRectFilled(
        rect_start,
        rect_end,
        IM_COL32(0, 0, 0, 200),
        8.f
    );

    d->AddRect(
        rect_start,
        rect_end,
        IM_COL32(100, 100, 100, 200),
        8.f
    );

    d->AddText(
        pos,
        IM_COL32(255, 255, 255, 255),
        watermark_string.data()
    );
}

void Overlays::RenderNotice() {
    static auto status = Updater::GetStatus();

    if (status.notice.empty())
        return;

    if (!Renderer::IsOpen())
        return;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    static int margin = 10;
    static int padding = 10;
    auto menu_pos = Menu::GetPos();
    auto menu_size = Menu::GetSize();

    auto max_width = menu_size.x - padding * 2;
    
    auto size = ImGui::CalcTextSize(status.notice.data(), nullptr, false, max_width);

    auto rect_start = ImVec2(menu_pos.x, menu_pos.y - margin - padding * 2 - size.y);
    auto rect_end = ImVec2(menu_pos.x + menu_size.x, menu_pos.y - margin);
    auto pos = ImVec2(rect_start.x + padding, rect_start.y + padding);

    d->AddRectFilled(
        rect_start,
        rect_end,
        IM_COL32(0, 0, 0, 200),
        10.f
    );

    d->AddRect(
        rect_start,
        rect_end,
        IM_COL32(100, 100, 100, 200),
        10.f
    );

    d->AddText(
        pos - ImVec2(0, padding + padding * 0.5),
        IM_COL32(255, 200, 0, 255),
        "Notice"
    );

    d->AddText(
        this->font,
        this->font->LegacySize,
        pos,
        IM_COL32(255, 255, 255, 255),
        status.notice.data(),
        nullptr, 
        max_width
    );
}

namespace {
    ImU32 Accent(float alpha = 1.f) {
        auto accent = cfg::settings::accent;
        return ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, alpha));
    }

    constexpr float CARD_ROUNDING = 6.f;
    constexpr float CARD_STRIP = 2.f;   // Accent line on top

    // Dark card with the accent on top, its border lights up while it is dragged around
    void DrawCard(ImDrawList* d, ImVec2 min, ImVec2 max, bool hovered = false) {
        d->AddRectFilled(min, max, IM_COL32(13, 13, 15, 235), CARD_ROUNDING);
        d->AddRectFilled(min, ImVec2(max.x, min.y + CARD_STRIP), Accent(), CARD_ROUNDING, ImDrawFlags_RoundCornersTop);
        d->AddRect(min, max, hovered ? Accent(0.6f) : IM_COL32(255, 255, 255, 22), CARD_ROUNDING);
    }

    // Invisible window over a card while the menu is open, so it can be dragged. True while hovered
    bool CardHandle(const char* id, Vec2_t& pos, ImVec2 size) {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));

        bool hovered = false;
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

        if (ImGui::Begin(id, nullptr, flags)) {
            pos = ImGui::GetWindowPos();
            hovered = ImGui::IsWindowHovered();
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
        return hovered;
    }
}

inline Player* FindPlayerByPawnIndex(std::vector<Player>& players, int index) {
    Player* found = nullptr;

    for (auto& p : players) {
        if (p.pawn_controller_addr == index) {
            found = &p;
            break;
        }
    }
    return found;
}

void Overlays::RenderSpectatorList() {
    if (!cfg::world::spectators::enabled)
        return;

    auto snapshot = Cache::CopySnapshot();
    auto& players = snapshot.players;

    const bool is_menu_open = Renderer::IsOpen();
    const bool detailed = cfg::world::spectators::detailed;
    const bool self_only = cfg::world::spectators::self_only;

    struct Row {
        std::string name;
        const char* mode;
        std::string target;
    };

    std::vector<Row> rows;
    for (auto& player : players) {
        if (player.alive || !player.observer_services.target)
            continue;

        auto target = FindPlayerByPawnIndex(players, player.observer_services.target);
        if (self_only && (!target || !target->localplayer))
            continue;

        std::string watching = player.observer_services.mode == ObserverMode::Free ? "No one"
            : !target ? "Bomb"
            : target->localplayer ? "You"
            : std::string(target->name, strnlen(target->name, sizeof(target->name)));

        rows.push_back({ std::string(player.name, strnlen(player.name, sizeof(player.name))), player.observer_services.ToString(), watching });
    }

    if (rows.empty() && !is_menu_open)
        return;

    // Measure
    const float padding = 9.f;
    const float column_gap = 14.f;
    const float row_gap = 4.f;
    const float line = ImGui::GetTextLineHeight();

    constexpr auto title = "Spectators";
    constexpr auto empty = "No spectators";
    auto count = std::to_string(rows.size());

    float name_width = 0.f, mode_width = 0.f, target_width = 0.f;
    for (const auto& row : rows) {
        name_width = std::max(name_width, ImGui::CalcTextSize(row.name.c_str()).x);
        mode_width = std::max(mode_width, ImGui::CalcTextSize(row.mode).x);
        target_width = std::max(target_width, ImGui::CalcTextSize(row.target.c_str()).x);
    }

    float content = rows.empty() ? ImGui::CalcTextSize(empty).x
        : detailed ? name_width + column_gap + mode_width + column_gap + target_width : name_width;
    float header = ImGui::CalcTextSize(title).x + column_gap + ImGui::CalcTextSize(count.c_str()).x + 12.f;

    int lines = std::max<int>(1, static_cast<int>(rows.size()));
    ImVec2 size(
        std::max(160.f, padding * 2.f + std::max(content, header)),
        CARD_STRIP + padding + line + 8.f + lines * (line + row_gap) - row_gap + padding
    );

    auto& pos = cfg::world::spectators::pos;
    bool hovered = is_menu_open && CardHandle("##spectators", pos, size);

    // Draw
    auto d = ImGui::GetBackgroundDrawList();
    ImVec2 min(floorf(pos.x), floorf(pos.y));
    ImVec2 max = min + size;

    DrawCard(d, min, max, hovered);

    float y = min.y + CARD_STRIP + padding;
    d->AddCircleFilled(ImVec2(min.x + padding + 3.f, y + line * 0.5f), 3.f, Accent(), 12);
    d->AddText(ImVec2(min.x + padding + 12.f, y), IM_COL32(240, 240, 240, 255), title);

    // Count in an accent pill on the right
    auto count_size = ImGui::CalcTextSize(count.c_str());
    ImVec2 pill_max(max.x - padding, y + line);
    ImVec2 pill_min(pill_max.x - count_size.x - 10.f, y);
    d->AddRectFilled(pill_min, pill_max, Accent(0.18f), line * 0.5f);
    d->AddText(ImVec2(pill_min.x + 5.f, y), Accent(), count.c_str());

    y += line + 4.f;
    d->AddLine(ImVec2(min.x + padding, y), ImVec2(max.x - padding, y), IM_COL32(255, 255, 255, 18));
    y += 4.f;

    if (rows.empty())
        d->AddText(ImVec2(min.x + padding, y), IM_COL32(120, 120, 128, 255), empty);

    for (const auto& row : rows) {
        float x = min.x + padding;
        d->AddText(ImVec2(x, y), IM_COL32(230, 230, 235, 255), row.name.c_str());

        if (detailed) {
            x += name_width + column_gap;
            d->AddText(ImVec2(x, y), IM_COL32(130, 130, 140, 255), row.mode);

            x += mode_width + column_gap;
            d->AddText(ImVec2(x, y), row.target == "You" ? Accent() : IM_COL32(190, 190, 198, 255), row.target.c_str());
        }

        y += line + row_gap;
    }
}

void Overlays::RenderSpeedChart() {
    if (!cfg::world::velocity::enabled)
        return;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    auto snapshot = Cache::CopySnapshot();
    auto& local = snapshot.local;

    const static float padding = 10.0f;
    const bool is_menu_open = Renderer::IsOpen();

    auto& pos = cfg::world::velocity::pos;
    auto& size = cfg::world::velocity::size;

    int rate = cfg::world::velocity::sample_rate;
    float length = cfg::world::velocity::sample_length;

    static int prev_rate = rate;
    static float prev_length = length;

    float left = pos.x + padding;
    float right = pos.x + size.x - padding;
    float bottom = pos.y + size.y - padding;
    float top = pos.y + padding;

    float width = right - left;
    float height = bottom - top;

    if (!is_menu_open && !local.alive)
        return;

    if (is_menu_open) {
        auto height_padding = 25; // some padding to keep the speed number inside the area
        auto altitude_padding = 10; // so it doesnt go under the titlebar

        ImGui::SetNextWindowBgAlpha(0.1f);
        ImGui::SetNextWindowPos(pos - Vec2_t(0, altitude_padding), ImGuiCond_Once);
        ImGui::SetNextWindowSize(size + Vec2_t(0, height_padding), ImGuiCond_Once);
        if (ImGui::Begin("Velocity Graph", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            pos = ImGui::GetWindowPos() + ImVec2(0, altitude_padding);
            size = ImGui::GetWindowSize() - ImVec2(0, height_padding);
            ImGui::End();
        }
    }

    // Cache menu values and resize when changed
    if (prev_rate != rate || prev_length != length) {
        prev_rate = rate;
        prev_length = length;

        vel_buffer.resize(static_cast<size_t>(rate * length));
    }

    Vec2_t speed_2d(local.vel.x, local.vel.y);
    int speed = floor(speed_2d.len());

    vel_accumulator += io.DeltaTime;
    size_t buff_size = vel_buffer.size();

    std::vector<ImVec2> points;
    points.reserve(buff_size);

    float sample_interval = 1.0f / rate;

    while (vel_accumulator >= sample_interval)
    {
        vel_accumulator -= sample_interval;
        vel_buffer.at(vel_index % buff_size) = speed;
        vel_index = (vel_index + 1) % buff_size;
    }

    int max_speed = 1;
    for (int v : vel_buffer)
        max_speed = std::max(max_speed, v);

    for (size_t i = 0; i < buff_size; ++i) {
        float t = i / float(buff_size - 1);

        float x = left + t * width;

        float normalized =
            vel_buffer[(i + vel_index) % buff_size] / float(max_speed);

        float y = bottom - (normalized * height);

        points.emplace_back(x, y);
    }

    d->AddPolyline(
        points.data(),
        static_cast<int>(points.size()),
        IM_COL32(255, 255, 255, 255),
        ImDrawFlags_None,
        1.0f
    );

    auto center = ImVec2(
        pos.x + size.x / 2,
        pos.y + size.y
    );

    d->AddText(
        center,
        IM_COL32(255, 255, 255, 255),
        std::to_string(speed).c_str());
}

#ifdef _DEBUG
void Overlays::RenderDebugWindow() {
    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    auto snapshot = Cache::CopySnapshot();
    auto& game = snapshot.game;
    auto& bomb = snapshot.bomb;
    auto& globals = snapshot.globals;
    auto& players = snapshot.players;

    static int margin = 10;
    static int padding = 10;
    std::string debug_string = "> Game Debug Window\n";

    debug_string += std::format("Map: {}\n", globals.map_name);
    debug_string += std::format("Max Clients: {}\n", globals.max_clients);
    debug_string += std::format("Cache Refresh: {}ms\n", cfg::dev::cache_refresh_rate);

    if (bomb.is_planted) {
        debug_string += "Bomb:\n";
        debug_string += std::format("- Planted Site: {}\n", bomb.site == BombSite::A ? "A" : "B");
    }

    if (!players.empty())
        debug_string += std::format("Players ({}):\n", players.size());

	for (auto& player : players)
		debug_string += std::format(
			"- [{}] {} {}hp {} {}\n", 
			player.index, player.name, 
			player.health, player.weapon.name,
			player.weapon.icon
		);

    auto size = ImGui::CalcTextSize(debug_string.data());

    d->AddRectFilled(
        ImVec2(10 + margin - padding, io.DisplaySize.y - size.y - 20 - margin - padding),
        ImVec2(10 + size.x + margin + padding, io.DisplaySize.y - 20 - margin + padding),
        IM_COL32(0, 0, 0, 200),
        10.f
    );

    d->AddRect(
        ImVec2(10 + margin - padding, io.DisplaySize.y - size.y - 20 - margin - padding),
        ImVec2(10 + size.x + margin + padding, io.DisplaySize.y - 20 - margin + padding),
        IM_COL32(100, 100, 100, 200),
        10.f
    );

    d->AddText(
        ImVec2(10 + margin, io.DisplaySize.y - size.y - 20 - margin),
        IM_COL32(255, 255, 255, 255),
        debug_string.data()
    );
}
#endif

void Overlays::RenderRadar() {
    if (!cfg::world::radar::enabled || cfg::world::radar::mode != cfg::world::radar::MODE_OVERLAY)
        return;

    auto snapshot = Cache::CopySnapshot();
    auto& local = snapshot.local;
    auto& players = snapshot.players;
    auto& matrix = snapshot.game.view_matrix;

    const bool is_menu_open = Renderer::IsOpen();

    if (!is_menu_open && !local.alive)
        return;

    auto& pos = cfg::world::radar::pos;
    auto& size = cfg::world::radar::size;
    float range = cfg::world::radar::range;

    if (is_menu_open) {
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
        ImGui::SetNextWindowSize(size, ImGuiCond_Once);
        if (ImGui::Begin("Radar", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
            pos = ImGui::GetWindowPos();
            size = ImGui::GetWindowSize();
            ImGui::End();
        }
    }

    auto d = ImGui::GetBackgroundDrawList();

    const float cx = pos.x + size.x * 0.5f;
    const float cy = pos.y + size.y * 0.5f;
    const float rx = size.x * 0.5f;
    const float ry = size.y * 0.5f;
    const float radius = std::min(rx, ry);

    d->AddRectFilled(
        ImVec2(pos.x, pos.y),
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(0, 0, 0, 50),
        6.f
    );

    d->AddRect(
        ImVec2(pos.x, pos.y),
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(80, 80, 80, 100),
        6.f
    );

    d->AddCircle(ImVec2(cx, cy), radius * 0.333f, IM_COL32(50, 50, 50, 120));
    d->AddCircle(ImVec2(cx, cy), radius * 0.666f, IM_COL32(50, 50, 50, 120));
    d->AddLine(ImVec2(pos.x + 4.f, cy), ImVec2(pos.x + size.x - 4.f, cy), IM_COL32(50, 50, 50, 120));
    d->AddLine(ImVec2(cx, pos.y + 4.f), ImVec2(cx, pos.y + size.y - 4.f), IM_COL32(50, 50, 50, 120));

    for (auto& player : players) {
        if (!player.alive)
            continue;

        if (player.localplayer)
            continue;

        Vec3_t delta = player.pos - local.pos;
        float dist = sqrtf(delta.x * delta.x + delta.y * delta.y);

        if (dist > range)
            continue;

        float nx = delta.x / range;
        float ny = delta.y / range;

        float sx, sy;
        if (!cfg::world::radar::no_rotate) {
            float rx = matrix[0][0];
            float ry = matrix[0][1];
            float len = sqrtf(rx * rx + ry * ry);
            if (len > 0.001f) { rx /= len; ry /= len; }
            float fx = -ry;
            float fy =  rx;
            float rad_x = nx * rx + ny * ry;
            float rad_y = nx * fx + ny * fy;
            sx = cx + rad_x * (size.x * 0.5f - 6.f);
            sy = cy - rad_y * (size.y * 0.5f - 6.f);
        } else {
            sx = cx + nx * (size.x * 0.5f - 6.f);
            sy = cy - ny * (size.y * 0.5f - 6.f);
        }

        bool mate = !snapshot.game.IsEnemy(local.team, player.team);
        ImU32 col = mate
            ? IM_COL32(0, 220, 80, 255)
            : IM_COL32(220, 50, 50, 255);

        d->AddCircleFilled(ImVec2(sx, sy), 4.f, col);
        d->AddCircle(ImVec2(sx, sy), 4.f, IM_COL32(0, 0, 0, 180));
    }

    d->AddCircleFilled(ImVec2(cx, cy), 5.f, IM_COL32(100, 180, 255, 255));
    d->AddCircle(ImVec2(cx, cy), 5.f, IM_COL32(0, 0, 0, 180));

    d->AddText(ImVec2(pos.x + 6.f, pos.y + 4.f), IM_COL32(180, 180, 180, 200), "Radar");
}

ImVec2 Overlays::DrawBombCard(ImDrawList* d, ImVec2 pos, const Bomb& bomb) {
    return GetInstance().DrawBombCardImpl(d, pos, bomb);
}

ImVec2 Overlays::DrawBombCardImpl(ImDrawList* d, ImVec2 pos, const Bomb& bomb) {
    const float padding = 8.f;
    const float gap = 8.f;
    const float bar_height = 3.f;
    const float section_gap = 7.f;

    const ImU32 green = IM_COL32(80, 210, 120, 255);
    const ImU32 red = IM_COL32(235, 70, 70, 255);

    ImGui::PushFont(this->font_alt);

    const bool planted = bomb.is_planted;
    const bool show_site = cfg::world::bomb::location;
    const bool show_timer = cfg::world::bomb::timer;
    const bool show_defuse = planted && bomb.defusing;

    float length = planted ? bomb.timer_length : 40.f;
    float left = planted ? bomb.time_left : length;
    bool urgent = planted && left <= 10.f; // No time left to defuse without a kit

    auto site_str = std::format("SITE {}", !planted || bomb.site == BombSite::A ? "A" : "B");
    auto time_str = std::format("{:.0f}s", ceilf(std::max(left, 0.f)));

    auto site_size = show_site ? ImGui::CalcTextSize(site_str.c_str()) : ImVec2();
    auto time_size = show_timer ? ImGui::CalcTextSize(time_str.c_str()) : ImVec2();
    float divider = show_site && show_timer ? 13.f : 0.f;

    ImGui::PushFont(this->font_icons);
    auto icon_size = ImGui::CalcTextSize(WeaponIcons::C4);
    ImGui::PopFont();

    // Defuse row: DEFUSING, kit or not & the time it still takes
    constexpr auto defuse_label = "DEFUSING";
    const char* kit_label = bomb.defuse_kit ? "KIT" : "NO KIT";
    auto defuse_str = std::format("{:.1f}s", std::max(bomb.defuse_left, 0.f));

    auto defuse_size = ImGui::CalcTextSize(defuse_label);
    auto kit_size = ImGui::CalcTextSize(kit_label);
    auto defuse_time_size = ImGui::CalcTextSize(defuse_str.c_str());
    const float pill_padding = 5.f;

    float content_height = std::max({ icon_size.y, site_size.y, time_size.y });
    float line = ImGui::GetTextLineHeight();

    float width = padding * 2.f + icon_size.x + gap + site_size.x + divider + time_size.x;
    float height = CARD_STRIP + padding * 2.f + content_height + (show_timer ? bar_height + 6.f : 0.f);

    if (show_defuse) {
        float row = defuse_size.x + gap + kit_size.x + pill_padding * 2.f + gap * 2.f + defuse_time_size.x;
        width = std::max(width, padding * 2.f + row);
        height += section_gap + line + 5.f + bar_height;
    }

    if (!d) {
        ImGui::PopFont();
        return ImVec2(width, height);
    }

    ImVec2 min(floorf(pos.x), floorf(pos.y));
    ImVec2 max(min.x + width, min.y + height);
    ImU32 highlight = urgent ? red : Accent();

    DrawCard(d, min, max);

    float top = min.y + CARD_STRIP + padding;
    float x = min.x + padding;

    d->AddText(this->font_icons, 16.f, ImVec2(x, top + (content_height - icon_size.y) * 0.5f), highlight, WeaponIcons::C4);
    x += icon_size.x + gap;

    if (show_site) {
        d->AddText(ImVec2(x, top + (content_height - site_size.y) * 0.5f), IM_COL32(240, 240, 240, 255), site_str.c_str());
        x += site_size.x;
    }

    if (divider > 0.f) {
        float middle = x + divider * 0.5f;
        d->AddLine(ImVec2(middle, top + 3.f), ImVec2(middle, top + content_height - 3.f), IM_COL32(255, 255, 255, 40));
        x += divider;
    }

    float y = top + content_height;

    if (show_timer) {
        d->AddText(ImVec2(x, top + (content_height - time_size.y) * 0.5f), highlight, time_str.c_str());

        float progress = std::clamp(left / length, 0.f, 1.f);
        ImVec2 bar_min(min.x + padding, y + 6.f);
        ImVec2 bar_max(max.x - padding, bar_min.y + bar_height);

        d->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 20), bar_height * 0.5f);
        if (progress > 0.f)
            d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * progress, bar_max.y), highlight, bar_height * 0.5f);

        y = bar_max.y;
    }

    if (show_defuse) {
        // Green while the defuse ends in time, red once it cannot anymore
        ImU32 state = bomb.CanDefuse() ? green : red;

        y += section_gap;
        d->AddLine(ImVec2(min.x + padding, y - section_gap * 0.5f), ImVec2(max.x - padding, y - section_gap * 0.5f), IM_COL32(255, 255, 255, 14));

        x = min.x + padding;
        d->AddText(ImVec2(x, y), state, defuse_label);
        x += defuse_size.x + gap;

        // Kit pill, filled when there is one
        ImVec2 pill_min(x, y);
        ImVec2 pill_max(x + kit_size.x + pill_padding * 2.f, y + line);
        if (bomb.defuse_kit)
            d->AddRectFilled(pill_min, pill_max, Accent(0.2f), line * 0.5f);
        else
            d->AddRect(pill_min, pill_max, IM_COL32(255, 255, 255, 40), line * 0.5f);
        d->AddText(ImVec2(x + pill_padding, y), bomb.defuse_kit ? Accent() : IM_COL32(150, 150, 158, 255), kit_label);

        d->AddText(ImVec2(max.x - padding - defuse_time_size.x, y), state, defuse_str.c_str());

        // Defuse progress, filling up towards done
        float progress = 1.f - std::clamp(bomb.defuse_left / bomb.defuse_length, 0.f, 1.f);
        ImVec2 bar_min(min.x + padding, y + line + 5.f);
        ImVec2 bar_max(max.x - padding, bar_min.y + bar_height);

        d->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 20), bar_height * 0.5f);
        if (progress > 0.f)
            d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * progress, bar_max.y), state, bar_height * 0.5f);
    }

    ImGui::PopFont();
    return ImVec2(width, height);
}

void Overlays::RenderBomb() {
    if (!cfg::world::bomb::location && !cfg::world::bomb::timer)
        return;

    auto& io = ImGui::GetIO();
    auto snapshot = Cache::CopySnapshot();

    auto& bomb = snapshot.bomb;
    auto& local = snapshot.local;
    auto& matrix = snapshot.game.view_matrix;

    // Counted down to this frame, not to the last cache refresh
    bomb.Tick();

    const bool is_menu_open = Renderer::IsOpen();

    static int margin = 4;

    auto size = DrawBombCardImpl(nullptr, ImVec2(), bomb);

    bool hovered = is_menu_open && CardHandle("##bomb", cfg::world::bomb::pos, size);

    if (!bomb.is_planted && !is_menu_open)
        return;

    if (bomb.is_planted && !bomb.pos.length() && !is_menu_open)
        return;

    if (!local.alive && !is_menu_open)
        return;

    Vec2_t screen_pos;
    bool on_top = bomb.is_planted ? matrix.wts(bomb.pos, io.DisplaySize, screen_pos) : false;

    auto dist = local.pos.dist_to(bomb.pos);

    if (is_menu_open) on_top = false;
    if (dist > 1500.f) on_top = false;

    ImVec2 render(cfg::world::bomb::pos.x, cfg::world::bomb::pos.y);

    // If we use bomb esp the overlay will be sticky
    if (on_top && bomb.is_planted && !cfg::esp::bomb)
        render = ImVec2(screen_pos.x - (size.x * 0.5f), screen_pos.y + margin);

    auto d = ImGui::GetBackgroundDrawList();
    DrawBombCardImpl(d, render, bomb);

    if (hovered)
        d->AddRect(render, render + size, Accent(0.6f), CARD_ROUNDING);
}

// Loading bar while a map is built from the game files, gone once it is done
void Overlays::RenderMapProgress() {
    auto progress = MapCollision::GetProgress();
    if (progress.map.empty())
        return;

    constexpr float HOLD = 1.2f;    // Seconds it stays full before fading out
    constexpr float FADE = 0.4f;

    float alpha = 1.f;
    if (!progress.active) {
        float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - progress.finished).count();
        float hold = progress.failed ? 3.f : HOLD;

        if (since > hold + FADE)
            return;

        alpha = std::clamp(1.f - (since - hold) / FADE, 0.f, 1.f);
    }

    // Eased towards the real value, the steps are far apart
    static float shown = 0.f;
    static std::string shown_map;
    if (shown_map != progress.map) {
        shown_map = progress.map;
        shown = 0.f;
    }
    shown += (progress.value - shown) * std::min(1.f, ImGui::GetIO().DeltaTime * 10.f);

    auto title = progress.failed
        ? std::format("Could not build the collision of {}", progress.map)
        : progress.active
            ? std::format("Building the collision of {}", progress.map)
            : std::format("Collision of {} is ready", progress.map);
    auto percent = std::format("{:.0f}%", shown * 100.f);

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetForegroundDrawList();

    auto title_size = ImGui::CalcTextSize(title.c_str());
    auto percent_size = ImGui::CalcTextSize(percent.c_str());

    const float padding = 10.f, bar_height = 4.f;
    float width = std::max(260.f, title_size.x + percent_size.x + padding * 3.f);
    float height = padding * 2.f + title_size.y + 8.f + bar_height;

    ImVec2 min(floorf((io.DisplaySize.x - width) * 0.5f), 40.f);
    ImVec2 max(min.x + width, min.y + height);

    auto a = [&](int value) { return static_cast<int>(value * alpha); };

    d->AddRectFilled(min, max, IM_COL32(15, 15, 15, a(225)), 6.f);
    d->AddRect(min, max, IM_COL32(45, 45, 45, a(255)), 6.f);

    d->AddText(ImVec2(min.x + padding, min.y + padding), IM_COL32(240, 240, 240, a(255)), title.c_str());
    d->AddText(ImVec2(max.x - padding - percent_size.x, min.y + padding), IM_COL32(160, 160, 160, a(255)), percent.c_str());

    ImVec2 bar_min(min.x + padding, max.y - padding - bar_height);
    ImVec2 bar_max(max.x - padding, bar_min.y + bar_height);
    ImU32 fill = progress.failed ? IM_COL32(220, 60, 60, a(255)) : IM_COL32(80, 170, 255, a(255));

    d->AddRectFilled(bar_min, bar_max, IM_COL32(40, 40, 40, a(255)), 2.f);
    d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * std::clamp(shown, 0.f, 1.f), bar_max.y), fill, 2.f);
}

void Overlays::RenderEspStatus() {
    if (cfg::enabled || !Renderer::IsOpen())
        return;

    auto d = ImGui::GetBackgroundDrawList();
    auto menu_pos = Menu::GetPos();
    auto menu_size = Menu::GetSize();

    constexpr int margin = 10;
    constexpr int padding = 10;
    constexpr auto message = "Settings can be changed now and will take effect when ESP is enabled.";

    auto max_width = menu_size.x - padding * 2;
    auto size = ImGui::CalcTextSize(message, nullptr, false, max_width);
    auto rect_start = ImVec2(menu_pos.x, menu_pos.y + menu_size.y + margin);
    auto rect_end = ImVec2(menu_pos.x + menu_size.x, rect_start.y + padding * 2 + size.y);
    auto pos = ImVec2(rect_start.x + padding, rect_start.y + padding);

    d->AddRectFilled(rect_start, rect_end, IM_COL32(0, 0, 0, 200), 10.f);
    d->AddRect(rect_start, rect_end, IM_COL32(100, 100, 100, 200), 10.f);
    //d->AddText(pos - ImVec2(0, padding + padding * 0.5f), IM_COL32(255, 200, 0, 255), "ESP disabled");
    d->AddText(this->font, this->font->LegacySize, pos, IM_COL32(255, 255, 255, 255), message, nullptr, max_width);
}

void Overlays::RenderSlideIndicator() {
    // Fades with the slide walk state, under the crosshair
    static float alpha = 0.f;

    bool show = cfg::misc::slide_walk && cfg::misc::slide_walk_indicator && Movement::IsSliding();
    alpha = std::clamp(alpha + ImGui::GetIO().DeltaTime * (show ? 8.f : -8.f), 0.f, 1.f);

    if (alpha <= 0.f)
        return;

    auto d = ImGui::GetBackgroundDrawList();
    auto center = ImGui::GetIO().DisplaySize * 0.5f;

    constexpr auto label = "SLIDE";
    auto text_size = ImGui::CalcTextSize(label);
    auto size = ImVec2(text_size.x + 16.f, text_size.y + 6.f);
    auto min = ImVec2(center.x - size.x * 0.5f, center.y + 36.f + (1.f - alpha) * 6.f);

    auto accent = cfg::settings::accent;
    d->AddRectFilled(min, min + size, ImGui::GetColorU32(IM_COL32(13, 13, 15, 255), 0.85f * alpha), 4.f);
    d->AddRectFilled(min, ImVec2(min.x + 2.f, min.y + size.y), ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, alpha)), 1.f);
    d->AddText(min + ImVec2(9.f, 3.f), ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, alpha)), label);
}
