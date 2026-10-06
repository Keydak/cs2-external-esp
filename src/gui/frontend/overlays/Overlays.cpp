#include "Overlays.hpp"
#include "core/engine/world/MapCollision.hpp"

#include "updater/Updater.hpp"
#include "gui/renderer/Renderer.hpp" // Circular dependency
#include "gui/frontend/menu/Menu.hpp" // Circular dependency
#include "assets/fonts/WeaponIcons.h"
#include "gui/frontend/images/Avatars.hpp"
#include "core/features/View.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Subtick.hpp"
#include "core/features/Movement.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/features/VoteEvents.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <optional>
#include <unordered_map>

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

        if (cfg::settings::notifications) {
            RenderNotice();
            RenderEspStatus();
        }

    #ifdef _DEBUG
        RenderDebugWindow();
    #endif

    }
    ImGui::PopFont();

    ImGui::PushFont(this->font_alt);
    {
        RenderSpectatorList();
        RenderKeybinds();
        RenderVotes();
        RenderSpeedChart();
        RenderRadar();
        RenderBomb();

        if (cfg::settings::notifications)
            RenderMapProgress();
    }
    ImGui::PopFont();
}

void Overlays::RenderWatermark() {
    if (!cfg::settings::watermark)
        return;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    auto current = Cache::Current();
    const auto& snapshot = *current;
    auto& globals = snapshot.globals;

    static int margin = 10;
    static int padding = 10;
    std::string watermark_string = "Cs2 External";

    watermark_string += std::format(" | {}fps", (int)io.Framerate);

    if (globals.in_match)
        watermark_string += std::format(" | {}", globals.map_name);

    // Where the time of a frame goes, only for development
#ifdef _DEBUG
    if (cfg::settings::frame_times) {
        auto& t = Renderer::GetFrameTimes();
        watermark_string += std::format("\nesp {:.2f} | overlays {:.2f} | menu {:.2f} | draw {:.2f} | present {:.2f} | window {:.2f} ms",
            t.esp, t.overlays, t.menu, t.draw, t.present, t.window);
    }
#endif

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

    // Fading cards in & out: how fast, eased out, & the alpha of what was drawn since start multiplied
    constexpr float FADE_SPEED = 8.f;

    float EaseOut(float t) {
        return 1.f - (1.f - t) * (1.f - t);
    }

    void FadeSince(ImDrawList* d, int start, float alpha) {
        for (int i = start; i < d->VtxBuffer.Size; i++) {
            auto& color = d->VtxBuffer[i].col;
            auto a = static_cast<ImU32>(((color & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT) * alpha);
            color = (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
        }
    }

    // Each row by its name fading in on its own, the ones gone dropped
    void StepRows(std::unordered_map<std::string, float>& alphas, const std::vector<std::string>& names, float step) {
        for (auto it = alphas.begin(); it != alphas.end();)
            it = std::find(names.begin(), names.end(), it->first) != names.end() ? std::next(it) : alphas.erase(it);
        for (const auto& name : names)
            alphas[name] = std::min(alphas[name] + step, 1.f);
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

inline const Player* FindPlayerByPawnIndex(const std::vector<Player>& players, int index) {
    const Player* found = nullptr;

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

    auto current = Cache::Current();
    const auto& snapshot = *current;
    auto& players = snapshot.players;

    const bool is_menu_open = Renderer::IsOpen();
    const bool detailed = cfg::world::spectators::detailed;
    const bool self_only = cfg::world::spectators::self_only;

    struct Row {
        std::string name;
        uint64_t steam_id;
        const char* mode;
        std::string target;
        bool watching_us;
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

        rows.push_back({
            std::string(player.name, strnlen(player.name, sizeof(player.name))),
            player.steam_id,
            player.observer_services.ToString(),
            watching,
            target && target->localplayer,
        });
    }

    // Fades in when someone starts watching & out after, the last rows shown meanwhile. Each one fades & slides
    // in on its own too
    static float window_alpha = 0.f;
    static std::vector<Row> last_rows;
    static std::unordered_map<std::string, float> row_alpha;

    float step = ImGui::GetIO().DeltaTime * FADE_SPEED;
    bool shown = !rows.empty() || is_menu_open;
    if (shown)
        last_rows = rows;
    else
        rows = last_rows;

    window_alpha = std::clamp(window_alpha + (shown ? step : -step), 0.f, 1.f);
    if (window_alpha <= 0.f) {
        last_rows.clear();
        row_alpha.clear();
        return;
    }

    std::vector<std::string> names;
    for (const auto& row : rows)
        names.push_back(row.name);
    StepRows(row_alpha, names, step);

    // Measure
    const float padding = 12.f;
    const float avatar = 26.f;
    const float row_height = 34.f;
    const float gap = 10.f;
    const float min_width = 250.f;
    const float title_font = 15.f;
    const float name_font = 14.f;
    const float small_font = 12.f;

    auto measure = [&](float size, const char* str) { return this->font_alt->CalcTextSizeA(size, FLT_MAX, 0.f, str); };

    constexpr auto title = "Spectators";
    constexpr auto empty = "No one is watching";
    auto count = std::to_string(rows.size());

    float name_width = 0.f, detail_width = 0.f;
    for (const auto& row : rows) {
        name_width = std::max(name_width, measure(name_font, row.name.c_str()).x);
        if (detailed) {
            auto mode = measure(small_font, row.mode).x;
            auto target = measure(small_font, row.target.c_str()).x;
            detail_width = std::max(detail_width, mode + 12.f + 6.f + target + 12.f);
        }
    }

    auto title_size = measure(title_font, title);
    auto count_size = measure(small_font, count.c_str());

    float content = rows.empty() ? measure(name_font, empty).x : avatar + gap + name_width + (detailed ? gap * 2.f + detail_width : 0.f);
    float header = 14.f + title_size.x + gap + count_size.x + 16.f;

    int lines = std::max<int>(1, static_cast<int>(rows.size()));
    float header_height = title_size.y + 8.f;
    ImVec2 size(
        std::max(min_width, padding * 2.f + std::max(content, header)),
        CARD_STRIP + padding + header_height + 8.f + lines * row_height + padding - 6.f
    );

    auto& pos = cfg::world::spectators::pos;
    bool hovered = is_menu_open && CardHandle("##spectators", pos, size);

    // Draw, dropping in a little while it fades in
    auto d = ImGui::GetBackgroundDrawList();
    int card_start = d->VtxBuffer.Size;
    float card_ease = EaseOut(window_alpha);
    ImVec2 min(floorf(pos.x), floorf(pos.y - (1.f - card_ease) * 8.f));
    ImVec2 max = min + size;

    DrawCard(d, min, max, hovered);

    // Accent wash behind the header
    d->AddRectFilledMultiColor(ImVec2(min.x + 1.f, min.y + CARD_STRIP), ImVec2(max.x - 1.f, min.y + CARD_STRIP + padding + header_height),
        Accent(0.12f), Accent(0.02f), Accent(0.f), Accent(0.f));

    float y = min.y + CARD_STRIP + padding;

    // Eye-like mark, then the title & the count in an accent pill
    ImVec2 mark(min.x + padding + 5.f, y + title_size.y * 0.5f);
    d->AddCircle(mark, 5.f, Accent(), 16, 1.5f);
    d->AddCircleFilled(mark, 2.f, Accent(), 12);
    d->AddText(this->font_alt, title_font, ImVec2(min.x + padding + 16.f, y), IM_COL32(240, 240, 244, 255), title);

    ImVec2 pill_max(max.x - padding, y + title_size.y);
    ImVec2 pill_min(pill_max.x - count_size.x - 16.f, y);
    d->AddRectFilled(pill_min, pill_max, Accent(0.2f), title_size.y * 0.5f);
    d->AddText(this->font_alt, small_font, ImVec2(pill_min.x + 8.f, y + (title_size.y - count_size.y) * 0.5f), Accent(), count.c_str());

    y += header_height;
    d->AddLine(ImVec2(min.x + padding, y), ImVec2(max.x - padding, y), IM_COL32(255, 255, 255, 16));
    y += 8.f;

    if (rows.empty()) {
        auto empty_size = measure(name_font, empty);
        d->AddText(this->font_alt, name_font, ImVec2(min.x + padding, y + (row_height - 6.f - empty_size.y) * 0.5f), IM_COL32(120, 120, 128, 255), empty);
    }

    for (const auto& row : rows) {
        int row_start = d->VtxBuffer.Size;
        float row_ease = EaseOut(row_alpha[row.name]);
        float x = min.x + padding + (1.f - row_ease) * -10.f;
        float center_y = y + (row_height - 6.f) * 0.5f;

        // Row background, lit for the ones watching us
        ImVec2 row_min(min.x + padding - 4.f, y - 2.f);
        ImVec2 row_max(max.x - padding + 4.f, y + row_height - 6.f + 2.f);
        if (row.watching_us)
            d->AddRectFilled(row_min, row_max, Accent(0.08f), 6.f);

        // Profile picture in a circle, the first letter of the name until it is there (bots have none)
        ImVec2 avatar_min(x, center_y - avatar * 0.5f);
        ImVec2 avatar_max(x + avatar, center_y + avatar * 0.5f);
        ImVec2 avatar_center = (avatar_min + avatar_max) * 0.5f;

        if (auto texture = Avatars::Get(row.steam_id)) {
            d->AddImageRounded(texture, avatar_min, avatar_max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, avatar * 0.5f);
        }
        else {
            d->AddCircleFilled(avatar_center, avatar * 0.5f, Accent(0.22f), 24);
            char letter[2] = { row.name.empty() ? '?' : static_cast<char>(toupper(static_cast<unsigned char>(row.name[0]))), 0 };
            auto letter_size = measure(13.f, letter);
            d->AddText(this->font_alt, 13.f, avatar_center - letter_size * 0.5f, Accent(), letter);
        }
        d->AddCircle(avatar_center, avatar * 0.5f + 1.f, row.watching_us ? Accent(0.9f) : IM_COL32(255, 255, 255, 40), 24, 1.5f);

        x += avatar + gap;
        auto name_size = measure(name_font, row.name.c_str());
        d->AddText(this->font_alt, name_font, ImVec2(x, center_y - name_size.y * 0.5f), IM_COL32(232, 232, 238, 255), row.name.c_str());

        if (detailed) {
            // Mode in a quiet pill, then who is watched, ours in the accent
            auto mode_size = measure(small_font, row.mode);
            auto target_size = measure(small_font, row.target.c_str());

            float right = max.x - padding;
            ImVec2 target_pos(right - target_size.x, center_y - target_size.y * 0.5f);
            d->AddText(this->font_alt, small_font, target_pos, row.watching_us ? Accent() : IM_COL32(190, 190, 198, 255), row.target.c_str());

            ImVec2 mode_max(target_pos.x - 6.f, center_y + 9.f);
            ImVec2 mode_min(mode_max.x - mode_size.x - 12.f, center_y - 9.f);
            d->AddRectFilled(mode_min, mode_max, IM_COL32(255, 255, 255, 14), 9.f);
            d->AddText(this->font_alt, small_font, ImVec2(mode_min.x + 6.f, center_y - mode_size.y * 0.5f), IM_COL32(150, 150, 160, 255), row.mode);
        }

        FadeSince(d, row_start, row_ease);
        y += row_height;
    }

    FadeSince(d, card_start, card_ease);
}

void Overlays::RenderKeybinds() {
    if (!cfg::world::keybinds::enabled)
        return;

    const bool is_menu_open = Renderer::IsOpen();

    struct Row {
        const char* name;
        std::string key;
        const char* mode;
        bool on;
    };

    // The features with a key, while on. With the menu open all of them, to see where the window goes
    static constexpr const char* THIRD_PERSON_MODES[] = { "Toggle", "Hold", "Always" };
    bool free_cam = Freecam::GetMode() == Freecam::Mode::Free && !Freecam::IsDeadCamera();
    bool third_person = View::IsThirdPersonOn() && Freecam::GetMode() == Freecam::Mode::Off;

    std::vector<Row> rows;
    if (cfg::view::third_person && (third_person || is_menu_open)) {
        int mode = std::clamp(cfg::view::third_person_mode, 0, 2);
        rows.push_back({ "Third Person", mode == 2 ? std::string("-") : Menu::GetKeyName(cfg::view::third_person_key), THIRD_PERSON_MODES[mode], third_person });
    }
    if (cfg::view::freecam && (free_cam || is_menu_open))
        rows.push_back({ "Free Cam", Menu::GetKeyName(cfg::view::freecam_key), "Toggle", free_cam });

    // Movement, unless picked to be hidden
    namespace kb = cfg::world::keybinds;
    bool bhop = cfg::misc::bhop && Movement::IsPlaying() && (GetAsyncKeyState(VK_SPACE) & 0x8000);
    if (cfg::misc::bhop && Movement::IsAvailable() && !kb::hide_bhop && (bhop || is_menu_open))
        rows.push_back({ "Bunny Hop", Menu::GetKeyName(VK_SPACE), "Hold", bhop });

    bool air_strafe = Subtick::IsAirStrafeOn();
    if (cfg::misc::auto_strafe && Subtick::IsAvailable() && !kb::hide_air_strafe && (air_strafe || is_menu_open)) {
        int mode = std::clamp(cfg::misc::air_strafe_mode, 0, 2);
        rows.push_back({ "Air Strafe", mode == 2 ? std::string("-") : Menu::GetKeyName(cfg::misc::air_strafe_key), THIRD_PERSON_MODES[mode], air_strafe });
    }

    bool jump_bug = Subtick::IsJumpBugOn();
    if (cfg::misc::jump_bug && Subtick::IsAvailable() && !kb::hide_jump_bug && (jump_bug || is_menu_open)) {
        int mode = std::clamp(cfg::misc::jump_bug_mode, 0, 2);
        rows.push_back({ "Jump Bug", mode == 2 ? std::string("-") : Menu::GetKeyName(cfg::misc::jump_bug_key), THIRD_PERSON_MODES[mode], jump_bug });
    }

    // Fades in when a key turns something on & out after, showing the last rows meanwhile. Each row fades in
    // on its own too, sliding in from the left
    static float window_alpha = 0.f;
    static std::vector<Row> last_rows;
    static std::unordered_map<std::string, float> row_alpha;

    float step = ImGui::GetIO().DeltaTime * FADE_SPEED;
    bool shown = !rows.empty() || is_menu_open;
    if (shown)
        last_rows = rows;
    else
        rows = last_rows;

    window_alpha = std::clamp(window_alpha + (shown ? step : -step), 0.f, 1.f);
    if (window_alpha <= 0.f) {
        last_rows.clear();
        row_alpha.clear();
        return;
    }

    std::vector<std::string> names;
    for (const auto& row : rows)
        names.push_back(row.name);
    StepRows(row_alpha, names, step);

    // Measure
    const float padding = 12.f;
    const float row_height = 24.f;
    const float gap = 10.f;
    const float min_width = 200.f;
    const float title_font = 15.f;
    const float name_font = 14.f;
    const float small_font = 12.f;

    auto measure = [&](float size, const char* str) { return this->font_alt->CalcTextSizeA(size, FLT_MAX, 0.f, str); };

    constexpr auto title = "Keybinds";
    constexpr auto empty = "No keybind in use";

    float name_width = 0.f, key_width = 0.f;
    for (const auto& row : rows) {
        name_width = std::max(name_width, measure(name_font, row.name).x);
        key_width = std::max(key_width, measure(small_font, row.mode).x + 12.f + 6.f + measure(small_font, row.key.c_str()).x + 12.f);
    }

    auto title_size = measure(title_font, title);
    float content = rows.empty() ? measure(name_font, empty).x : 14.f + name_width + gap * 2.f + key_width;
    float header_height = title_size.y + 8.f;

    int lines = std::max<int>(1, static_cast<int>(rows.size()));
    ImVec2 size(
        std::max(min_width, padding * 2.f + std::max(content, 16.f + title_size.x)),
        CARD_STRIP + padding + header_height + 8.f + lines * row_height + padding - 6.f
    );

    auto& pos = cfg::world::keybinds::pos;
    bool hovered = is_menu_open && CardHandle("##keybinds", pos, size);

    // Draw, dropping in a little while it fades in
    auto d = ImGui::GetBackgroundDrawList();
    int card_start = d->VtxBuffer.Size;
    float card_ease = EaseOut(window_alpha);
    ImVec2 min(floorf(pos.x), floorf(pos.y - (1.f - card_ease) * 8.f));
    ImVec2 max = min + size;

    DrawCard(d, min, max, hovered);

    d->AddRectFilledMultiColor(ImVec2(min.x + 1.f, min.y + CARD_STRIP), ImVec2(max.x - 1.f, min.y + CARD_STRIP + padding + header_height),
        Accent(0.12f), Accent(0.02f), Accent(0.f), Accent(0.f));

    float y = min.y + CARD_STRIP + padding;

    // Key-like mark, then the title
    ImVec2 mark_min(min.x + padding, y + title_size.y * 0.5f - 5.f);
    d->AddRect(mark_min, mark_min + ImVec2(10.f, 10.f), Accent(), 2.f, 0, 1.5f);
    d->AddText(this->font_alt, title_font, ImVec2(min.x + padding + 16.f, y), IM_COL32(240, 240, 244, 255), title);

    y += header_height;
    d->AddLine(ImVec2(min.x + padding, y), ImVec2(max.x - padding, y), IM_COL32(255, 255, 255, 16));
    y += 8.f;

    if (rows.empty()) {
        auto empty_size = measure(name_font, empty);
        d->AddText(this->font_alt, name_font, ImVec2(min.x + padding, y + (row_height - 6.f - empty_size.y) * 0.5f), IM_COL32(120, 120, 128, 255), empty);
    }

    for (const auto& row : rows) {
        float center_y = y + (row_height - 6.f) * 0.5f;
        int row_start = d->VtxBuffer.Size;
        float row_ease = EaseOut(row_alpha[row.name]);
        float slide = (1.f - row_ease) * -10.f;

        // Dot lit while on, then the name
        d->AddCircleFilled(ImVec2(min.x + padding + 4.f + slide, center_y), 3.5f, row.on ? Accent() : IM_COL32(255, 255, 255, 40), 12);

        auto name_size = measure(name_font, row.name);
        d->AddText(this->font_alt, name_font, ImVec2(min.x + padding + 14.f + slide, center_y - name_size.y * 0.5f),
            row.on ? IM_COL32(232, 232, 238, 255) : IM_COL32(150, 150, 160, 255), row.name);

        // Key on the right in the accent, its mode in a quiet pill before it
        auto key_size = measure(small_font, row.key.c_str());
        auto mode_size = measure(small_font, row.mode);

        float right = max.x - padding;
        ImVec2 key_pos(right - key_size.x, center_y - key_size.y * 0.5f);
        d->AddText(this->font_alt, small_font, key_pos, row.on ? Accent() : IM_COL32(150, 150, 160, 255), row.key.c_str());

        ImVec2 mode_max(key_pos.x - 6.f, center_y + 9.f);
        ImVec2 mode_min(mode_max.x - mode_size.x - 12.f, center_y - 9.f);
        d->AddRectFilled(mode_min, mode_max, IM_COL32(255, 255, 255, 14), 9.f);
        d->AddText(this->font_alt, small_font, ImVec2(mode_min.x + 6.f, center_y - mode_size.y * 0.5f), IM_COL32(150, 150, 160, 255), row.mode);

        FadeSince(d, row_start, row_ease);
        y += row_height;
    }

    FadeSince(d, card_start, card_ease);
}

void Overlays::RenderSpeedChart() {
    if (!cfg::world::velocity::enabled)
        return;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();

    auto current = Cache::Current();
    const auto& snapshot = *current;
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

    auto current = Cache::Current();
    const auto& snapshot = *current;
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

    auto current = Cache::Current();
    const auto& snapshot = *current;
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
    const float padding = 12.f;
    const float gap = 10.f;
    const float badge = 40.f;           // Square with the C4 icon
    const float time_font = 28.f;
    const float label_font = 12.f;
    const float bar_height = 6.f;
    const float min_width = 240.f;

    const ImU32 green = IM_COL32(80, 210, 120, 255);
    const ImU32 red = IM_COL32(235, 70, 70, 255);
    const ImU32 text = IM_COL32(240, 240, 244, 255);
    const ImU32 dim = IM_COL32(140, 140, 150, 255);

    const bool planted = bomb.is_planted;
    const bool show_site = cfg::world::bomb::location;
    const bool show_timer = cfg::world::bomb::timer;
    const bool show_defuse = planted && bomb.defusing;

    float length = planted ? bomb.timer_length : 40.f;
    float left = std::max(planted ? bomb.time_left : length, 0.f);
    bool urgent = planted && left <= 10.f; // No time left to defuse without a kit

    // Tenths once every one of them counts
    auto time_str = left < 10.f ? std::format("{:.1f}", left) : std::format("{:.0f}", ceilf(left));
    const char* site = !planted || bomb.site == BombSite::A ? "A" : "B";
    const char* state_str = !planted ? "C4" : urgent ? "DEFUSE WITH KIT ONLY" : "PLANTED";

    // Being defused: cutters for the icon, kit or not next to it, the time green while the defuse still makes it
    const bool defusing = planted && bomb.defusing;
    const ImU32 defuse_color = bomb.CanDefuse() ? green : red;
    if (defusing)
        state_str = bomb.defuse_kit ? "DEFUSING  -  WITH KIT" : "DEFUSING  -  NO KIT";

    auto measure = [&](float size, const char* str) { return this->font_alt->CalcTextSizeA(size, FLT_MAX, 0.f, str); };

    auto time_size = show_timer ? measure(time_font, time_str.c_str()) : ImVec2();
    auto unit_size = show_timer ? measure(label_font + 2.f, "s") : ImVec2();
    auto state_size = measure(label_font, state_str);
    auto site_size = measure(20.f, site);
    float site_badge = show_site ? std::max(site_size.x + 18.f, 34.f) : 0.f;

    // Defuse row: DEFUSING, kit or not & the time it still takes
    constexpr auto defuse_label = "DEFUSING";
    const char* kit_label = bomb.defuse_kit ? "KIT" : "NO KIT";
    auto defuse_str = std::format("{:.1f}s", std::max(bomb.defuse_left, 0.f));
    auto defuse_size = measure(13.f, defuse_label);
    auto kit_size = measure(label_font, kit_label);
    auto defuse_time_size = measure(13.f, defuse_str.c_str());
    const float pill_padding = 7.f;

    float info_width = std::max(time_size.x + unit_size.x + 3.f, state_size.x);
    float width = std::max(min_width, padding * 2.f + badge + gap + info_width + gap + site_badge);
    float height = CARD_STRIP + padding + badge + padding;

    if (show_timer)
        height += bar_height + 10.f;

    if (show_defuse) {
        float row = defuse_size.x + gap + kit_size.x + pill_padding * 2.f + gap * 2.f + defuse_time_size.x;
        width = std::max(width, padding * 2.f + row);
        height += 12.f + 18.f + 6.f + bar_height;
    }

    if (!d)
        return ImVec2(width, height);

    ImVec2 min(floorf(pos.x), floorf(pos.y));
    ImVec2 max(min.x + width, min.y + height);
    ImU32 highlight = defusing ? defuse_color : urgent ? red : Accent();
    auto tint = [&](float alpha) {
        if (defusing)
            return (defuse_color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha * 255) << IM_COL32_A_SHIFT);
        return urgent ? IM_COL32(235, 70, 70, static_cast<int>(alpha * 255)) : Accent(alpha);
    };

    DrawCard(d, min, max);

    // Soft glow of the state color behind the icon side
    d->AddRectFilledMultiColor(ImVec2(min.x + 1.f, min.y + CARD_STRIP), ImVec2(min.x + width * 0.55f, max.y - 1.f),
        tint(0.10f), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), tint(0.10f));

    float top = min.y + CARD_STRIP + padding;

    // C4 in a rounded square of the state color
    ImVec2 badge_min(min.x + padding, top);
    ImVec2 badge_max(badge_min.x + badge, badge_min.y + badge);
    d->AddRectFilled(badge_min, badge_max, tint(0.16f), 8.f);
    d->AddRect(badge_min, badge_max, tint(0.45f), 8.f);

    const char* icon = defusing ? WeaponIcons::CUTTERS : WeaponIcons::C4;
    auto icon_size = this->font_icons->CalcTextSizeA(22.f, FLT_MAX, 0.f, icon);
    d->AddText(this->font_icons, 22.f, badge_min + (ImVec2(badge, badge) - icon_size) * 0.5f, highlight, icon);

    // Time, big, with the state under it
    float x = badge_max.x + gap;
    if (show_timer) {
        float time_y = top - 3.f;
        d->AddText(this->font_alt, time_font, ImVec2(x, time_y), defusing ? defuse_color : urgent ? red : text, time_str.c_str());
        d->AddText(this->font_alt, label_font + 2.f, ImVec2(x + time_size.x + 3.f, time_y + time_size.y - unit_size.y - 3.f), dim, "s");
    }
    d->AddText(this->font_alt, label_font, ImVec2(x, top + badge - state_size.y), defusing ? defuse_color : urgent ? red : dim, state_str);

    // Site in a pill of the accent on the right
    if (show_site) {
        ImVec2 site_min(max.x - padding - site_badge, top + (badge - 30.f) * 0.5f);
        ImVec2 site_max(max.x - padding, site_min.y + 30.f);
        d->AddRectFilled(site_min, site_max, Accent(0.18f), 8.f);
        d->AddRect(site_min, site_max, Accent(0.55f), 8.f);
        d->AddText(this->font_alt, 20.f, site_min + (site_max - site_min - site_size) * 0.5f, Accent(), site);
    }

    float y = top + badge;

    if (show_timer) {
        float progress = std::clamp(left / length, 0.f, 1.f);
        ImVec2 bar_min(min.x + padding, y + 10.f);
        ImVec2 bar_max(max.x - padding, bar_min.y + bar_height);
        float bar_width = bar_max.x - bar_min.x;

        d->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 18), bar_height * 0.5f);
        if (progress > 0.f) {
            ImVec2 fill_max(bar_min.x + bar_width * progress, bar_max.y);
            d->AddRectFilled(bar_min, fill_max, highlight, bar_height * 0.5f);
            d->AddCircleFilled(ImVec2(fill_max.x, bar_min.y + bar_height * 0.5f), bar_height * 0.5f + 1.5f, text, 12);
        }

        // Last moments a defuse still makes it: 10s without a kit, 5s with one
        for (float mark : { 10.f, 5.f }) {
            if (mark >= length)
                continue;
            float mx = bar_min.x + bar_width * (mark / length);
            d->AddLine(ImVec2(mx, bar_min.y - 2.f), ImVec2(mx, bar_max.y + 2.f), IM_COL32(255, 255, 255, 70), 1.f);
        }

        y = bar_max.y;
    }

    if (show_defuse) {
        // Green while the defuse ends in time, red once it cannot anymore
        ImU32 state = bomb.CanDefuse() ? green : red;

        y += 12.f;
        d->AddLine(ImVec2(min.x + padding, y - 6.f), ImVec2(max.x - padding, y - 6.f), IM_COL32(255, 255, 255, 14));

        float row_y = y + (18.f - defuse_size.y) * 0.5f;
        x = min.x + padding;
        d->AddText(this->font_alt, 13.f, ImVec2(x, row_y), state, defuse_label);
        x += defuse_size.x + gap;

        // Kit pill, filled when there is one
        ImVec2 pill_min(x, y);
        ImVec2 pill_max(x + kit_size.x + pill_padding * 2.f, y + 18.f);
        if (bomb.defuse_kit)
            d->AddRectFilled(pill_min, pill_max, Accent(0.2f), 9.f);
        else
            d->AddRect(pill_min, pill_max, IM_COL32(255, 255, 255, 40), 9.f);
        d->AddText(this->font_alt, label_font, ImVec2(x + pill_padding, y + (18.f - kit_size.y) * 0.5f), bomb.defuse_kit ? Accent() : dim, kit_label);

        d->AddText(this->font_alt, 13.f, ImVec2(max.x - padding - defuse_time_size.x, row_y), state, defuse_str.c_str());

        // Defuse progress, filling up towards done
        float progress = 1.f - std::clamp(bomb.defuse_left / bomb.defuse_length, 0.f, 1.f);
        ImVec2 bar_min(min.x + padding, y + 18.f + 6.f);
        ImVec2 bar_max(max.x - padding, bar_min.y + bar_height);

        d->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 18), bar_height * 0.5f);
        if (progress > 0.f)
            d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * progress, bar_max.y), state, bar_height * 0.5f);
    }

    return ImVec2(width, height);
}

