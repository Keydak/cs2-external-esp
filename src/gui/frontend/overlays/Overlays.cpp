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
#include "core/features/HitEffects.hpp"
#include "core/features/KillEffect.hpp"

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
        RenderSelfAura();
        RenderStrikes();
        RenderHitmarkers();
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

    // Fades & drops in like the other cards, out with the last bomb still on it (defused, exploded, we died)
    bool shown = is_menu_open || (bomb.is_planted && bomb.pos.length() && local.alive);

    static float window_alpha = 0.f;
    static Bomb last_bomb;
    static ImVec2 last_render;
    static bool has_last = false;

    float step = io.DeltaTime * FADE_SPEED;
    window_alpha = std::clamp(window_alpha + (shown ? step : -step), 0.f, 1.f);
    if (window_alpha <= 0.f || (!shown && !has_last)) {
        has_last = false;
        return;
    }

    ImVec2 render = last_render;

    if (shown) {
        Vec2_t screen_pos;
        bool on_top = bomb.is_planted ? matrix.wts(bomb.pos, io.DisplaySize, screen_pos) : false;

        auto dist = local.pos.dist_to(bomb.pos);

        if (is_menu_open) on_top = false;
        if (dist > 1500.f) on_top = false;

        render = ImVec2(cfg::world::bomb::pos.x, cfg::world::bomb::pos.y);

        // If we use bomb esp the overlay will be sticky
        if (on_top && bomb.is_planted && !cfg::esp::bomb)
            render = ImVec2(screen_pos.x - (size.x * 0.5f), screen_pos.y + margin);

        last_bomb = bomb;
        last_render = render;
        has_last = true;
    }
    else {
        bomb = last_bomb;
    }

    auto d = ImGui::GetBackgroundDrawList();
    int card_start = d->VtxBuffer.Size;
    float card_ease = EaseOut(window_alpha);
    render.y = floorf(render.y - (1.f - card_ease) * 8.f);

    DrawBombCardImpl(d, render, bomb);

    if (hovered)
        d->AddRect(render, render + size, Accent(0.6f), CARD_ROUNDING);

    FadeSince(d, card_start, card_ease);
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

        // Still the entity it was: after a map change its memory can belong to something else, read as a vote
        if (controller) {
            auto identity = p->read<uintptr_t>(controller + 0x10);
            if (!identity || p->read<uintptr_t>(identity) != controller) {
                LOGF(VERBOSE, "Vote controller at {:#x} is gone", controller);
                controller = 0;
                next_search = {};
            }
        }

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

        // A vote always has voters: a controller not filled yet (joining a server) reads issue 0 with none
        // Counts out of what a server can hold are not a vote either: memory of something else
        bool sane = state.issue >= 0 && state.issue <= 64 && state.potential > 0 && state.potential <= 64
            && (state.team == -1 || state.team == 2 || state.team == 3)
            && state.yes >= 0 && state.no >= 0 && state.yes + state.no <= state.potential;
        if (!sane)
            state = {};
        return state;
    }
}

// An X of four short lines around a point, darker lines under them so they show on anything
static void DrawHitmarker(ImDrawList* d, ImVec2 center, float gap, float length, ImU32 color, float alpha) {
    const ImVec2 dirs[] = { { -1.f, -1.f }, { 1.f, -1.f }, { -1.f, 1.f }, { 1.f, 1.f } };
    const float k = 0.7071f;

    for (const auto& dir : dirs) {
        auto from = ImVec2(center.x + dir.x * gap * k, center.y + dir.y * gap * k);
        auto to = ImVec2(center.x + dir.x * (gap + length) * k, center.y + dir.y * (gap + length) * k);
        d->AddLine(from, to, IM_COL32(0, 0, 0, static_cast<int>(150 * alpha)), 3.5f);
    }

    for (const auto& dir : dirs) {
        auto from = ImVec2(center.x + dir.x * gap * k, center.y + dir.y * gap * k);
        auto to = ImVec2(center.x + dir.x * (gap + length) * k, center.y + dir.y * (gap + length) * k);
        d->AddLine(from, to, color, 1.6f);
    }
}

// Our hits, at the crosshair & where on the player they landed. Where exactly the server does not tell, so the
// bone of the player hit that was nearest to the crosshair at that moment
void Overlays::RenderHitmarkers() {
    namespace hm = cfg::world::hitmarker;

    struct Mark {
        HitEvent event;
        bool placed = false;
        Vec3_t pos{};
    };

    static std::deque<Mark> marks;

    auto events = HitEffects::Drain();
    if (!hm::crosshair && !hm::world) {
        marks.clear();
        return;
    }

    auto& io = ImGui::GetIO();
    auto current = Cache::Current();
    const auto& snapshot = *current;
    const auto& matrix = snapshot.game.view_matrix;
    auto center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    for (const auto& event : events) {
        Mark mark{ event };

        for (const auto& player : snapshot.players) {
            if (player.index != event.victim || player.bone_list.empty())
                continue;

            static const int bones[] = {
                head, neck, chest, spine_2, spine_1, pelvis,
                shoulder_L, elbow_L, hand_L, shoulder_R, elbow_R, hand_R,
                hip_L, knee_L, foot_heel_L, hip_R, knee_R, foot_heel_R,
            };

            float best = FLT_MAX;
            for (int bone : bones) {
                if (bone >= static_cast<int>(player.bone_list.size()))
                    continue;

                const auto& pos = player.bone_list[bone].pos;
                Vec2_t screen;
                if (!matrix.wts(pos, io.DisplaySize, screen, false))
                    continue;

                float dx = screen.x - center.x, dy = screen.y - center.y;
                float distance = dx * dx + dy * dy;
                if (distance < best) {
                    best = distance;
                    mark.pos = pos;
                    mark.placed = true;
                }
            }
            break;
        }

        marks.push_back(mark);
        if (marks.size() > 24)
            marks.pop_front();
    }

    auto d = ImGui::GetBackgroundDrawList();
    auto now = std::chrono::steady_clock::now();
    float duration = std::max(0.1f, hm::duration);

    while (!marks.empty() && std::chrono::duration<float>(now - marks.front().event.time).count() > duration)
        marks.pop_front();

    bool crosshair_drawn = false;

    // Newest first, the crosshair one only for the newest
    for (auto it = marks.rbegin(); it != marks.rend(); ++it) {
        const auto& mark = *it;
        float t = std::clamp(std::chrono::duration<float>(now - mark.event.time).count() / duration, 0.f, 1.f);
        float alpha = 1.f - t * t;

        const auto& base = mark.event.kill ? hm::kill_color : hm::color;
        auto color = ImGui::GetColorU32(ImVec4(base.r, base.g, base.b, base.a * alpha));

        // Pops out a little at first
        float pop = 1.f + 0.35f * std::max(0.f, 1.f - t * 6.f);
        float length = hm::size * pop * (mark.event.kill ? 1.25f : 1.f);

        if (hm::crosshair && !crosshair_drawn) {
            crosshair_drawn = true;
            DrawHitmarker(d, center, 4.f, length, color, alpha);
        }

        if (hm::world && mark.placed) {
            Vec2_t screen;
            if (!matrix.wts(mark.pos, io.DisplaySize, screen))
                continue;

            auto at = ImVec2(screen.x, screen.y);
            DrawHitmarker(d, at, 2.f, length * 0.8f, color, alpha);

            if (hm::damage && mark.event.damage > 0) {
                auto text = std::to_string(mark.event.damage);
                auto size = ImGui::CalcTextSize(text.c_str());
                auto pos = ImVec2(at.x - size.x * 0.5f, at.y - length - size.y - 4.f - t * 18.f);

                d->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), text.c_str());
                d->AddText(pos, color, text.c_str());
            }
        }
    }
}

namespace {
    // Same numbers for the same seed: a bolt keeps its shape until it is told to change
    struct Random {
        uint32_t state;
        float next() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return (state & 0xFFFFFF) / 16777215.f;
        }
        float signed_next() { return next() * 2.f - 1.f; }
    };

    // A jagged line between two points of the world, each piece moved aside up to roughness
    std::vector<Vec3_t> Bolt(const Vec3_t& from, const Vec3_t& to, int pieces, float roughness, Random& random) {
        std::vector<Vec3_t> points{ from };
        for (int k = 1; k < pieces; k++) {
            float t = static_cast<float>(k) / pieces;
            auto at = from + (to - from) * t;
            float aside = roughness * (0.4f + 0.6f * std::sin(t * 3.14159f));
            points.push_back(at + Vec3_t(random.signed_next() * aside, random.signed_next() * aside, random.signed_next() * aside * 0.3f));
        }
        points.push_back(to);
        return points;
    }

    // Singularity: a black hole opens in the middle of the body, the body (glowing points where its bones were) spirals
    // into it faster & faster, lines around bend into it, then it collapses to a point & bursts
    void DrawSingularity(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const view_matrix_t& matrix,
        ImVec2 display) {
        constexpr float OPEN = 0.25f;           // Seconds it takes to open
        constexpr float PULL_FROM = 0.2f;       // The body starts going in
        constexpr float PULL_TIME = 0.95f;      // Each point of the body on its way in
        constexpr float COLLAPSE = 1.55f;       // It closes from here
        constexpr float COLLAPSE_TIME = 0.2f;
        constexpr float BURST_TIME = 0.35f;     // The burst after it closed
        constexpr float RADIUS = 26.f;          // Of the hole, in the world
        constexpr int POINTS = 140;             // Of the body

        auto project = [&](const Vec3_t& world, ImVec2& out) {
            Vec2_t screen;
            if (!matrix.wts(world, Vec2_t(display.x, display.y), screen, false))
                return false;
            out = ImVec2(screen.x, screen.y);
            return true;
        };
        auto color = [&](int r, int g, int b, float a) {
            return IM_COL32(r, g, b, static_cast<int>(255.f * std::clamp(a * dim, 0.f, 1.f)));
        };

        ImVec2 center;
        if (!project(strike.center, center))
            return;
        // The size of a world unit on the screen there
        ImVec2 up;
        if (!project(strike.center + Vec3_t(0.f, 0.f, RADIUS), up))
            return;
        float unit_radius = std::max(2.f, std::fabs(center.y - up.y));

        // Size of the hole: opens, holds (breathing a little), collapses
        float open = std::clamp(age / OPEN, 0.f, 1.f);
        open = 1.f - (1.f - open) * (1.f - open) * (1.f - open);
        float closing = std::clamp((age - COLLAPSE) / COLLAPSE_TIME, 0.f, 1.f);
        float size = open * (1.f - closing * closing) * (1.f + 0.06f * std::sin(age * 18.f));
        float radius = unit_radius * size;
        float spin = age * 5.f;

        Random random{ strike.seed ? strike.seed : 1u };

        // Lines of the space around bending into it
        if (age < COLLAPSE + COLLAPSE_TIME) {
            float strength = open * (1.f - closing);
            for (int line = 0; line < 16; line++) {
                float start_angle = random.next() * 6.2831853f;
                float speed = 2.f + random.next() * 2.f;
                float phase = std::fmod(age * speed + random.next(), 1.f);     // Each one falls in & starts again
                std::vector<ImVec2> points;
                for (int k = 0; k <= 10; k++) {
                    float t = k / 10.f;
                    float r = unit_radius * (1.1f + (5.f - 1.1f) * (1.f - phase) * (1.f - t));
                    float a = start_angle + spin * 0.6f + t * 1.6f + phase * 2.f;
                    points.push_back(center + ImVec2(std::cos(a) * r, std::sin(a) * r * 0.75f));
                }
                d->AddPolyline(points.data(), static_cast<int>(points.size()), color(170, 90, 255, 0.35f * strength * phase), 0, 1.5f);
            }
        }

        // Glow around it
        if (radius > 0.5f) {
            for (int ring = 6; ring >= 1; ring--)
                d->AddCircleFilled(center, radius * (1.f + ring * 0.35f), color(140, 40, 230, 0.07f), 48);

            // The disc of light turning around it, flattened, magenta to violet arcs
            for (int arc = 0; arc < 3; arc++) {
                float r = radius * (1.25f + arc * 0.22f);
                float from = spin * (1.f + arc * 0.3f) + arc * 2.1f;
                d->PathClear();
                for (int k = 0; k <= 24; k++) {
                    float a = from + k / 24.f * 4.2f;
                    d->PathLineTo(center + ImVec2(std::cos(a) * r, std::sin(a) * r * 0.45f));
                }
                d->PathStroke(arc == 0 ? color(255, 120, 255, 0.85f) : color(170, 90, 255, 0.6f), 0, std::max(1.5f, radius * 0.12f));
            }

            // The hole, a bright rim
            d->AddCircleFilled(center, radius, color(0, 0, 0, 1.f), 48);
            d->AddCircle(center, radius * 1.04f, color(255, 170, 255, 0.9f), 48, std::max(1.f, radius * 0.06f));
        }

        // The body: points where its bones were, a little around each, going in one after another
        if (!strike.bones.empty() && age < COLLAPSE) {
            for (int k = 0; k < POINTS; k++) {
                const auto& bone = strike.bones[static_cast<size_t>(random.next() * strike.bones.size()) % strike.bones.size()];
                auto start = bone + Vec3_t(random.signed_next() * 5.f, random.signed_next() * 5.f, random.signed_next() * 5.f);
                float delay = random.next() * 0.45f;
                float turn = (random.next() < 0.5f ? -1.f : 1.f) * (2.5f + random.next() * 2.f);

                auto place = [&](float progress) {
                    float eased = progress * progress;
                    auto offset = start - strike.center;
                    float angle = eased * turn;
                    float c = std::cos(angle), s = std::sin(angle);
                    auto turned = Vec3_t(offset.x * c - offset.y * s, offset.x * s + offset.y * c, offset.z);
                    return strike.center + turned * (1.f - eased);
                };

                float progress = std::clamp((age - PULL_FROM - delay) / PULL_TIME, 0.f, 1.f);
                if (progress >= 1.f)
                    continue;
                float alpha = (1.f - progress) * std::min(1.f, age / 0.1f);

                ImVec2 head, tail;
                if (!project(place(progress), head) || !project(place(std::max(0.f, progress - 0.08f)), tail))
                    continue;

                // Still: a glowing point of the body. Moving: a streak along its way
                if (progress <= 0.f) {
                    d->AddCircleFilled(head, 2.2f, color(230, 200, 255, 0.9f * alpha), 8);
                }
                else {
                    d->AddLine(tail, head, color(200, 110, 255, 0.5f * alpha), 3.5f);
                    d->AddLine(tail, head, color(250, 225, 255, alpha), 1.4f);
                }
            }
        }

        // Closed: a flash & a ring going out
        float burst = (age - COLLAPSE - COLLAPSE_TIME) / BURST_TIME;
        if (burst >= 0.f && burst <= 1.f) {
            float fade = 1.f - burst;
            d->AddCircleFilled(center, unit_radius * (0.4f + 0.8f * burst), color(255, 230, 255, 0.8f * fade * fade), 32);
            d->AddCircle(center, unit_radius * (0.5f + 3.5f * burst), color(220, 120, 255, fade), 48, 2.f + 4.f * fade);
            d->AddCircle(center, unit_radius * (0.3f + 2.2f * burst), color(255, 200, 255, 0.7f * fade), 48, 1.5f);
        }
    }

    // Where a point of the world is on the screen, false behind the camera
    struct Projector {
        const view_matrix_t& matrix;
        Vec2_t display;
        bool operator()(const Vec3_t& world, ImVec2& out) const {
            Vec2_t screen;
            if (!matrix.wts(world, display, screen, false))
                return false;
            out = ImVec2(screen.x, screen.y);
            return true;
        }
    };

    ImU32 Shade(float r, float g, float b, float a, float dim) {
        auto byte = [](float v) { return static_cast<int>(std::clamp(v, 0.f, 255.f)); };
        return IM_COL32(byte(r), byte(g), byte(b), byte(255.f * a * dim));
    }

    // Pixels of one world unit at a point (from a unit straight up there)
    float PixelsPerUnit(const Projector& project, const Vec3_t& at) {
        ImVec2 a, b;
        if (!project(at, a) || !project(at + Vec3_t(0.f, 0.f, 10.f), b))
            return 0.f;
        return std::fabs(a.y - b.y) / 10.f;
    }

    // A point of the body: a bone it had, a little around it
    Vec3_t BodyPoint(const KillEffect::Strike& strike, Random& random, float spread) {
        const auto& bone = strike.bones[static_cast<size_t>(random.next() * strike.bones.size()) % strike.bones.size()];
        return bone + Vec3_t(random.signed_next() * spread, random.signed_next() * spread, random.signed_next() * spread);
    }

    // Shatter: the body turns to glass for a moment, then breaks: shards fly out, turn & flash, fall to the ground
    void DrawShatter(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr float BREAK = 0.18f;      // Glass this long, then it breaks
        constexpr float GRAVITY = 520.f;
        constexpr int SHARDS = 90;

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.6f, 0.f, 1.f);
        float floor_z = strike.feet.z + 1.f;
        Random random{ strike.seed | 1u };

        for (int k = 0; k < SHARDS && !strike.bones.empty(); k++) {
            auto base = BodyPoint(strike, random, 4.f);
            Vec3_t corners[3];
            for (auto& corner : corners)
                corner = Vec3_t(random.signed_next() * 7.f, random.signed_next() * 7.f, random.signed_next() * 7.f);

            // Out from the middle of the body, up, then down
            auto out = base - strike.center;
            out.z = 0.f;
            float length = out.length();
            auto direction = length > 0.1f ? out * (1.f / length) : Vec3_t(random.signed_next(), random.signed_next(), 0.f);
            float speed = 60.f + random.next() * 150.f;
            float lift = 40.f + random.next() * 150.f;
            float spin = random.signed_next() * 12.f;
            float flip = 4.f + random.next() * 10.f;
            float phase = random.next() * 6.2831853f;

            float t = std::max(0.f, age - BREAK);
            auto pos = base + direction * (speed * t) + Vec3_t(0.f, 0.f, lift * t - 0.5f * GRAVITY * t * t);
            pos.z = std::max(pos.z, floor_z);

            float angle = spin * t, c = std::cos(angle), s = std::sin(angle), squash = std::cos(flip * t);
            ImVec2 screen[3];
            bool seen = true;
            for (int i = 0; i < 3; i++) {
                const auto& o = corners[i];
                seen &= project(pos + Vec3_t(o.x * c - o.y * s, o.x * s + o.y * c, o.z * squash), screen[i]);
            }
            if (!seen)
                continue;

            // Catches the light now & then
            float glint = std::pow(std::max(0.f, std::sin(age * 9.f + phase)), 10.f);
            d->AddTriangleFilled(screen[0], screen[1], screen[2], Shade(150.f + 105.f * glint, 210.f + 45.f * glint, 255.f, (0.35f + 0.55f * glint) * fade, dim));
            d->AddTriangle(screen[0], screen[1], screen[2], Shade(225.f, 245.f, 255.f, 0.9f * fade, dim), 1.2f);
        }

        // The moment it breaks: a flash & a ring
        ImVec2 center;
        float unit = PixelsPerUnit(project, strike.center);
        float burst = (age - BREAK) / 0.4f;
        if (unit > 0.f && burst >= 0.f && burst <= 1.f && project(strike.center, center)) {
            float f = 1.f - burst;
            d->AddCircleFilled(center, unit * (8.f + 30.f * burst), Shade(220.f, 240.f, 255.f, 0.5f * f * f, dim), 32);
            d->AddCircle(center, unit * (10.f + 90.f * burst), Shade(200.f, 235.f, 255.f, f, dim), 48, 1.5f + 3.f * f);
        }
    }

    // Glitch: the body in pixels with the colors split, shaking in steps, then the pixels break up & go up; bars of
    // broken picture over it
    void DrawGlitch(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr int PIXELS = 120;
        constexpr float STEP = 0.05f;       // The picture jumps this often
        constexpr float HOLD = 0.25f;       // Whole this long

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.5f, 0.f, 1.f);
        float unit = PixelsPerUnit(project, strike.center);
        ImVec2 center;
        if (unit <= 0.f || !project(strike.center, center))
            return;

        auto step = static_cast<uint32_t>(age / STEP);
        Random random{ strike.seed | 1u };

        for (int k = 0; k < PIXELS && !strike.bones.empty(); k++) {
            auto base = BodyPoint(strike, random, 5.f);
            float delay = random.next() * 0.5f;
            float rise = 60.f + random.next() * 120.f;

            float t = std::max(0.f, age - HOLD - delay);
            float life = 1.f - t / 0.9f;
            if (life <= 0.f)
                continue;

            Random jitter{ (strike.seed ^ (k * 7919u) ^ (step * 2654435761u)) | 1u };
            auto pos = base + Vec3_t(jitter.signed_next() * (1.5f + t * 25.f), jitter.signed_next() * (1.5f + t * 25.f), rise * t * t * 1.5f);
            ImVec2 at;
            if (!project(pos, at))
                continue;

            float half = std::max(1.5f, unit * 2.2f * life);
            float split = unit * 1.2f + 1.5f;
            float a = fade * std::min(1.f, life * 1.5f);
            d->AddRectFilled(at - ImVec2(half + split, half), at + ImVec2(half - split, half), Shade(255.f, 40.f, 120.f, 0.55f * a, dim));
            d->AddRectFilled(at - ImVec2(half - split, half), at + ImVec2(half + split, half), Shade(40.f, 230.f, 255.f, 0.55f * a, dim));
            d->AddRectFilled(at - ImVec2(half, half), at + ImVec2(half, half), Shade(240.f, 240.f, 255.f, 0.9f * a, dim));
        }

        // Bars of broken picture, jumping with it
        if (age < 0.9f) {
            Random bars{ (strike.seed ^ (step * 40503u)) | 1u };
            float strength = 1.f - age / 0.9f;
            for (int i = 0; i < 8; i++) {
                float y = center.y + bars.signed_next() * unit * 45.f;
                float w = unit * 40.f * (0.3f + bars.next() * 1.4f);
                float x = center.x + bars.signed_next() * unit * 20.f - w * 0.5f;
                float h = std::max(1.5f, unit * (0.8f + bars.next() * 3.f));
                bool magenta = bars.next() < 0.5f;
                d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                    magenta ? Shade(255.f, 50.f, 170.f, 0.5f * strength, dim) : Shade(50.f, 240.f, 255.f, 0.5f * strength, dim));
            }
        }
    }

    // A column of light standing in the world: each piece as wide as the column is at its distance (thinner far up, like a
    // real one, not a flat band on the screen), fading out toward its top. Layers: { width, r, g, b, alpha }
    void DrawBeam(ImDrawList* d, const Projector& project, const Vec3_t& bottom, float height, float radius, float alpha, float dim) {
        constexpr int SEGMENTS = 32;
        struct Layer { float width, r, g, b, a; };
        constexpr Layer LAYERS[] = {
            { 2.4f, 255.f, 190.f, 70.f,  0.10f },
            { 1.5f, 255.f, 210.f, 110.f, 0.18f },
            { 0.8f, 255.f, 235.f, 170.f, 0.38f },
            { 0.3f, 255.f, 255.f, 240.f, 0.85f },
        };

        // The right of the camera in the world: always across the view, also looking straight up the column
        Vec3_t right(project.matrix[0][0], project.matrix[0][1], project.matrix[0][2]);
        float length = right.length();
        if (length < 0.0001f)
            return;
        right = right * (1.f / length);

        // Pieces closer together low down, where the column is near & large
        ImVec2 points[SEGMENTS + 1];
        float half[SEGMENTS + 1];
        float fade[SEGMENTS + 1];
        bool seen[SEGMENTS + 1];
        for (int i = 0; i <= SEGMENTS; i++) {
            float along = static_cast<float>(i) / SEGMENTS;
            along *= along;
            auto at = bottom + Vec3_t(0.f, 0.f, height * along);
            ImVec2 side;
            seen[i] = project(at, points[i]) && project(at + right * radius, side);
            half[i] = seen[i] ? std::sqrt((side.x - points[i].x) * (side.x - points[i].x) + (side.y - points[i].y) * (side.y - points[i].y)) : 0.f;
            fade[i] = std::pow(1.f - along, 0.8f);
        }

        for (const auto& layer : LAYERS) {
            for (int i = 0; i < SEGMENTS; i++) {
                if (!seen[i] || !seen[i + 1])
                    continue;
                auto a = points[i], b = points[i + 1];
                float dx = b.x - a.x, dy = b.y - a.y;
                float span = std::sqrt(dx * dx + dy * dy);
                // Seen end on (straight up or down it): across any way
                ImVec2 across = span > 0.01f ? ImVec2(-dy / span, dx / span) : ImVec2(1.f, 0.f);
                float wa = std::max(0.6f, half[i] * layer.width), wb = std::max(0.6f, half[i + 1] * layer.width);
                float a_alpha = layer.a * alpha * (fade[i] + fade[i + 1]) * 0.5f;
                // Looking along it: a round end, not a sliver
                if (span < std::min(wa, wb) * 0.5f) {
                    d->AddCircleFilled(a, wa, Shade(layer.r, layer.g, layer.b, a_alpha * 0.5f, dim), 24);
                    continue;
                }
                d->AddQuadFilled(a + across * wa, b + across * wb, b - across * wb, a - across * wa,
                    Shade(layer.r, layer.g, layer.b, a_alpha, dim));
            }
        }
    }

    // Ascend: a beam of gold light comes down where the body was (from the ceiling or the sky), the body rises in it as
    // points of light, sparks go up the beam, a ring of light spreads on the ground
    void DrawAscend(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr float MAX_HEIGHT = 2500.f;
        constexpr int MOTES = 110;
        constexpr int SPARKS = 24;

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.7f, 0.f, 1.f);
        float open = std::clamp(age / 0.3f, 0.f, 1.f);
        float unit = PixelsPerUnit(project, strike.center);
        if (unit <= 0.f)
            return;

        float height = MAX_HEIGHT;
        {
            MapCollision::Hit above;
            auto from = strike.feet + Vec3_t(0.f, 0.f, 72.f);
            if (MapCollision::Trace(from, strike.feet + Vec3_t(0.f, 0.f, MAX_HEIGHT), above))
                height = std::max(120.f, 72.f + (MAX_HEIGHT - 72.f) * above.fraction);
        }

        // The beam: wide soft gold to a white core, a real column in the world
        DrawBeam(d, project, strike.feet, height, 13.f * open * (1.f + 0.05f * std::sin(age * 20.f)), fade * open, dim);

        // A ring of light spreading on the ground
        {
            float spread = std::clamp(age / 1.2f, 0.f, 1.f);
            float radius = 20.f + 90.f * spread;
            std::vector<ImVec2> ring;
            for (int k = 0; k < 32; k++) {
                float angle = k / 32.f * 6.2831853f;
                ImVec2 at;
                if (project(strike.feet + Vec3_t(std::cos(angle) * radius, std::sin(angle) * radius, 2.f), at))
                    ring.push_back(at);
            }
            if (ring.size() == 32)
                d->AddPolyline(ring.data(), 32, Shade(255.f, 215.f, 120.f, (1.f - spread) * 0.9f * fade, dim), ImDrawFlags_Closed, 2.f + 3.f * (1.f - spread));
        }

        Random random{ strike.seed | 1u };

        // The body rising as gold points, swaying, whiter as they go
        for (int k = 0; k < MOTES && !strike.bones.empty(); k++) {
            auto base = BodyPoint(strike, random, 4.f);
            float delay = random.next() * 0.6f;
            float speed = 30.f + random.next() * 60.f;
            float phase = random.next() * 6.2831853f;

            float t = std::max(0.f, age - 0.15f - delay);
            float life = 1.f - t / 1.6f;
            if (life <= 0.f)
                continue;
            auto pos = base + Vec3_t(std::sin(age * 3.f + phase) * 6.f * t, std::cos(age * 2.5f + phase) * 6.f * t, speed * t + 30.f * t * t);
            ImVec2 at;
            if (!project(pos, at))
                continue;
            float white = 1.f - life;
            float a = fade * std::min(1.f, life * 2.f);
            d->AddCircleFilled(at, std::max(2.f, unit * 1.6f), Shade(255.f, 200.f, 90.f, 0.35f * a, dim), 10);
            d->AddCircleFilled(at, std::max(1.f, unit * 0.7f), Shade(255.f, 225.f + 30.f * white, 150.f + 105.f * white, a, dim), 8);
        }

        // Sparks going up the beam
        for (int k = 0; k < SPARKS; k++) {
            float offset = random.next() * height;
            float speed = 300.f + random.next() * 300.f;
            auto pos = strike.feet + Vec3_t(random.signed_next() * 10.f, random.signed_next() * 10.f, std::fmod(offset + age * speed, height));
            ImVec2 at;
            if (!project(pos, at))
                continue;
            float size = std::max(2.f, unit * 2.f);
            auto color = Shade(255.f, 245.f, 200.f, 0.8f * fade * open, dim);
            d->AddLine(at - ImVec2(size, 0.f), at + ImVec2(size, 0.f), color, 1.2f);
            d->AddLine(at - ImVec2(0.f, size), at + ImVec2(0.f, size), color, 1.2f);
        }
    }

    // The right & up of the camera in the world, from the view matrix
    bool CameraAxes(const Projector& project, Vec3_t& right, Vec3_t& up) {
        right = Vec3_t(project.matrix[0][0], project.matrix[0][1], project.matrix[0][2]);
        up = Vec3_t(project.matrix[1][0], project.matrix[1][1], project.matrix[1][2]);
        float r = right.length(), u = up.length();
        if (r < 0.0001f || u < 0.0001f)
            return false;
        right = right * (1.f / r);
        up = up * (1.f / u);
        return true;
    }

    // Lowest & highest a body reaches: its feet, its bones (the top of the head a little over the highest)
    void BodyHeights(const KillEffect::Strike& strike, float& bottom, float& top) {
        bottom = strike.feet.z;
        top = strike.feet.z + 60.f;
        for (const auto& bone : strike.bones)
            top = std::max(top, bone.z + 5.f);
    }

    // A circle lying on the ground, on the screen. False when part of it is behind the camera
    bool GroundCircle(const Projector& project, const Vec3_t& middle, float radius, std::vector<ImVec2>& out, int count = 32) {
        out.clear();
        for (int k = 0; k < count; k++) {
            float angle = k / static_cast<float>(count) * 6.2831853f;
            ImVec2 at;
            if (!project(middle + Vec3_t(std::cos(angle) * radius, std::sin(angle) * radius, 0.f), at))
                return false;
            out.push_back(at);
        }
        return true;
    }

    // Frost: ice climbs over the body, it stands frozen & glints, cracks, then crumbles into falling crystals; cold mist
    void DrawFrost(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr int CRYSTALS = 140;
        constexpr float FREEZE = 0.35f;     // Feet to head
        constexpr float BREAK = 0.95f;
        constexpr float GRAVITY = 480.f;

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.6f, 0.f, 1.f);
        float unit = PixelsPerUnit(project, strike.center);
        Vec3_t right, up;
        if (unit <= 0.f || strike.bones.empty() || !CameraAxes(project, right, up))
            return;

        float bottom, top;
        BodyHeights(strike, bottom, top);
        float frozen = (top - bottom + 6.f) * std::clamp(age / FREEZE, 0.f, 1.f);
        float floor_z = strike.feet.z + 1.f;

        // Cold mist
        ImVec2 middle;
        if (project(strike.center, middle))
            for (int ring = 4; ring >= 1; ring--)
                d->AddCircleFilled(middle, unit * (14.f + ring * 8.f), Shade(170.f, 215.f, 255.f, 0.05f * (age < BREAK ? 1.f : fade), dim), 24);

        Random random{ strike.seed | 1u };
        for (int k = 0; k < CRYSTALS; k++) {
            auto base = BodyPoint(strike, random, 4.f);
            float size = 2.f + random.next() * 3.f;
            float spin = random.signed_next() * 8.f;
            float phase = random.next() * 6.2831853f;
            auto drift = Vec3_t(random.signed_next() * 45.f, random.signed_next() * 45.f, random.next() * 70.f);
            float late = random.next() * 0.15f;
            if (base.z - bottom > frozen)
                continue;

            float t = std::max(0.f, age - BREAK - late);
            auto pos = base + Vec3_t(drift.x * t, drift.y * t, drift.z * t - 0.5f * GRAVITY * t * t);
            pos.z = std::max(pos.z, floor_z);

            float angle = spin * t + phase, c = std::cos(angle), s = std::sin(angle);
            auto a = right * c + up * s;
            auto b = up * c - right * s;
            ImVec2 q[4];
            if (!project(pos + b * (size * 1.5f), q[0]) || !project(pos + a * size, q[1]) ||
                !project(pos - b * (size * 1.5f), q[2]) || !project(pos - a * size, q[3]))
                continue;

            float glint = std::pow(std::max(0.f, std::sin(age * 7.f + phase * 3.f)), 12.f);
            d->AddQuadFilled(q[0], q[1], q[2], q[3], Shade(160.f + 95.f * glint, 215.f + 40.f * glint, 255.f, (0.5f + 0.4f * glint) * fade, dim));
            d->AddQuad(q[0], q[1], q[2], q[3], Shade(235.f, 250.f, 255.f, 0.85f * fade, dim), 1.f);
        }

        // Cracks just before it breaks
        if (age > BREAK - 0.2f && age < BREAK + 0.1f) {
            Random cracks{ (strike.seed ^ 0x51ED27u) | 1u };
            float a = 1.f - std::fabs(age - BREAK + 0.05f) / 0.15f;
            for (int i = 0; i < 7; i++) {
                auto from = BodyPoint(strike, cracks, 2.f);
                auto to = BodyPoint(strike, cracks, 2.f);
                auto points = Bolt(from, to, 6, 3.f, cracks);
                std::vector<ImVec2> screen;
                for (const auto& point : points) {
                    ImVec2 at;
                    if (!project(point, at))
                        break;
                    screen.push_back(at);
                }
                if (screen.size() == points.size())
                    d->AddPolyline(screen.data(), static_cast<int>(screen.size()), Shade(255.f, 255.f, 255.f, a, dim), 0, 1.5f);
            }
        }
    }

    // Slash: a blade of light cuts across the body, the screen flashes, the two halves slide apart & fall, sparks fly
    // from the cut
    void DrawSlash(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project, ImVec2 display) {
        constexpr float CUT = 0.12f;
        constexpr int POINTS = 160;
        constexpr float GRAVITY = 300.f;

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.5f, 0.f, 1.f);
        float unit = PixelsPerUnit(project, strike.center);
        Vec3_t right, up;
        ImVec2 center;
        if (unit <= 0.f || strike.bones.empty() || !CameraAxes(project, right, up) || !project(strike.center, center))
            return;

        Random random{ strike.seed | 1u };
        float angle = -0.55f + random.signed_next() * 0.35f;
        ImVec2 dir(std::cos(angle), std::sin(angle));
        ImVec2 perp(-dir.y, dir.x);
        float reach = unit * 85.f;
        float after = std::max(0.f, age - CUT);

        // The halves, apart along the cut & across it, falling
        auto along_world = right * dir.x - up * dir.y;
        auto across_world = right * perp.x - up * perp.y;
        float floor_z = strike.feet.z + 1.f;
        for (int k = 0; k < POINTS; k++) {
            auto base = BodyPoint(strike, random, 4.f);
            ImVec2 start;
            if (!project(base, start))
                continue;
            float side_distance = (start.x - center.x) * perp.x + (start.y - center.y) * perp.y;
            float side = side_distance >= 0.f ? 1.f : -1.f;
            auto pos = base + across_world * (side * 22.f * after) + along_world * (side * 18.f * after) +
                Vec3_t(0.f, 0.f, -0.5f * GRAVITY * after * after * (side > 0.f ? 1.f : 0.6f));
            pos.z = std::max(pos.z, floor_z);
            ImVec2 at;
            if (!project(pos, at))
                continue;
            float near_cut = std::clamp(1.f - std::fabs(side_distance) / (unit * 4.f), 0.f, 1.f);
            near_cut *= after > 0.f ? 1.f : 0.f;
            d->AddCircleFilled(at, std::max(1.5f, unit * 1.5f),
                Shade(205.f + 50.f * near_cut, 210.f - 150.f * near_cut, 220.f - 160.f * near_cut, 0.85f * fade, dim), 8);
        }

        // The blade of light, drawn across fast, then fading
        float wipe = std::clamp(age / CUT, 0.f, 1.f);
        float line = age < CUT ? 1.f : std::max(0.f, 1.f - after / 0.35f);
        if (line > 0.f) {
            auto a = center - dir * (reach * 1.3f);
            auto b = a + dir * (reach * 2.6f * wipe);
            d->AddLine(center - dir * 3000.f, center + dir * 3000.f, Shade(255.f, 200.f, 200.f, 0.2f * line, dim), 1.f);
            d->AddLine(a, b, Shade(255.f, 50.f, 60.f, 0.3f * line, dim), std::max(4.f, unit * 6.f));
            d->AddLine(a, b, Shade(255.f, 150.f, 150.f, 0.6f * line, dim), std::max(2.5f, unit * 2.5f));
            d->AddLine(a, b, Shade(255.f, 255.f, 255.f, line, dim), std::max(1.5f, unit * 0.9f));
        }

        // The flash as it cuts
        if (age >= CUT && age < CUT + 0.08f)
            d->AddRectFilled(ImVec2(0.f, 0.f), display, Shade(255.f, 255.f, 255.f, 0.25f * (1.f - (age - CUT) / 0.08f), dim));

        // Sparks out of the cut
        if (after > 0.f && after < 0.6f) {
            for (int i = 0; i < 22; i++) {
                auto from = center + dir * (random.signed_next() * reach * 0.6f);
                float speed = 200.f + random.next() * 350.f;
                auto velocity = perp * (random.signed_next() * speed) + dir * (random.signed_next() * speed * 0.4f);
                auto head = from + velocity * after + ImVec2(0.f, 600.f * after * after);
                auto tail = from + velocity * std::max(0.f, after - 0.03f) + ImVec2(0.f, 600.f * std::max(0.f, after - 0.03f) * std::max(0.f, after - 0.03f));
                float a = 1.f - after / 0.6f;
                d->AddLine(tail, head, Shade(255.f, 180.f, 90.f, a, dim), 2.f);
            }
        }
    }

    // Pixels: the body falls apart into blocks (the shape of its bones on a grid) that jump, turn, fall & bounce on the
    // ground, then shrink away
    void DrawPixels(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project, const Vec3_t& eye) {
        constexpr float BLOCK = 5.5f;
        constexpr float DROP = 0.12f;
        constexpr float GRAVITY = 600.f;
        constexpr int MAX_BLOCKS = 90;
        constexpr float STEP = 1.f / 60.f;
        static const float PALETTE[][3] = {
            { 120.f, 85.f, 60.f }, { 95.f, 140.f, 60.f }, { 205.f, 175.f, 125.f }, { 70.f, 95.f, 150.f }, { 150.f, 150.f, 155.f },
        };

        if (strike.bones.empty())
            return;
        float total = KillEffect::Duration(strike.kind);
        float shrink = std::clamp((total - age) / 0.5f, 0.f, 1.f);
        float floor_z = strike.feet.z;

        // The blocks: points of the body on a grid, one block per cell
        Random random{ strike.seed | 1u };
        std::vector<Vec3_t> cells;
        for (int k = 0; k < 260 && cells.size() < MAX_BLOCKS; k++) {
            auto point = BodyPoint(strike, random, 4.f);
            auto cell = Vec3_t(std::round(point.x / BLOCK) * BLOCK, std::round(point.y / BLOCK) * BLOCK, std::round(point.z / BLOCK) * BLOCK);
            bool taken = false;
            for (const auto& other : cells)
                taken |= (other - cell).length_sqr() < 0.01f;
            if (!taken)
                cells.push_back(cell);
        }

        struct Block {
            Vec3_t pos;
            float angle;
            const float* color;
            float distance;
        };
        std::vector<Block> blocks;
        float half = BLOCK * 0.5f * shrink;
        for (const auto& cell : cells) {
            auto velocity = Vec3_t(random.signed_next() * 70.f, random.signed_next() * 70.f, 60.f + random.next() * 130.f);
            float spin = random.signed_next() * 5.f;
            const float* color = PALETTE[static_cast<size_t>(random.next() * 5.f) % 5];
            float late = random.next() * 0.1f;

            float t = std::max(0.f, age - DROP - late);
            auto pos = cell;
            int steps = std::min(static_cast<int>(t / STEP), 160);
            for (int s = 0; s < steps; s++) {
                velocity.z -= GRAVITY * STEP;
                pos = pos + velocity * STEP;
                if (pos.z - half < floor_z) {
                    pos.z = floor_z + half;
                    velocity.z = -velocity.z * 0.35f;
                    velocity.x *= 0.7f;
                    velocity.y *= 0.7f;
                }
            }
            blocks.push_back({ pos, spin * t, color, (pos - eye).length_sqr() });
        }

        // Far ones first, near ones over them
        std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.distance > b.distance; });

        static const int FACES[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 }, { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
        static const float LIGHT[6] = { 0.8f, 0.65f, 0.75f, 0.6f, 1.f, 0.45f };
        for (const auto& block : blocks) {
            float c = std::cos(block.angle), s = std::sin(block.angle);
            ImVec2 corners[8];
            bool seen = true;
            for (int i = 0; i < 8; i++) {
                float x = (i & 1) ? half : -half, y = (i & 2) ? half : -half, z = (i & 4) ? half : -half;
                seen &= project(block.pos + Vec3_t(x * c - y * s, x * s + y * c, z), corners[i]);
            }
            if (!seen)
                continue;

            const Vec3_t normals[6] = { Vec3_t(c, s, 0.f), Vec3_t(-c, -s, 0.f), Vec3_t(-s, c, 0.f), Vec3_t(s, -c, 0.f), Vec3_t(0.f, 0.f, 1.f), Vec3_t(0.f, 0.f, -1.f) };
            auto to_eye = eye - block.pos;
            for (int f = 0; f < 6; f++) {
                if (normals[f].x * to_eye.x + normals[f].y * to_eye.y + normals[f].z * to_eye.z <= 0.f)
                    continue;
                const auto* q = FACES[f];
                d->AddQuadFilled(corners[q[0]], corners[q[1]], corners[q[2]], corners[q[3]],
                    Shade(block.color[0] * LIGHT[f], block.color[1] * LIGHT[f], block.color[2] * LIGHT[f], 1.f, dim));
                d->AddQuad(corners[q[0]], corners[q[1]], corners[q[2]], corners[q[3]], Shade(0.f, 0.f, 0.f, 0.45f, dim), 1.f);
            }
        }
    }


    // Meteor: a fireball comes down slanted from the sky with a burning tail & hits where the body stands (still seen as
    // dim points), the screen flashes, a shockwave & a dark crater on the ground, rocks & the body thrown out
    void DrawMeteor(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project, ImVec2 display) {
        constexpr float FALL = 0.55f;       // Seconds to come down
        constexpr float HIGH = 900.f;
        constexpr float FAR_SIDE = 450.f;
        constexpr float GRAVITY = 500.f;
        constexpr int ROCKS = 40;
        constexpr int POINTS = 120;

        float total = KillEffect::Duration(strike.kind);
        float fade = std::clamp((total - age) / 0.6f, 0.f, 1.f);
        float unit = PixelsPerUnit(project, strike.center);
        if (unit <= 0.f || strike.bones.empty())
            return;

        Random random{ strike.seed | 1u };
        float heading = random.next() * 6.2831853f;
        auto impact = strike.feet + Vec3_t(0.f, 0.f, 6.f);
        auto sky = impact + Vec3_t(std::cos(heading) * FAR_SIDE, std::sin(heading) * FAR_SIDE, HIGH);
        auto at_time = [&](float time) {
            float t = std::clamp(time / FALL, 0.f, 1.f);
            return sky + (impact - sky) * (t * t);     // Faster as it comes
        };
        float after = age - FALL;

        // The fireball & its tail
        if (after < 0.f) {
            std::vector<ImVec2> tail;
            for (int k = 0; k <= 14; k++) {
                ImVec2 at;
                if (!project(at_time(age - k * 0.025f), at))
                    break;
                tail.push_back(at);
            }
            for (size_t k = 1; k < tail.size(); k++) {
                float f = 1.f - static_cast<float>(k) / tail.size();
                d->AddLine(tail[k - 1], tail[k], Shade(255.f, 90.f + 120.f * f, 20.f, 0.5f * f, dim), std::max(2.f, unit * 14.f * f));
            }
            ImVec2 head;
            auto head_world = at_time(age);
            float head_unit = PixelsPerUnit(project, head_world);
            if (project(head_world, head) && head_unit > 0.f) {
                d->AddCircleFilled(head, head_unit * 22.f, Shade(255.f, 120.f, 30.f, 0.25f, dim), 24);
                d->AddCircleFilled(head, head_unit * 12.f, Shade(255.f, 190.f, 80.f, 0.7f, dim), 24);
                d->AddCircleFilled(head, head_unit * 6.f, Shade(255.f, 250.f, 220.f, 1.f, dim), 16);
            }
        }

        // The flash as it hits
        if (after >= 0.f && after < 0.3f)
            d->AddRectFilled(ImVec2(0.f, 0.f), display, Shade(255.f, 230.f, 190.f, 0.35f * (1.f - after / 0.3f), dim));

        // Crater, the shockwave
        if (after >= 0.f) {
            std::vector<ImVec2> ring;
            if (GroundCircle(project, strike.feet + Vec3_t(0.f, 0.f, 1.f), 38.f, ring))
                d->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), Shade(20.f, 12.f, 8.f, 0.6f * fade, dim));
            float wave = std::clamp(after / 0.5f, 0.f, 1.f);
            if (wave < 1.f && GroundCircle(project, strike.feet + Vec3_t(0.f, 0.f, 2.f), 15.f + 180.f * wave, ring)) {
                d->AddPolyline(ring.data(), static_cast<int>(ring.size()), Shade(255.f, 150.f, 60.f, 0.4f * (1.f - wave), dim), ImDrawFlags_Closed, std::max(3.f, unit * 8.f * (1.f - wave)));
                d->AddPolyline(ring.data(), static_cast<int>(ring.size()), Shade(255.f, 240.f, 200.f, 1.f - wave, dim), ImDrawFlags_Closed, 2.f);
            }
        }

        // The body: dim points until it is hit, then thrown out glowing
        float floor_z = strike.feet.z + 1.f;
        for (int k = 0; k < POINTS; k++) {
            auto base = BodyPoint(strike, random, 4.f);
            auto out = base - strike.feet;
            out.z = 0.f;
            float length = std::max(1.f, out.length());
            auto velocity = out * ((120.f + random.next() * 200.f) / length) + Vec3_t(0.f, 0.f, 80.f + random.next() * 200.f);
            float t = std::max(0.f, after);
            auto pos = base + velocity * t + Vec3_t(0.f, 0.f, -0.5f * GRAVITY * t * t);
            pos.z = std::max(pos.z, floor_z);
            ImVec2 at;
            if (!project(pos, at))
                continue;
            float hot = after >= 0.f ? std::max(0.f, 1.f - t / 1.2f) : 0.f;
            d->AddCircleFilled(at, std::max(1.4f, unit * 1.4f),
                Shade(120.f + 135.f * hot, 120.f + 60.f * hot, 120.f - 90.f * hot, (after < 0.f ? 0.5f : hot) * fade, dim), 8);
        }

        // Rocks
        if (after >= 0.f) {
            for (int k = 0; k < ROCKS; k++) {
                float angle = random.next() * 6.2831853f;
                float speed = 100.f + random.next() * 250.f;
                float lift = 150.f + random.next() * 250.f;
                float size = 2.f + random.next() * 3.f;
                auto pos = impact + Vec3_t(std::cos(angle) * speed * after, std::sin(angle) * speed * after, lift * after - 0.5f * GRAVITY * after * after);
                pos.z = std::max(pos.z, floor_z);
                ImVec2 at;
                if (!project(pos, at))
                    continue;
                float half = std::max(1.5f, PixelsPerUnit(project, pos) * size);
                d->AddRectFilled(at - ImVec2(half, half), at + ImVec2(half, half), Shade(90.f, 65.f, 45.f, fade, dim));
            }
        }
    }

    // Tornado: a whirlwind grows where the body was, its strands turning fast, dust on the ground; the body (points) is
    // pulled round into it & taken up out of the top
    void DrawTornado(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr float TALL = 240.f;
        constexpr int STRANDS = 7;
        constexpr int POINTS = 130;

        float total = KillEffect::Duration(strike.kind);
        float grow = std::clamp(age / 0.35f, 0.f, 1.f);
        float fade = std::clamp((total - age) / 0.6f, 0.f, 1.f);
        float strength = grow * fade;
        float unit = PixelsPerUnit(project, strike.center);
        if (unit <= 0.f)
            return;

        auto radius_at = [](float height) { return 6.f + height * 0.33f; };
        auto base = strike.feet;

        // Dust on the ground
        std::vector<ImVec2> ring;
        for (int r = 3; r >= 1; r--)
            if (GroundCircle(project, base + Vec3_t(0.f, 0.f, 2.f), 25.f * r * grow, ring))
                d->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), Shade(170.f, 150.f, 120.f, 0.07f * strength, dim));

        // The strands of the funnel
        Random random{ strike.seed | 1u };
        for (int s = 0; s < STRANDS; s++) {
            float offset = s / static_cast<float>(STRANDS) * 6.2831853f + random.next();
            std::vector<ImVec2> points;
            for (int k = 0; k <= 30; k++) {
                float height = TALL * grow * k / 30.f;
                float angle = offset + height * 0.05f - age * 9.f;
                float r = radius_at(height) * (1.f + 0.1f * std::sin(age * 7.f + s));
                ImVec2 at;
                if (!project(base + Vec3_t(std::cos(angle) * r, std::sin(angle) * r, height), at))
                    break;
                points.push_back(at);
            }
            if (points.size() > 1) {
                d->AddPolyline(points.data(), static_cast<int>(points.size()), Shade(200.f, 205.f, 215.f, 0.18f * strength, dim), 0, std::max(3.f, unit * 5.f));
                d->AddPolyline(points.data(), static_cast<int>(points.size()), Shade(240.f, 245.f, 250.f, 0.45f * strength, dim), 0, std::max(1.f, unit * 1.2f));
            }
        }

        // The body pulled round & up
        for (int k = 0; k < POINTS && !strike.bones.empty(); k++) {
            auto point = BodyPoint(strike, random, 4.f);
            float delay = random.next() * 0.5f;
            float turn = 6.f + random.next() * 6.f;
            float rise = 60.f + random.next() * 90.f;
            float t = std::max(0.f, age - 0.15f - delay);
            float height = std::max(0.f, point.z - base.z) + rise * t + 50.f * t * t;
            if (height > TALL * 1.2f)
                continue;
            auto offset = point - base;
            float start_angle = std::atan2(offset.y, offset.x);
            float start_radius = std::sqrt(offset.x * offset.x + offset.y * offset.y);
            float pull = std::min(1.f, t * 2.f);
            float r = start_radius + (radius_at(height) * 0.8f - start_radius) * pull;
            float angle = start_angle + turn * t;
            ImVec2 at;
            if (!project(base + Vec3_t(std::cos(angle) * r, std::sin(angle) * r, height), at))
                continue;
            float a = fade * (1.f - std::max(0.f, height - TALL) / (TALL * 0.2f));
            d->AddCircleFilled(at, std::max(1.4f, unit * 1.4f), Shade(225.f, 225.f, 235.f, 0.85f * a, dim), 8);
        }
    }

    // TV Off: the body as a flickering hologram with scanlines, then switched off like an old television: squashed to
    // a bright line, the line to a dot, the dot goes out
    void DrawTvOff(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project) {
        constexpr int POINTS = 170;
        constexpr float SQUASH_FROM = 0.3f, SQUASH = 0.18f;
        constexpr float LINE_FROM = 0.5f, LINE = 0.18f;
        constexpr float DOT_FROM = 0.7f, DOT = 0.4f;

        float unit = PixelsPerUnit(project, strike.center);
        ImVec2 center;
        if (unit <= 0.f || strike.bones.empty() || !project(strike.center, center))
            return;

        float tall = std::clamp((age - SQUASH_FROM) / SQUASH, 0.f, 1.f);
        float wide = std::clamp((age - LINE_FROM) / LINE, 0.f, 1.f);
        float sy = 1.f - tall * tall * 0.985f;
        float sx = 1.f - wide * wide;
        float flicker = 0.75f + 0.25f * std::sin(age * 60.f);

        // The body on the screen, squashed toward its middle
        if (age < DOT_FROM) {
            Random random{ strike.seed | 1u };
            float min_x = center.x, max_x = center.x;
            for (int k = 0; k < POINTS; k++) {
                auto point = BodyPoint(strike, random, 3.f);
                ImVec2 at;
                if (!project(point, at))
                    continue;
                at = ImVec2(center.x + (at.x - center.x) * sx, center.y + (at.y - center.y) * sy);
                min_x = std::min(min_x, at.x);
                max_x = std::max(max_x, at.x);
                if (tall < 0.9f)
                    d->AddCircleFilled(at, std::max(1.4f, unit * 1.4f), Shade(110.f, 230.f, 255.f, 0.8f * flicker, dim), 8);
            }

            // Scanlines over it while it stands
            if (tall <= 0.f) {
                float half_h = unit * 40.f;
                for (float y = center.y - half_h; y < center.y + half_h; y += std::max(3.f, unit * 3.f))
                    d->AddLine(ImVec2(min_x, y), ImVec2(max_x, y), Shade(150.f, 240.f, 255.f, 0.12f * flicker, dim), 1.f);
            }

            // Squashed flat: a bright line
            if (tall > 0.5f) {
                float half_w = std::max(2.f, (max_x - min_x) * 0.5f + unit * 6.f) * sx;
                float a = (tall - 0.5f) * 2.f;
                d->AddLine(center - ImVec2(half_w, 0.f), center + ImVec2(half_w, 0.f), Shade(150.f, 240.f, 255.f, 0.4f * a, dim), std::max(4.f, unit * 4.f));
                d->AddLine(center - ImVec2(half_w, 0.f), center + ImVec2(half_w, 0.f), Shade(255.f, 255.f, 255.f, a, dim), std::max(1.5f, unit * 1.2f));
            }
        }

        // The dot, fading
        float dot = (age - DOT_FROM) / DOT;
        if (dot >= -0.05f && dot <= 1.f) {
            float a = 1.f - std::max(0.f, dot);
            d->AddCircleFilled(center, std::max(3.f, unit * 6.f) * a, Shade(150.f, 240.f, 255.f, 0.35f * a, dim), 16);
            d->AddCircleFilled(center, std::max(1.5f, unit * 2.f) * a, Shade(255.f, 255.f, 255.f, a, dim), 12);
        }
    }

    // Supernova: the body falls into its middle where a star forms, brighter & pulsing faster, then it explodes: the
    // screen goes white, shells of light & rays go out, a glowing cloud stays a moment
    void DrawSupernova(ImDrawList* d, const KillEffect::Strike& strike, float age, float dim, const Projector& project, ImVec2 display) {
        constexpr float FORM = 0.65f;       // The star forms, then it explodes
        constexpr int POINTS = 140;
        constexpr int RAYS = 28;

        float total = KillEffect::Duration(strike.kind);
        float unit = PixelsPerUnit(project, strike.center);
        ImVec2 center;
        if (unit <= 0.f || !project(strike.center, center))
            return;

        Random random{ strike.seed | 1u };
        float after = age - FORM;

        if (after < 0.f) {
            // The body falling in
            float form = age / FORM;
            for (int k = 0; k < POINTS && !strike.bones.empty(); k++) {
                auto point = BodyPoint(strike, random, 4.f);
                float delay = random.next() * 0.3f;
                float t = std::clamp((age - delay) / (FORM - delay), 0.f, 1.f);
                auto pos = point + (strike.center - point) * (t * t);
                ImVec2 at;
                if (project(pos, at))
                    d->AddCircleFilled(at, std::max(1.3f, unit * 1.3f), Shade(255.f, 240.f, 200.f, 0.9f * (1.f - t * t), dim), 8);
            }

            // The star
            float pulse = 0.85f + 0.15f * std::sin(age * (10.f + 50.f * form));
            float r = unit * (2.f + 9.f * form) * pulse;
            d->AddCircleFilled(center, r * 3.f, Shade(120.f, 160.f, 255.f, 0.12f * form, dim), 32);
            d->AddCircleFilled(center, r * 1.8f, Shade(200.f, 220.f, 255.f, 0.3f * form, dim), 32);
            d->AddCircleFilled(center, r, Shade(255.f, 255.f, 240.f, 0.95f, dim), 24);
            return;
        }

        float rest = total - FORM;
        float t = after / rest;
        float fade = 1.f - t;

        // White screen
        if (after < 0.3f)
            d->AddRectFilled(ImVec2(0.f, 0.f), display, Shade(255.f, 250.f, 240.f, 0.55f * (1.f - after / 0.3f), dim));

        // The cloud left
        d->AddCircleFilled(center, unit * (30.f + 60.f * t), Shade(150.f, 90.f, 255.f, 0.10f * fade, dim), 40);
        d->AddCircleFilled(center, unit * (15.f + 30.f * t), Shade(255.f, 140.f, 90.f, 0.12f * fade, dim), 40);

        // Shells of light going out
        const float colors[3][3] = { { 255.f, 255.f, 255.f }, { 120.f, 180.f, 255.f }, { 255.f, 130.f, 220.f } };
        for (int shell = 0; shell < 3; shell++) {
            float s = std::clamp(after / (0.5f + shell * 0.2f), 0.f, 1.f);
            if (s >= 1.f)
                continue;
            float r = unit * (8.f + (180.f + shell * 60.f) * (1.f - (1.f - s) * (1.f - s)));
            d->AddCircle(center, r, Shade(colors[shell][0], colors[shell][1], colors[shell][2], 1.f - s, dim), 64, std::max(2.f, unit * 5.f * (1.f - s)));
        }

        // Rays
        float ray = std::clamp(after / 0.6f, 0.f, 1.f);
        if (ray < 1.f) {
            for (int k = 0; k < RAYS; k++) {
                float angle = random.next() * 6.2831853f;
                float length = unit * (60.f + random.next() * 160.f) * (0.3f + ray);
                ImVec2 dir(std::cos(angle), std::sin(angle));
                d->AddLine(center + dir * (length * 0.3f), center + dir * length, Shade(255.f, 245.f, 220.f, 0.8f * (1.f - ray), dim), std::max(1.5f, unit * 1.5f));
            }
        }
    }

    // The auras of the self effect: around our own player, all the time, from where its bones are now

    // Storm: small lightning jumping between parts of the body & out of it, flickering
    void DrawStormAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        float unit = PixelsPerUnit(project, body.center);
        ImVec2 center;
        if (unit <= 0.f || !project(body.center, center))
            return;
        for (int ring = 3; ring >= 1; ring--)
            d->AddCircleFilled(center, unit * (14.f + ring * 8.f), Shade(90.f, 160.f, 255.f, 0.04f, 1.f), 24);

        Random random{ (static_cast<uint32_t>(time / 0.07f) * 2654435761u) | 1u };
        for (int i = 0; i < 5; i++) {
            auto from = BodyPoint(body, random, 2.f);
            auto to = random.next() < 0.5f ? BodyPoint(body, random, 2.f)
                : from + Vec3_t(random.signed_next() * 22.f, random.signed_next() * 22.f, random.signed_next() * 22.f);
            auto points = Bolt(from, to, 6, 4.f, random);
            std::vector<ImVec2> screen;
            for (const auto& point : points) {
                ImVec2 at;
                if (!project(point, at))
                    break;
                screen.push_back(at);
            }
            if (screen.size() != points.size())
                continue;
            int count = static_cast<int>(screen.size());
            d->AddPolyline(screen.data(), count, Shade(80.f, 150.f, 255.f, 0.3f, 1.f), 0, std::max(3.f, unit * 3.f));
            d->AddPolyline(screen.data(), count, Shade(150.f, 210.f, 255.f, 0.6f, 1.f), 0, std::max(1.5f, unit * 1.2f));
            d->AddPolyline(screen.data(), count, Shade(245.f, 250.f, 255.f, 1.f, 1.f), 0, std::max(1.f, unit * 0.45f));
        }
    }

    // Void: three small black holes going round the body with trails, purple specks turning around it
    void DrawVoidAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;

        auto orb_at = [&](int i, float t) {
            float angle = t * 2.2f + i * 2.0943951f;
            return body.center + Vec3_t(std::cos(angle) * 24.f, std::sin(angle) * 24.f, std::sin(t * 1.5f + i) * 12.f);
        };

        Random random{ 0x5EEDu };
        for (int k = 0; k < 40; k++) {
            float start = random.next() * 6.2831853f;
            float speed = 1.2f + random.next() * 1.5f;
            float radius = 12.f + random.next() * 20.f;
            float height = body.feet.z + 8.f + random.next() * 60.f;
            float angle = start + time * speed;
            ImVec2 at;
            if (project(Vec3_t(body.center.x + std::cos(angle) * radius, body.center.y + std::sin(angle) * radius, height), at))
                d->AddCircleFilled(at, std::max(1.f, unit * 0.8f), Shade(200.f, 110.f, 255.f, 0.35f + 0.3f * std::sin(time * 4.f + start), 1.f), 6);
        }

        for (int i = 0; i < 3; i++) {
            for (int k = 10; k >= 1; k--) {
                ImVec2 at;
                if (project(orb_at(i, time - k * 0.035f), at))
                    d->AddCircleFilled(at, std::max(1.f, unit * 2.6f * (1.f - k / 11.f)), Shade(170.f, 70.f, 255.f, 0.25f * (1.f - k / 11.f), 1.f), 10);
            }
            ImVec2 at;
            if (!project(orb_at(i, time), at))
                continue;
            float r = std::max(2.f, unit * 3.5f);
            d->AddCircleFilled(at, r * 2.2f, Shade(150.f, 50.f, 240.f, 0.15f, 1.f), 16);
            d->AddCircleFilled(at, r, Shade(0.f, 0.f, 0.f, 1.f, 1.f), 16);
            d->AddCircle(at, r * 1.08f, Shade(255.f, 150.f, 255.f, 0.9f, 1.f), 16, std::max(1.f, r * 0.15f));
        }
    }

    // Halo: a ring of gold light over the head, bobbing, a soft gold glow, sparkles rising around the body
    void DrawHaloAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;
        Vec3_t head = body.bones.front();
        for (const auto& bone : body.bones)
            if (bone.z > head.z)
                head = bone;

        ImVec2 center;
        if (project(body.center, center))
            for (int ring = 3; ring >= 1; ring--)
                d->AddCircleFilled(center, unit * (12.f + ring * 9.f), Shade(255.f, 210.f, 110.f, 0.035f, 1.f), 24);

        std::vector<ImVec2> ring;
        if (GroundCircle(project, head + Vec3_t(0.f, 0.f, 9.f + std::sin(time * 2.f) * 1.2f), 7.f, ring)) {
            d->AddPolyline(ring.data(), static_cast<int>(ring.size()), Shade(255.f, 200.f, 80.f, 0.3f, 1.f), ImDrawFlags_Closed, std::max(3.f, unit * 2.6f));
            d->AddPolyline(ring.data(), static_cast<int>(ring.size()), Shade(255.f, 245.f, 200.f, 1.f, 1.f), ImDrawFlags_Closed, std::max(1.2f, unit * 0.8f));
        }

        Random random{ 0xA1A1u };
        for (int k = 0; k < 16; k++) {
            float angle = random.next() * 6.2831853f;
            float radius = 10.f + random.next() * 18.f;
            float t = std::fmod(time * (0.35f + random.next() * 0.3f) + random.next(), 1.f);
            auto pos = Vec3_t(body.center.x + std::cos(angle) * radius, body.center.y + std::sin(angle) * radius, body.feet.z + 90.f * t);
            ImVec2 at;
            if (!project(pos, at))
                continue;
            float a = std::sin(t * 3.14159f);
            float size = std::max(2.f, unit * 1.8f);
            auto color = Shade(255.f, 235.f, 160.f, a, 1.f);
            d->AddLine(at - ImVec2(size, 0.f), at + ImVec2(size, 0.f), color, 1.2f);
            d->AddLine(at - ImVec2(0.f, size), at + ImVec2(0.f, size), color, 1.2f);
        }
    }

    // Frost: ice crystals going slowly round the body, cold mist at the feet, snow falling around it
    void DrawFrostAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        float unit = PixelsPerUnit(project, body.center);
        Vec3_t right, up;
        if (unit <= 0.f || !CameraAxes(project, right, up))
            return;
        float bottom, top;
        BodyHeights(body, bottom, top);

        std::vector<ImVec2> ring;
        for (int r = 2; r >= 1; r--)
            if (GroundCircle(project, body.feet + Vec3_t(0.f, 0.f, 2.f), 16.f * r, ring))
                d->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), Shade(180.f, 220.f, 255.f, 0.08f, 1.f));

        Random random{ 0xF205u };
        for (int k = 0; k < 14; k++) {
            float angle = random.next() * 6.2831853f + time * 0.8f;
            float radius = 20.f + random.next() * 8.f;
            float height = bottom + 10.f + random.next() * (top - bottom - 10.f);
            float size = 2.f + random.next() * 1.5f;
            float spin = time * (1.f + random.next() * 2.f);
            auto pos = Vec3_t(body.center.x + std::cos(angle) * radius, body.center.y + std::sin(angle) * radius, height + std::sin(time * 2.f + k) * 3.f);
            float c = std::cos(spin), s = std::sin(spin);
            auto a = right * c + up * s;
            auto b = up * c - right * s;
            ImVec2 q[4];
            if (!project(pos + b * (size * 1.5f), q[0]) || !project(pos + a * size, q[1]) ||
                !project(pos - b * (size * 1.5f), q[2]) || !project(pos - a * size, q[3]))
                continue;
            d->AddQuadFilled(q[0], q[1], q[2], q[3], Shade(170.f, 220.f, 255.f, 0.6f, 1.f));
            d->AddQuad(q[0], q[1], q[2], q[3], Shade(240.f, 250.f, 255.f, 0.9f, 1.f), 1.f);
        }

        for (int k = 0; k < 22; k++) {
            float angle = random.next() * 6.2831853f;
            float radius = random.next() * 32.f;
            float fall = std::fmod(time * (18.f + random.next() * 12.f) + random.next() * 100.f, 100.f);
            auto pos = Vec3_t(body.center.x + std::cos(angle) * radius + std::sin(time + k) * 3.f,
                body.center.y + std::sin(angle) * radius, top + 25.f - fall);
            ImVec2 at;
            if (project(pos, at))
                d->AddCircleFilled(at, std::max(1.f, unit * 0.7f), Shade(255.f, 255.f, 255.f, 0.8f * std::min(1.f, (100.f - fall) / 20.f), 1.f), 6);
        }
    }

    // Rings: two tilted rings of neon light turning around the body, like a planet's
    void DrawRingsAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;
        const float colors[2][3] = { { 80.f, 230.f, 255.f }, { 255.f, 90.f, 220.f } };
        for (int i = 0; i < 2; i++) {
            float tilt = 0.5f + i * 0.35f;
            float spin = time * (0.9f + i * 0.6f) + i * 1.7f;
            auto normal = Vec3_t(std::sin(tilt) * std::cos(spin), std::sin(tilt) * std::sin(spin), std::cos(tilt));
            // Two directions across the normal
            auto u = Vec3_t(normal.y, -normal.x, 0.f);
            float length = u.length();
            u = length > 0.001f ? u * (1.f / length) : Vec3_t(1.f, 0.f, 0.f);
            auto w = Vec3_t(normal.y * u.z - normal.z * u.y, normal.z * u.x - normal.x * u.z, normal.x * u.y - normal.y * u.x);

            std::vector<ImVec2> points;
            for (int k = 0; k < 48; k++) {
                float a = k / 48.f * 6.2831853f;
                ImVec2 at;
                if (!project(body.center + (u * std::cos(a) + w * std::sin(a)) * 30.f, at))
                    break;
                points.push_back(at);
            }
            if (points.size() != 48)
                continue;
            d->AddPolyline(points.data(), 48, Shade(colors[i][0], colors[i][1], colors[i][2], 0.25f, 1.f), ImDrawFlags_Closed, std::max(3.f, unit * 3.f));
            d->AddPolyline(points.data(), 48, Shade(colors[i][0] * 0.5f + 127.f, colors[i][1] * 0.5f + 127.f, colors[i][2] * 0.5f + 127.f, 0.95f, 1.f), ImDrawFlags_Closed, std::max(1.f, unit * 0.8f));

            // A bright bead going round it
            float a = time * 3.f + i * 3.14159f;
            ImVec2 bead;
            if (project(body.center + (u * std::cos(a) + w * std::sin(a)) * 30.f, bead))
                d->AddCircleFilled(bead, std::max(2.f, unit * 1.6f), Shade(255.f, 255.f, 255.f, 1.f, 1.f), 10);
        }
    }

    // Fireflies: little lights wandering around the body, glowing on & off, a short trail each
    void DrawFirefliesAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        constexpr int FLIES = 18;
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;

        Random random{ 0xF1F1u };
        for (int k = 0; k < FLIES; k++) {
            float ax = 0.6f + random.next() * 0.8f, ay = 0.5f + random.next() * 0.8f, az = 0.7f + random.next() * 0.9f;
            float px = random.next() * 6.28f, py = random.next() * 6.28f, pz = random.next() * 6.28f;
            float radius = 20.f + random.next() * 18.f;
            float blink = random.next() * 6.28f;
            auto at_time = [&](float t) {
                return Vec3_t(body.center.x + std::sin(t * ax + px) * radius, body.center.y + std::cos(t * ay + py) * radius,
                    body.center.z + std::sin(t * az + pz) * 30.f);
            };
            float glow = 0.35f + 0.65f * std::pow(0.5f + 0.5f * std::sin(time * 2.5f + blink), 2.f);

            for (int trail = 6; trail >= 1; trail--) {
                ImVec2 at;
                if (project(at_time(time - trail * 0.05f), at))
                    d->AddCircleFilled(at, std::max(1.f, unit * 0.6f), Shade(200.f, 255.f, 120.f, 0.15f * glow * (1.f - trail / 7.f), 1.f), 6);
            }
            ImVec2 at;
            if (!project(at_time(time), at))
                continue;
            d->AddCircleFilled(at, std::max(3.f, unit * 3.5f), Shade(190.f, 255.f, 90.f, 0.15f * glow, 1.f), 12);
            d->AddCircleFilled(at, std::max(1.2f, unit * 1.1f), Shade(240.f, 255.f, 180.f, glow, 1.f), 8);
        }
    }

    // Sakura: pink petals turning around the body as they fall, tumbling
    void DrawSakuraAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        constexpr int PETALS = 26;
        constexpr float FALL = 110.f;
        float unit = PixelsPerUnit(project, body.center);
        Vec3_t right, up;
        if (unit <= 0.f || !CameraAxes(project, right, up))
            return;
        float bottom, top;
        BodyHeights(body, bottom, top);

        Random random{ 0x5A4Bu };
        for (int k = 0; k < PETALS; k++) {
            float start = random.next() * 6.2831853f;
            float radius = 16.f + random.next() * 20.f;
            float speed = 12.f + random.next() * 10.f;
            float offset = random.next() * FALL;
            float tumble = 2.f + random.next() * 3.f;
            float fall = std::fmod(time * speed + offset, FALL);
            float angle = start + time * 0.7f + fall * 0.02f;
            auto pos = Vec3_t(body.center.x + std::cos(angle) * radius, body.center.y + std::sin(angle) * radius, top + 20.f - fall);
            if (pos.z < bottom)
                continue;

            float spin = time * tumble + start;
            float c = std::cos(spin), s = std::sin(spin);
            auto a = (right * c + up * s) * 2.6f;
            auto b = (up * c - right * s) * (1.4f * std::fabs(std::cos(time * tumble * 0.7f + k)) + 0.3f);
            ImVec2 q[4];
            if (!project(pos + a, q[0]) || !project(pos + b, q[1]) || !project(pos - a, q[2]) || !project(pos - b, q[3]))
                continue;
            float a_fade = std::min(1.f, (FALL - fall) / 15.f) * std::min(1.f, fall / 10.f);
            d->AddQuadFilled(q[0], q[1], q[2], q[3], Shade(255.f, 170.f, 200.f, 0.85f * a_fade, 1.f));
            d->AddQuad(q[0], q[1], q[2], q[3], Shade(255.f, 225.f, 235.f, 0.7f * a_fade, 1.f), 1.f);
        }
    }

    // Blades: six blades of light going round the body at the waist, bobbing, a glow trail behind each
    void DrawBladesAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        constexpr int BLADES = 6;
        constexpr float RADIUS = 28.f;
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;
        auto up = Vec3_t(0.f, 0.f, 1.f);
        for (int i = 0; i < BLADES; i++) {
            for (int ghost = 3; ghost >= 0; ghost--) {
                float angle = (time - ghost * 0.04f) * 1.8f + i * 6.2831853f / BLADES;
                auto out = Vec3_t(std::cos(angle), std::sin(angle), 0.f);
                auto along = Vec3_t(-std::sin(angle), std::cos(angle), 0.f);
                auto pos = body.center + out * RADIUS + up * (std::sin(time * 2.f + i) * 6.f);
                ImVec2 tip, tail, left, right;
                if (!project(pos + along * 10.f, tip) || !project(pos - along * 7.f, tail) ||
                    !project(pos + up * 1.8f, left) || !project(pos - up * 1.8f, right))
                    continue;
                float a = ghost == 0 ? 1.f : 0.25f / ghost;
                d->AddQuadFilled(tip, left, tail, right, Shade(120.f, 220.f, 255.f, 0.45f * a, 1.f));
                if (ghost == 0) {
                    d->AddQuad(tip, left, tail, right, Shade(235.f, 250.f, 255.f, 1.f, 1.f), std::max(1.f, unit * 0.5f));
                    d->AddLine(tail, tip, Shade(255.f, 255.f, 255.f, 0.9f, 1.f), std::max(1.f, unit * 0.4f));
                }
            }
        }
    }

    // Matrix: columns of green code raining down around the body, the head of each bright, the rest fading behind it
    void DrawMatrixAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        constexpr int COLUMNS = 16;
        constexpr int LENGTH = 9;
        constexpr float GAP = 7.f;
        constexpr float FALL = 130.f;
        static const char GLYPHS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ@#$%&*+=<>";
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;
        float bottom, top;
        BodyHeights(body, bottom, top);
        float size = std::clamp(unit * 6.f, 8.f, 40.f);
        auto change = static_cast<uint32_t>(time / 0.1f);

        Random random{ 0xC0DEu };
        for (int c = 0; c < COLUMNS; c++) {
            float angle = random.next() * 6.2831853f;
            float radius = 20.f + random.next() * 12.f;
            float speed = 30.f + random.next() * 30.f;
            float offset = random.next() * FALL;
            float head_z = top + 30.f - std::fmod(time * speed + offset, FALL);
            for (int k = 0; k < LENGTH; k++) {
                float z = head_z + k * GAP;
                if (z < bottom || z > top + 30.f)
                    continue;
                ImVec2 at;
                if (!project(Vec3_t(body.center.x + std::cos(angle) * radius, body.center.y + std::sin(angle) * radius, z), at))
                    continue;
                Random glyph{ ((c * 131u + k * 7u) ^ ((change + c * 3u) * 2654435761u)) | 1u };
                char text[2] = { GLYPHS[static_cast<size_t>(glyph.next() * (sizeof(GLYPHS) - 1)) % (sizeof(GLYPHS) - 1)], 0 };
                float a = 1.f - k / static_cast<float>(LENGTH);
                auto color = k == 0 ? Shade(220.f, 255.f, 220.f, 1.f, 1.f) : Shade(40.f, 230.f, 90.f, 0.85f * a, 1.f);
                auto extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, text);
                d->AddText(nullptr, size, at - extent * 0.5f, color, text);
            }
        }
    }

    // Shield: a dome of energy over the body, its lines turning slowly, a brighter wave going up it again & again
    void DrawShieldAura(ImDrawList* d, const KillEffect::Strike& body, float time, const Projector& project) {
        constexpr float RADIUS = 42.f;
        constexpr int RINGS = 7, MERIDIANS = 10, STEPS = 36;
        float unit = PixelsPerUnit(project, body.center);
        if (unit <= 0.f)
            return;
        auto middle = Vec3_t(body.feet.x, body.feet.y, body.feet.z + 30.f);
        float wave = std::fmod(time * 0.6f, 1.f) * 2.f - 1.f;    // Height of the wave, -1 to 1 of the radius
        float width = std::max(1.f, unit * 0.5f);

        auto line = [&](auto point_at, float level) {
            std::vector<ImVec2> points;
            for (int k = 0; k <= STEPS; k++) {
                ImVec2 at;
                if (!project(point_at(k / static_cast<float>(STEPS)), at))
                    return;
                points.push_back(at);
            }
            float near_wave = std::max(0.f, 1.f - std::fabs(level - wave) * 4.f);
            d->AddPolyline(points.data(), static_cast<int>(points.size()), Shade(90.f, 200.f, 255.f, 0.18f + 0.6f * near_wave, 1.f), 0, width * (1.f + near_wave));
        };

        // Rings across, from the ground up (a dome: the half over the ground)
        for (int r = 0; r < RINGS; r++) {
            float level = -0.6f + 1.55f * r / (RINGS - 1);
            level = std::min(level, 0.95f);
            float ring = std::sqrt(std::max(0.f, 1.f - level * level)) * RADIUS;
            line([&](float t) {
                float a = t * 6.2831853f;
                return middle + Vec3_t(std::cos(a) * ring, std::sin(a) * ring, level * RADIUS);
            }, level);
        }
        // Lines from the bottom over the top, turning
        for (int m = 0; m < MERIDIANS; m++) {
            float around = m * 3.14159265f / MERIDIANS + time * 0.25f;
            std::vector<ImVec2> points;
            for (int k = 0; k <= STEPS; k++) {
                float a = -0.6435f + (3.14159265f + 2.f * 0.6435f) * k / STEPS;    // Over the top, down to the ground on both sides
                float level = std::sin(a > 1.5708f ? 3.14159265f - a : a);
                float flat = std::cos(a);
                ImVec2 at;
                if (!project(middle + Vec3_t(std::cos(around) * flat * RADIUS, std::sin(around) * flat * RADIUS, level * RADIUS), at))
                    break;
                points.push_back(at);
            }
            if (points.size() > 1)
                d->AddPolyline(points.data(), static_cast<int>(points.size()), Shade(90.f, 200.f, 255.f, 0.15f, 1.f), 0, width);
        }
    }

    // Trail: a ribbon of light along where the body went the last moment, at the waist, thinner & fainter to its end
    void DrawTrailAura(ImDrawList* d, const std::deque<std::pair<Vec3_t, float>>& path, float time, const Projector& project) {
        constexpr float KEEP = 0.6f;
        if (path.size() < 2)
            return;
        ImVec2 last;
        bool have_last = false;
        for (size_t k = 0; k < path.size(); k++) {
            ImVec2 at;
            if (!project(path[k].first, at)) {
                have_last = false;
                continue;
            }
            float age = time - path[k].second;
            float life = std::clamp(1.f - age / KEEP, 0.f, 1.f);
            float unit = PixelsPerUnit(project, path[k].first);
            if (have_last && life > 0.f) {
                float hue = std::fmod(time * 0.3f + k * 0.02f, 1.f);
                float r = 127.f + 127.f * std::sin(6.2831853f * hue);
                float g = 127.f + 127.f * std::sin(6.2831853f * (hue + 0.33f));
                float b = 127.f + 127.f * std::sin(6.2831853f * (hue + 0.67f));
                d->AddLine(last, at, Shade(r, g, b, 0.3f * life, 1.f), std::max(2.f, unit * 10.f * life));
                d->AddLine(last, at, Shade(r * 0.4f + 153.f, g * 0.4f + 153.f, b * 0.4f + 153.f, 0.9f * life, 1.f), std::max(1.f, unit * 3.f * life));
            }
            last = at;
            have_last = true;
        }
    }
}