void Overlays::RenderBomb() {
    if (!cfg::world::bomb::location && !cfg::world::bomb::timer)
        return;

    auto& io = ImGui::GetIO();
    auto current = Cache::Current();
    const auto& snapshot = *current;

    auto bomb = snapshot.bomb;  // Counted down below, our own copy
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

namespace {
    // What a vote is about, by the index of its issue on the server (the order Counter-Strike registers them in)
    const char* VoteIssueName(int issue) {
        static const char* names[] = {
            "Kick", "Change Map", "Next Map", "Swap Teams", "Scramble Teams", "Restart Match", "Surrender", "Rematch",
            "Continue", "Pause Match", "Unpause Match", "Load Backup", "End Warmup", "Timeout", "End Timeout",
            "Ready", "Not Ready",
        };
        return issue >= 0 && issue < IM_ARRAYSIZE(names) ? names[issue] : "Vote";
    }

    struct VoteState {
        int issue = -1;     // -1 without a vote
        int team = -1;      // Who votes: -1 everyone, else the team number
        int yes = 0, no = 0, potential = 0;
    };

    // The vote controller entity, looked for by its designer name now & then
    uintptr_t FindVoteController() {
        static uintptr_t controller = 0;
        static std::chrono::steady_clock::time_point next_search{};

        auto p = Engine::GetProcess();
        if (!p)
            return 0;

        auto now = std::chrono::steady_clock::now();
        if (controller && now < next_search)
            return controller;
        next_search = now + std::chrono::seconds(5);
        controller = 0;

        auto entity_list = p->read<uintptr_t>(Engine::GetClient().base + offsets::entityList);
        static bool logged_list = false;
        if (!logged_list) {
            logged_list = true;
            LOGF(VERBOSE, "Vote list: client {:#x}, entity list {:#x}", Engine::GetClient().base, entity_list);
        }
        if (!entity_list)
            return 0;

        // Identities of 0x70 in chunks of 512, the controller is made early
        constexpr size_t IDENTITY_SIZE = 0x70;
        std::vector<uint8_t> buffer(IDENTITY_SIZE * 512);

        int seen = 0;
        for (int chunk = 0; chunk < 64 && !controller; chunk++) {
            auto chunk_address = p->read<uintptr_t>(entity_list + 0x10 + 0x8 * chunk);
            if (!chunk_address || !p->read_raw(chunk_address, buffer.data(), buffer.size()))
                continue;

            for (int i = 0; i < 512; i++) {
                auto identity = buffer.data() + IDENTITY_SIZE * i;
                auto entity = *reinterpret_cast<uintptr_t*>(identity);
                auto class_info = *reinterpret_cast<uintptr_t*>(identity + 0x08);
                auto name_address = *reinterpret_cast<uintptr_t*>(identity + offsets::grenade::m_designerName);
                if (!entity || (!class_info && !name_address))
                    continue;

                seen++;
                char name[32]{};
                if (name_address)
                    p->read_raw(name_address, name, sizeof(name) - 1);

                // Class info -> binding -> class name, for when the designer name is something else
                char class_name[32]{};
                if (class_info) {
                    auto binding = p->read<uintptr_t>(class_info + 0x08);
                    auto class_name_address = binding ? p->read<uintptr_t>(binding + 0x08) : 0;
                    if (class_name_address)
                        p->read_raw(class_name_address, class_name, sizeof(class_name) - 1);
                }

                std::string_view designer(name), klass(class_name);

                if (designer == "vote_controller" || klass == "C_VoteController") {
                    controller = entity;
                    break;
                }
            }
        }

        // Once per change, found or not
        static int logged = -1;
        int state = controller ? 1 : 0;
        if (state != logged) {
            logged = state;
            if (controller)
                LOGF(VERBOSE, "Vote controller at {:#x}", controller);
            else
                LOGF(VERBOSE, "Vote controller not found among {} entities", seen);
        }

        return controller;
    }

    VoteState ReadVote() {
        VoteState state;
        auto p = Engine::GetProcess();
        auto controller = FindVoteController();
        if (!p || !controller)
            return state;

        state.issue = p->read<int32_t>(controller + offsets::votes::m_iActiveIssueIndex);
        state.team = p->read<int32_t>(controller + offsets::votes::m_iOnlyTeamToVote);
        state.yes = p->read<int32_t>(controller + offsets::votes::m_nVoteOptionCount);
        state.no = p->read<int32_t>(controller + offsets::votes::m_nVoteOptionCount + 4);
        state.potential = p->read<int32_t>(controller + offsets::votes::m_nPotentialVotes);

        static std::string last;
        auto raw = std::format("issue {} team {} yes {} no {} of {}", state.issue, state.team, state.yes, state.no, state.potential);
        if (raw != last) {
            last = raw;
            LOGF(VERBOSE, "Vote controller: {}", raw);
        }

        if (state.issue < 0 || state.issue > 64)
            state = {};
        return state;
    }
}

void Overlays::RenderVotes() {
    static int logged_enabled = -1;
    if (logged_enabled != static_cast<int>(cfg::world::votes::enabled)) {
        logged_enabled = cfg::world::votes::enabled;
        LOGF(VERBOSE, "Vote list {}, process {}", logged_enabled ? "on" : "off", Engine::GetProcess() ? "attached" : "not attached");
    }

    if (!cfg::world::votes::enabled)
        return;

    struct Voter {
        std::string name;
        uint64_t steam_id = 0;
        int option = -1;    // 0 yes, 1 no
    };

    struct Entry {
        VoteState vote;
        bool ended = false;
        bool from_events = false;   // Only the events told of it, the controller did not
        std::vector<Voter> voters;

        // The game casts the first ballots itself: yes for who called the vote, no for who it kicks
        std::chrono::steady_clock::time_point first_ballot{};
        std::chrono::steady_clock::time_point started_at{};
        int caller = -1, target = -1;   // Into voters
    };

    static std::optional<Entry> last;   // The vote going on or the last one
    static std::chrono::steady_clock::time_point next_read{}, ended_at{}, last_event{};
    static VoteState current{};

    auto start_entry = [&](const VoteState& vote, bool from_events) {
        last.emplace();
        last->started_at = std::chrono::steady_clock::now();
        last->vote = vote;
        last->from_events = from_events;
    };

    auto end_entry = [&](std::chrono::steady_clock::time_point at) {
        last->ended = true;
        ended_at = at;
        LOGF(VERBOSE, "Vote ended: {} yes, {} no of {}, {} ballots seen", last->vote.yes, last->vote.no, last->vote.potential, last->voters.size());
    };

    // A few times a second is plenty for votes
    auto now = std::chrono::steady_clock::now();
    if (now >= next_read) {
        next_read = now + std::chrono::milliseconds(150);
        auto vote = ReadVote();

        bool started = vote.issue >= 0 && (current.issue < 0 || vote.issue != current.issue || vote.team != current.team);
        bool ended = current.issue >= 0 && vote.issue < 0;

        if (started) {
            // The ballots might have come in just before the controller told of the vote
            if (last && last->from_events && !last->ended) {
                last->vote.issue = vote.issue;
                last->vote.team = vote.team;
                last->from_events = false;
            }
            else
                start_entry(vote, false);
            LOGF(VERBOSE, "Vote started: issue {} ({}), team {}, {} can vote", vote.issue, VoteIssueName(vote.issue), vote.team, vote.potential);
        }

        if (vote.issue >= 0 && last) {
            last->vote.yes = vote.yes;     // Counts as they come in
            last->vote.no = vote.no;
            last->vote.potential = vote.potential;
        }

        if (ended && last && !last->ended)
            end_entry(now);

        // The game keeps showing the result a while longer, the card goes as soon as it is decided: everyone voted,
        // or more than half said yes
        if (last && !last->ended && last->vote.issue >= 0 && last->vote.potential > 0) {
            const auto& counts = last->vote;
            if (counts.yes + counts.no >= counts.potential || counts.yes * 2 > counts.potential)
                end_entry(now);
        }

        // The vote runs 15 seconds (sv_vote_timer_duration), the game hides it then & clears the controller ~5 s later
        constexpr auto VOTE_DURATION = std::chrono::seconds(15);
        if (last && !last->ended && now - last->started_at >= VOTE_DURATION)
            end_entry(now);

        current = vote;

        // Who voted what, from our listener in the game
        for (auto& event : VoteEvents::Take()) {
            last_event = now;

            // Ballots late to a vote we already called decided still belong to it, while the game shows it
            if (!last || (last->ended && current.issue < 0)) {
                VoteState unknown;
                unknown.issue = -2;     // Some vote, the controller did not say which
                start_entry(unknown, true);
            }

            auto& entry = *last;
            if (event.cast) {
                auto name = event.player.empty() ? std::format("Player {}", event.slot + 1) : event.player;
                auto voter = std::find_if(entry.voters.begin(), entry.voters.end(), [&](const Voter& v) { return v.name == name; });
                int index = static_cast<int>(voter - entry.voters.begin());
                if (voter != entry.voters.end())
                    voter->option = event.option;
                else
                    entry.voters.push_back({ name, event.steam_id, event.option });

                // The ballots the game casts come together, right as the vote starts
                if (entry.voters.size() == 1)
                    entry.first_ballot = now;
                if (now - entry.first_ballot < std::chrono::milliseconds(500)) {
                    if (event.option == 0 && entry.caller < 0)
                        entry.caller = index;
                    else if (event.option == 1 && entry.target < 0)
                        entry.target = index;
                }

                if (entry.from_events && entry.vote.team < 0)
                    entry.vote.team = event.team;
            }
            else if (entry.from_events && event.yes >= 0) {
                entry.vote.yes = event.yes;
                entry.vote.no = event.no;
                entry.vote.potential = event.potential;
            }

            // Without the controller, the ballots are the counts
            if (entry.from_events) {
                int yes = 0, no = 0;
                for (const auto& v : entry.voters) {
                    if (v.option == 0)
                        yes++;
                    else if (v.option == 1)
                        no++;
                }
                entry.vote.yes = std::max(entry.vote.yes, yes);
                entry.vote.no = std::max(entry.vote.no, no);
            }
        }

        // A vote only the events told of ends when they stop
        if (last && last->from_events && !last->ended && now - last_event > std::chrono::seconds(20))
            end_entry(now);
    }

    // Shown during a vote & a while after, or with the menu open to place it
    const bool is_menu_open = Renderer::IsOpen();
    bool voting = last && !last->ended;
    bool recent = last && last->ended && now - ended_at < std::chrono::milliseconds(1500);
    bool live = voting || recent;
    bool shown = live || is_menu_open;

    // In quick like the other cards, out slower: it slides up & fades with the vote still on it
    constexpr float LEAVE_SPEED = 2.5f;
    static float window_alpha = 0.f;
    float dt = ImGui::GetIO().DeltaTime;
    window_alpha = std::clamp(window_alpha + (shown ? dt * FADE_SPEED : -dt * LEAVE_SPEED), 0.f, 1.f);
    if (window_alpha <= 0.f)
        return;

    // Our team, to tell the votes of the enemy
    int local_team = 0;
    bool deathmatch = false;
    {
        auto snapshot = Cache::Current();
        deathmatch = snapshot->game.deathmatch;
        for (const auto& player : snapshot->players)
            if (player.localplayer)
                local_team = player.team;
    }

    // In deathmatch everyone is an enemy, whatever team the game put them in
    auto who = [&](int team) {
        return team < 0 ? "Everyone" : !deathmatch && team == local_team ? "Your team" : "Enemy";
    };

    const float padding = 12.f;
    const float title_font = 15.f;
    const float text_font = 14.f;
    const float small_font = 12.f;
    const float width = 280.f;
    const float bar_height = 8.f;
    const float avatar = 16.f;
    const float voter_height = avatar + 5.f;
    constexpr size_t MAX_ROWS = 6;

    const ImU32 yes_color = IM_COL32(96, 200, 120, 255);
    const ImU32 no_color = IM_COL32(225, 90, 90, 255);
    const ImU32 text_color = IM_COL32(232, 232, 238, 255);
    const ImU32 muted_color = IM_COL32(150, 150, 160, 255);

    auto measure = [&](float size, const char* str) { return this->font_alt->CalcTextSizeA(size, FLT_MAX, 0.f, str); };

    // What the card holds: the vote, who called it on whom, the bar, the ballots in a yes & a no column
    // Leaving keeps what was on it: the vote, or the empty card when the menu closes
    static bool had_vote = false;
    if (shown)
        had_vote = live;
    const Entry* entry = last && (live || (!shown && had_vote)) ? &*last : nullptr;
    bool kick = entry && (entry->vote.issue == 0 || entry->vote.issue == -2);
    const Voter* caller = entry && entry->caller >= 0 ? &entry->voters[entry->caller] : nullptr;
    const Voter* target = entry && kick && entry->target >= 0 ? &entry->voters[entry->target] : nullptr;

    std::vector<const Voter*> yes_voters, no_voters;
    if (entry) {
        for (const auto& voter : entry->voters) {
            if (voter.option == 0)
                yes_voters.push_back(&voter);
            else if (voter.option == 1)
                no_voters.push_back(&voter);
        }
    }
    size_t rows = std::min(std::max(yes_voters.size(), no_voters.size()), MAX_ROWS);

    float body = text_font;
    if (entry) {
        if (caller)
            body += 6.f + text_font;
        body += 10.f + bar_height + 6.f + small_font;
        if (rows > 0)
            body += 14.f + rows * voter_height;
    }

    ImVec2 size(width, CARD_STRIP + padding + body + padding);

    auto& pos = cfg::world::votes::pos;
    bool hovered = is_menu_open && CardHandle("##votes", pos, size);

    auto d = ImGui::GetBackgroundDrawList();
    int card_start = d->VtxBuffer.Size;
    float card_ease = EaseOut(window_alpha);
    float rise = shown ? 8.f : 16.f;
    ImVec2 min(floorf(pos.x), floorf(pos.y - (1.f - card_ease) * rise));
    ImVec2 max = min + size;

    DrawCard(d, min, max, hovered);

    float left = min.x + padding, right = max.x - padding;
    float y = min.y + CARD_STRIP + padding;

    if (!entry) {
        d->AddText(this->font_alt, text_font, ImVec2(left, y), IM_COL32(120, 120, 128, 255), "No vote right now");
        FadeSince(d, card_start, card_ease);
        return;
    }

    const auto& vote = entry->vote;

    // "KICK" in the accent, whose vote & whether it still runs on the right
    std::string issue = VoteIssueName(vote.issue);
    std::transform(issue.begin(), issue.end(), issue.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
    d->AddText(this->font_alt, title_font, ImVec2(left, y - 1.f), Accent(), issue.c_str());

    auto state = std::format("{} \xC2\xB7 {}", who(vote.team), entry->ended ? "ended" : "voting");
    auto state_size = measure(small_font, state.c_str());
    d->AddText(this->font_alt, small_font, ImVec2(right - state_size.x, y + 1.f), muted_color, state.c_str());
    y += text_font;

    // "m0nesy kicks Clear", or who called a vote of another kind
    if (caller) {
        y += 6.f;
        float x = left;
        auto put = [&](const char* text, ImU32 color) {
            d->AddText(this->font_alt, text_font, ImVec2(x, y), color, text);
            x += measure(text_font, text).x;
        };

        d->PushClipRect(ImVec2(left, y), ImVec2(right, y + text_font + 2.f), true);
        put(caller->name.c_str(), text_color);
        if (target) {
            put("  kicks  ", muted_color);
            put(target->name.c_str(), no_color);
        }
        else
            put("  called it", muted_color);
        d->PopClipRect();
        y += text_font;
    }

    // Yes from the left, no from the right, of everyone who can vote
    y += 10.f;
    float total = static_cast<float>(std::max(vote.potential, vote.yes + vote.no));
    float bar_w = right - left;
    ImVec2 bar_min(left, y), bar_max(right, y + bar_height);
    d->AddRectFilled(bar_min, bar_max, IM_COL32(255, 255, 255, 18), bar_height * 0.5f);
    if (total > 0.f) {
        if (vote.yes > 0)
            d->AddRectFilled(bar_min, ImVec2(left + bar_w * vote.yes / total, bar_max.y), yes_color, bar_height * 0.5f);
        if (vote.no > 0)
            d->AddRectFilled(ImVec2(right - bar_w * vote.no / total, bar_min.y), bar_max, no_color, bar_height * 0.5f);
    }
    y += bar_height + 6.f;

    auto yes_text = std::format("YES {}", vote.yes);
    auto no_text = std::format("NO {}", vote.no);
    auto of_text = std::format("{} of {} voted", vote.yes + vote.no, vote.potential);
    d->AddText(this->font_alt, small_font, ImVec2(left, y), yes_color, yes_text.c_str());
    auto of_size = measure(small_font, of_text.c_str());
    d->AddText(this->font_alt, small_font, ImVec2((left + right - of_size.x) * 0.5f, y), muted_color, of_text.c_str());
    auto no_size = measure(small_font, no_text.c_str());
    d->AddText(this->font_alt, small_font, ImVec2(right - no_size.x, y), no_color, no_text.c_str());
    y += small_font;

    // The ballots: yes on the left, no on the right
    if (rows > 0) {
        y += 6.f;
        float middle = (left + right) * 0.5f;
        d->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(255, 255, 255, 14));
        d->AddLine(ImVec2(middle, y + 4.f), ImVec2(middle, y + 4.f + rows * voter_height), IM_COL32(255, 255, 255, 14));
        y += 4.f;

        auto draw_voter = [&](const Voter& voter, float x, float column_right, float row_y, ImU32 color) {
            float center_y = row_y + avatar * 0.5f;
            ImVec2 center(x + avatar * 0.5f, center_y);
            if (auto texture = Avatars::Get(voter.steam_id)) {
                d->AddImageRounded(texture, center - ImVec2(avatar, avatar) * 0.5f, center + ImVec2(avatar, avatar) * 0.5f,
                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, avatar * 0.5f);
            }
            else {
                d->AddCircleFilled(center, avatar * 0.5f, Accent(0.22f), 20);
                char letter[2] = { voter.name.empty() ? '?' : static_cast<char>(toupper(static_cast<unsigned char>(voter.name[0]))), 0 };
                auto letter_size = measure(10.f, letter);
                d->AddText(this->font_alt, 10.f, center - letter_size * 0.5f, Accent(), letter);
            }
            d->AddCircle(center, avatar * 0.5f + 1.f, color, 20, 1.5f);

            auto name_size = measure(small_font, voter.name.c_str());
            d->PushClipRect(ImVec2(x + avatar + 6.f, row_y), ImVec2(column_right, row_y + avatar), true);
            d->AddText(this->font_alt, small_font, ImVec2(x + avatar + 6.f, center_y - name_size.y * 0.5f), text_color, voter.name.c_str());
            d->PopClipRect();
        };

        for (size_t i = 0; i < rows; i++) {
            float row_y = y + 4.f + i * voter_height;
            if (i < yes_voters.size())
                draw_voter(*yes_voters[i], left, middle - 6.f, row_y, yes_color);
            if (i < no_voters.size())
                draw_voter(*no_voters[i], middle + 8.f, right, row_y, no_color);
        }
    }

    FadeSince(d, card_start, card_ease);
}