// Zeus kill effect: lightning standing where the body was, from high above down to the ground, branches & arcs along
// the ground, a glow under it. Drawn in the world (its size follows the distance), only while the spot is in sight
// The aura of the self effect around our own player, from where its bones are now
void Overlays::RenderSelfAura() {
    // Trail: where the body went the last moment, { place, time }
    static std::deque<std::pair<Vec3_t, float>> path;
    static const auto started = std::chrono::steady_clock::now();
    // Seconds, kept small so the float stays precise
    float time = std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count(), 3600.f);

    auto aura = KillEffect::GetSelfAura();
    if (aura != KillEffect::SelfAura::TRAIL)
        path.clear();
    if (aura == KillEffect::SelfAura::NONE)
        return;

    auto current = Cache::Current();
    const auto& local = current->local;
    if (!local.alive)
        return;

    // Only the bones on the body: agents have some off it (aiming, the weapon) that go far up as it looks up
    KillEffect::Strike body;
    Vec3_t sum{};
    for (const auto& bone : local.bone_list) {
        auto apart = bone.pos - local.pos;
        bool on_body = std::sqrt(apart.x * apart.x + apart.y * apart.y) < 40.f && apart.z > -8.f && apart.z < local.eye.z - local.pos.z + 16.f;
        if (!bone.pos.zero() && on_body) {
            body.bones.push_back(bone.pos);
            sum = sum + bone.pos;
        }
    }
    if (body.bones.empty())
        return;
    body.center = sum * (1.f / body.bones.size());
    body.feet = local.pos;

    auto& io = ImGui::GetIO();
    auto d = ImGui::GetBackgroundDrawList();
    Projector project{ current->game.view_matrix, Vec2_t(io.DisplaySize.x, io.DisplaySize.y) };

    switch (aura) {
    case KillEffect::SelfAura::STORM:       DrawStormAura(d, body, time, project); break;
    case KillEffect::SelfAura::VOID_ORBS:   DrawVoidAura(d, body, time, project); break;
    case KillEffect::SelfAura::HALO:        DrawHaloAura(d, body, time, project); break;
    case KillEffect::SelfAura::FROST:       DrawFrostAura(d, body, time, project); break;
    case KillEffect::SelfAura::RINGS:       DrawRingsAura(d, body, time, project); break;
    case KillEffect::SelfAura::FIREFLIES:   DrawFirefliesAura(d, body, time, project); break;
    case KillEffect::SelfAura::SAKURA:      DrawSakuraAura(d, body, time, project); break;
    case KillEffect::SelfAura::BLADES:      DrawBladesAura(d, body, time, project); break;
    case KillEffect::SelfAura::MATRIX:      DrawMatrixAura(d, body, time, project); break;
    case KillEffect::SelfAura::SHIELD:      DrawShieldAura(d, body, time, project); break;
    case KillEffect::SelfAura::TRAIL: {
        // A new point each time it moved a little, the old ones dropped
        auto waist = local.pos + Vec3_t(0.f, 0.f, 38.f);
        if (path.empty() || (path.back().first - waist).length() > 2.f)
            path.push_back({ waist, time });
        while (!path.empty() && (time - path.front().second > 0.6f || time < path.front().second))
            path.pop_front();
        // Standing still: the end of the ribbon goes on to where it is
        auto drawn = path;
        drawn.push_back({ waist, time });
        DrawTrailAura(d, drawn, time, project);
        break;
    }
    default: break;
    }
}

void Overlays::RenderStrikes() {
    auto strikes = KillEffect::GetStrikes();
    if (strikes.empty())
        return;

    auto& io = ImGui::GetIO();
    auto current = Cache::Current();
    const auto& snapshot = *current;
    const auto& matrix = snapshot.game.view_matrix;
    const auto eye = snapshot.local.eye;
    auto d = ImGui::GetBackgroundDrawList();
    auto now = std::chrono::steady_clock::now();

    constexpr float MAX_HEIGHT = 4000.f;    // Of the main bolt under the open sky, else up to the ceiling
    constexpr float WHITE_FLASH = 0.12f;    // Seconds the screen goes white as a strike lands
    constexpr float WHITE_ALPHA = 0.45f;    // At its brightest, the first strike
    constexpr float FLICKER = 0.06f;        // Seconds a shape lasts
    constexpr float FLASH_EVERY = 0.38f;    // A new strike this often, brightest as it lands

    for (const auto& strike : strikes) {
        float age = std::chrono::duration<float>(now - strike.at).count();
        float life = 1.f - age / KillEffect::Duration(strike.kind);
        if (life <= 0.f)
            continue;

        if (strike.kind != KillEffect::StrikeKind::ZEUS) {
            // Behind a wall (a wallbang kill): fainter
            bool behind = MapCollision::Blocked(eye, strike.center) && MapCollision::Blocked(eye, strike.center + Vec3_t(0.f, 0.f, 30.f));
            float dim = behind ? 0.55f : 1.f;
            Projector project{ matrix, Vec2_t(io.DisplaySize.x, io.DisplaySize.y) };
            switch (strike.kind) {
            case KillEffect::StrikeKind::SINGULARITY:   DrawSingularity(d, strike, age, dim, matrix, io.DisplaySize); break;
            case KillEffect::StrikeKind::SHATTER:       DrawShatter(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::GLITCH:        DrawGlitch(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::ASCEND:        DrawAscend(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::FROST:         DrawFrost(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::SLASH:         DrawSlash(d, strike, age, dim, project, io.DisplaySize); break;
            case KillEffect::StrikeKind::PIXELS:        DrawPixels(d, strike, age, dim, project, eye); break;
            case KillEffect::StrikeKind::METEOR:        DrawMeteor(d, strike, age, dim, project, io.DisplaySize); break;
            case KillEffect::StrikeKind::TORNADO:       DrawTornado(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::TV_OFF:        DrawTvOff(d, strike, age, dim, project); break;
            case KillEffect::StrikeKind::SUPERNOVA:     DrawSupernova(d, strike, age, dim, project, io.DisplaySize); break;
            default: break;
            }
            continue;
        }

        auto feet = strike.feet;
        // From as high as it goes: the ceiling or roof above, the open sky up to MAX_HEIGHT
        float height = MAX_HEIGHT;
        {
            MapCollision::Hit above;
            auto from = feet + Vec3_t(0.f, 0.f, 72.f);
            if (MapCollision::Trace(from, feet + Vec3_t(0.f, 0.f, MAX_HEIGHT), above))
                height = std::max(120.f, 72.f + (MAX_HEIGHT - 72.f) * above.fraction);
        }
        auto top = feet + Vec3_t(0.f, 0.f, height);

        // Behind a wall (a wallbang kill): still drawn, fainter & without the white flash. Hidden it never showed for
        // those (the bolt high up is checked too, it can stand over a low wall)
        bool hidden = MapCollision::Blocked(eye, feet + Vec3_t(0.f, 0.f, 40.f)) && MapCollision::Blocked(eye, feet + Vec3_t(0.f, 0.f, 150.f)) &&
            MapCollision::Blocked(eye, feet + Vec3_t(0.f, 0.f, std::min(height * 0.6f, 600.f)));

        // Strikes one after another, each a bright flash fading
        float since_flash = std::fmod(age, FLASH_EVERY) / FLASH_EVERY;
        float flash = (1.f - since_flash) * (1.f - since_flash);
        float alpha = std::clamp(life * 1.5f, 0.f, 1.f) * (0.7f + 0.3f * flash) * (hidden ? 0.55f : 1.f);

        float distance = std::max(1.f, (feet - eye).length());
        float scale = std::clamp(600.f / distance, 0.6f, 3.f);

        // The screen white for a moment as each strike lands, the first one the brightest, less from far away
        float in_flash = std::fmod(age, FLASH_EVERY);
        if (!hidden && in_flash < WHITE_FLASH) {
            float fade = 1.f - in_flash / WHITE_FLASH;
            float strength = WHITE_ALPHA * fade * fade * (age < FLASH_EVERY ? 1.f : 0.55f) * std::clamp(scale / 1.5f, 0.4f, 1.f);
            d->AddRectFilled(ImVec2(0.f, 0.f), io.DisplaySize, IM_COL32(235, 242, 255, static_cast<int>(255 * strength * life)));
        }

        Random random{ strike.seed ^ (static_cast<uint32_t>(age / FLICKER) * 2654435761u) };
        if (!random.state)
            random.state = 1;

        auto project = [&](const Vec3_t& world, ImVec2& out) {
            Vec2_t screen;
            if (!matrix.wts(world, io.DisplaySize, screen, false))
                return false;
            out = ImVec2(screen.x, screen.y);
            return true;
        };

        // A wide glow, a softer middle, a thick white core (added light: brighter where they cross)
        auto draw = [&](const std::vector<Vec3_t>& points, float width) {
            std::vector<ImVec2> screen;
            for (const auto& point : points) {
                ImVec2 at;
                if (!project(point, at))
                    return;
                screen.push_back(at);
            }
            int count = static_cast<int>(screen.size());
            d->AddPolyline(screen.data(), count, IM_COL32(70, 140, 255, static_cast<int>(55 * alpha)), 0, width * 16.f * scale);
            d->AddPolyline(screen.data(), count, IM_COL32(100, 170, 255, static_cast<int>(110 * alpha)), 0, width * 9.f * scale);
            d->AddPolyline(screen.data(), count, IM_COL32(150, 210, 255, static_cast<int>(200 * alpha)), 0, width * 4.5f * scale);
            d->AddPolyline(screen.data(), count, IM_COL32(245, 250, 255, static_cast<int>(255 * alpha)), 0, width * 2.2f * scale);
        };

        // Light on the ground
        ImVec2 ground;
        if (project(feet, ground)) {
            float radius = 90.f * scale * (0.8f + 0.5f * flash);
            for (int ring = 5; ring >= 1; ring--)
                d->AddCircleFilled(ground, radius * ring / 5.f, IM_COL32(120, 190, 255, static_cast<int>(40 * alpha)), 32);
        }

        // Main bolt from the sky, two thinner ones beside it
        // More pieces the longer it is, about one each 30 units
        int pieces = std::clamp(static_cast<int>(height / 30.f), 14, 80);
        auto trunk = Bolt(top, feet, pieces, 34.f, random);
        draw(trunk, 1.f);
        for (int side = 0; side < 2; side++) {
            auto start = top + Vec3_t(random.signed_next() * 60.f, random.signed_next() * 60.f, -random.next() * height * 0.25f);
            draw(Bolt(start, feet + Vec3_t(random.signed_next() * 12.f, random.signed_next() * 12.f, 0.f), std::max(10, pieces * 3 / 4), 28.f, random), 0.5f);
        }

        // Branches off the main bolt, down & out
        for (int branch = 0; branch < 6; branch++) {
            const auto& from = trunk[1 + static_cast<size_t>(random.next() * (trunk.size() - 3))];
            auto to = from + Vec3_t(random.signed_next() * 70.f, random.signed_next() * 70.f, -30.f - random.next() * 60.f);
            draw(Bolt(from, to, 5, 14.f, random), 0.45f);
        }

        // Arcs along the ground around where it lands
        for (int arc = 0; arc < 6; arc++) {
            float angle = random.next() * 6.2831853f;
            float reach = 40.f + random.next() * 50.f;
            auto to = feet + Vec3_t(std::cos(angle) * reach, std::sin(angle) * reach, random.next() * 10.f);
            draw(Bolt(feet + Vec3_t(0.f, 0.f, 4.f), to, 5, 10.f, random), 0.4f);
        }
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
            // Only a ballot starts a vote: the counts the server sends when we join are of no vote
            bool no_vote = !last || (last->ended && current.issue < 0);
            if (no_vote && !event.cast)
                continue;

            last_event = now;

            // Ballots late to a vote we already called decided still belong to it, while the game shows it
            if (no_vote) {
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
