#include "EmoteWheel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numbers>

#include <fstream>

#include "core/features/Emotes.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Movement.hpp"
#include "gui/frontend/images/ImageCache.hpp"

namespace {
    constexpr float PI = std::numbers::pi_v<float>;

    constexpr auto TAP = std::chrono::milliseconds(250);  // Let go sooner on nothing: it stays open
    constexpr float DEADZONE = 18.f;        // Pixels of the cursor from the middle before a slot is pointed at
    constexpr float POINTER_MAX = 120.f;    // Further is the same direction
    constexpr float APPEAR_TIME = 0.12f;    // Seconds to open
    constexpr int ICONS_PER_FRAME = 2;      // Made into textures, the first time a page shows

    ImU32 Accent(float alpha) {
        auto accent = cfg::settings::accent;
        return ImGui::GetColorU32(ImVec4(accent.r, accent.g, accent.b, alpha));
    }

    // The colors of Fortnite's rarities, the accent for the game's own & unknown ones
    ImU32 Rarity(int rarity, float alpha) {
        static constexpr ImU32 COLORS[] = {
            0,                              // Unknown
            IM_COL32(178, 178, 178, 255),   // Common
            IM_COL32(105, 187, 30, 255),    // Uncommon
            IM_COL32(44, 168, 255, 255),    // Rare
            IM_COL32(189, 63, 250, 255),    // Epic
            IM_COL32(222, 124, 50, 255),    // Legendary
            IM_COL32(197, 51, 52, 255),     // Marvel
            IM_COL32(54, 183, 183, 255),    // Icon Series
            IM_COL32(92, 76, 214, 255),     // Gaming Legends
            IM_COL32(84, 117, 199, 255),    // DC
            IM_COL32(231, 196, 19, 255),    // Star Wars
        };
        if (rarity <= 0 || rarity >= static_cast<int>(std::size(COLORS)))
            return Accent(alpha);
        auto color = COLORS[rarity];
        auto a = static_cast<ImU32>(255.f * std::clamp(alpha, 0.f, 1.f) + 0.5f);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    const char* KindName(int kind) {
        switch (static_cast<Emotes::Kind>(kind)) {
        case Emotes::Kind::Dance: return "DANCES";
        case Emotes::Kind::Emote: return "EMOTES";
        default: return "CS2";
        }
    }

    ImU32 Mix(ImU32 a, ImU32 b, float t) {
        auto channel = [&](int shift) {
            float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<ImU32>(x + (y - x) * t + 0.5f) << shift;
        };
        return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
    }

    ImU32 Fade(ImU32 color, float alpha) {
        auto a = static_cast<ImU32>(((color & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT) * std::clamp(alpha, 0.f, 1.f) + 0.5f);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    // Ring segment around the angle mid, half to each side, a gap of the same width all along to its neighbours.
    // Darker inside than outside
    void Wedge(ImDrawList* d, ImVec2 c, float r0, float r1, float mid, float half, float gap, ImU32 inner, ImU32 outer,
        ImU32 line, float line_width) {
        constexpr int N = 24;
        float gap_in = std::asin(std::min(0.9f, gap * 0.5f / r0)), gap_out = std::asin(std::min(0.9f, gap * 0.5f / r1));

        ImVec2 in[N + 1], out[N + 1];
        for (int j = 0; j <= N; j++) {
            float t = static_cast<float>(j) / N;
            float a_in = mid - half + gap_in + 2.f * (half - gap_in) * t;
            float a_out = mid - half + gap_out + 2.f * (half - gap_out) * t;
            in[j] = ImVec2(c.x + std::cos(a_in) * r0, c.y + std::sin(a_in) * r0);
            out[j] = ImVec2(c.x + std::cos(a_out) * r1, c.y + std::sin(a_out) * r1);
        }

        auto uv = ImGui::GetFontTexUvWhitePixel();
        d->PrimReserve(N * 6, (N + 1) * 2);
        auto base = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
        for (int j = 0; j <= N; j++) {
            d->PrimWriteVtx(in[j], uv, inner);
            d->PrimWriteVtx(out[j], uv, outer);
        }
        for (int j = 0; j < N; j++) {
            auto a = static_cast<ImDrawIdx>(base + 2 * j);
            d->PrimWriteIdx(a); d->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1)); d->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
            d->PrimWriteIdx(a); d->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3)); d->PrimWriteIdx(static_cast<ImDrawIdx>(a + 2));
        }

        for (int j = 0; j <= N; j++)
            d->PathLineTo(out[j]);
        for (int j = N; j >= 0; j--)
            d->PathLineTo(in[j]);
        d->PathStroke(line, ImDrawFlags_Closed, line_width);
    }

    // Text centred on at, on two lines split at the space nearest its middle when it is wider than max_width
    void Label(ImDrawList* d, ImFont* font, float size, ImVec2 at, float max_width, ImU32 color, const char* text) {
        float width = font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
        if (width <= max_width) {
            d->AddText(font, size, ImVec2(std::floor(at.x - width * 0.5f), std::floor(at.y - size * 0.5f)), color, text);
            return;
        }

        const char* split = nullptr;
        size_t length = std::strlen(text);
        for (const char* space = std::strchr(text, ' '); space; space = std::strchr(space + 1, ' '))
            if (!split || std::abs(static_cast<long>(space - text) - static_cast<long>(length / 2))
                < std::abs(static_cast<long>(split - text) - static_cast<long>(length / 2)))
                split = space;

        if (!split) {
            size = size * max_width / width;
            width = font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
            d->AddText(font, size, ImVec2(std::floor(at.x - width * 0.5f), std::floor(at.y - size * 0.5f)), color, text);
            return;
        }

        float first = font->CalcTextSizeA(size, FLT_MAX, 0.f, text, split).x;
        float second = font->CalcTextSizeA(size, FLT_MAX, 0.f, split + 1).x;
        if (std::max(first, second) > max_width) {
            float scale = max_width / std::max(first, second);
            size *= scale, first *= scale, second *= scale;
        }

        float top = at.y - size;
        d->AddText(font, size, ImVec2(std::floor(at.x - first * 0.5f), std::floor(top)), color, text, split);
        d->AddText(font, size, ImVec2(std::floor(at.x - second * 0.5f), std::floor(top + size)), color, split + 1);
    }
}

void EmoteWheel::Render() {
    GetInstance().RenderImpl();
}

void EmoteWheel::Open() {
    this->open = true;
    this->toggle = false;
    this->opened_at = std::chrono::steady_clock::now();
    this->appear = 0.f;
    this->pointer_x = this->pointer_y = 0.f;
    std::fill(std::begin(this->pop), std::end(this->pop), 0.f);

    Freecam::TakeLentMouse();   // Nothing from before it opened
    Freecam::LendMouse(true);
}

void EmoteWheel::Close() {
    this->open = false;
    Freecam::LendMouse(false);
}

void EmoteWheel::RenderImpl() {
    int key = cfg::view::emote_key;
    bool usable = cfg::view::emotes && Emotes::IsAvailable() && Movement::IsPlaying() && key > 0 && key < 0xFF;
    bool down = usable && (GetAsyncKeyState(key) & 0x8000);
    bool pressed = down && !this->key_was_down, released = !down && this->key_was_down;
    this->key_was_down = down;

    if (!this->open) {
        if (!pressed)
            return;

        // The key while one plays stops it
        if (Emotes::IsPlaying()) {
            Emotes::Stop();
            return;
        }

        if (!Emotes::CanPlay())
            return;
        Open();
    }

    // Right click, the key again after a tap, or not playing anymore (Escape, the menu, dead) closes it
    auto mouse = Freecam::TakeLentMouse();
    if (!usable || !Emotes::CanPlay() || mouse.right > 0 || (this->toggle && pressed)) {
        Close();
        return;
    }
    Freecam::LendMouse(true);

    // The mouse points from the middle, a direction: its length kept short so turning back is quick
    this->pointer_x += static_cast<float>(mouse.dx);
    this->pointer_y += static_cast<float>(mouse.dy);
    float length = std::sqrt(this->pointer_x * this->pointer_x + this->pointer_y * this->pointer_y);
    if (length > POINTER_MAX) {
        this->pointer_x *= POINTER_MAX / length;
        this->pointer_y *= POINTER_MAX / length;
        length = POINTER_MAX;
    }

    // Slot 0 at the top, then clockwise
    int hot = -1;
    if (length > DEADZONE) {
        float step = 2.f * PI / SLOTS;
        int slot = static_cast<int>(std::floor((std::atan2(this->pointer_y, this->pointer_x) + PI * 0.5f + step * 0.5f) / step));
        hot = ((slot % SLOTS) + SLOTS) % SLOTS;
    }

    // Pages of one kind each: the dances, the emotes, the game's own
    const auto& entries = Emotes::GetEntries(Emotes::CanDance());
    int count = static_cast<int>(entries.size());
    std::vector<Page> pages;
    for (int i = 0; i < count;) {
        int end = i;
        while (end < count && entries[end].kind == entries[i].kind)
            end++;
        int of = (end - i + SLOTS - 1) / SLOTS;
        for (int k = 0; k < of; k++)
            pages.push_back({ static_cast<int>(entries[i].kind), i + k * SLOTS, std::min(SLOTS, end - i - k * SLOTS), k + 1, of });
        i = end;
    }
    if (pages.empty()) {
        Close();
        return;
    }

    // The mouse wheel down is the next page
    int page_count = static_cast<int>(pages.size());
    if (mouse.wheel)
        this->page = ((this->page - mouse.wheel) % page_count + page_count) % page_count;
    this->page = std::clamp(this->page, 0, page_count - 1);

    const auto& shown = pages[this->page];
    int values[SLOTS];
    for (int k = 0; k < SLOTS; k++)
        values[k] = k < shown.count ? shown.first + k : -1;
    int picked = hot >= 0 && values[hot] >= 0 ? entries[values[hot]].id : -1;

    if (mouse.left > 0 && picked >= 0) {
        Emotes::Play(picked);
        Close();
        return;
    }

    if (released && !this->toggle) {
        if (picked >= 0) {
            Emotes::Play(picked);
            Close();
            return;
        }

        // Held & let go on nothing closes it, a tap keeps it open
        if (std::chrono::steady_clock::now() - this->opened_at >= TAP) {
            Close();
            return;
        }
        this->toggle = true;
    }

    Draw(hot, values, pages);
}

ImTextureID EmoteWheel::Icon(const char* sequence) {
    if (!sequence)
        return 0;

    // Another pack: its own icons
    auto pack = Emotes::GetPack();
    if (pack != this->icon_pack) {
        this->icon_pack = pack;
        this->icons.clear();
    }
    if (pack.empty())
        return 0;

    // Files of it downloaded since: the icons it had none for looked for again
    int missing = Emotes::GetPackMissing();
    if (missing != this->icon_missing) {
        this->icon_missing = missing;
        std::erase_if(this->icons, [](const auto& icon) { return !icon.second; });
    }

    if (auto it = this->icons.find(sequence); it != this->icons.end())
        return it->second;
    if (this->icons_made >= ICONS_PER_FRAME)
        return 0;
    this->icons_made++;

    std::ifstream file(pack / "icons" / (std::string(sequence) + ".png"), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto texture = bytes.empty() ? ImTextureID{} : ImageCache::FromMemory(bytes.data(), bytes.size());
    this->icons[sequence] = texture;
    return texture;
}

void EmoteWheel::Draw(int hot, const int (&values)[SLOTS], const std::vector<Page>& pages) {
    auto d = ImGui::GetBackgroundDrawList();
    auto& io = ImGui::GetIO();
    auto font = ImGui::GetFont();
    const auto& entries = Emotes::GetEntries(Emotes::CanDance());
    const auto& shown = pages[this->page];
    this->icons_made = 0;

    float dt = std::clamp(io.DeltaTime, 0.f, 0.1f);
    this->appear = std::min(1.f, this->appear + dt / APPEAR_TIME);
    float a = 1.f - (1.f - this->appear) * (1.f - this->appear);

    // The size of a 1080p screen, it grows into place
    float s = std::max(0.5f, io.DisplaySize.y / 1080.f);
    float grow = 0.92f + 0.08f * a;
    ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    float outer = 230.f * s * grow, inner = 92.f * s * grow, gap = 5.f * s;
    float step = 2.f * PI / SLOTS;

    // The game behind it a little darker
    d->AddRectFilled(ImVec2(0.f, 0.f), io.DisplaySize, Fade(IM_COL32(0, 0, 0, 90), a));

    for (int k = 0; k < SLOTS; k++) {
        bool filled = values[k] >= 0;
        this->pop[k] += ((k == hot && filled ? 1.f : 0.f) - this->pop[k]) * std::min(1.f, dt * 16.f);
        float p = this->pop[k];

        float mid = -PI * 0.5f + k * step;
        ImVec2 dir(std::cos(mid), std::sin(mid));
        ImVec2 at(c.x + dir.x * 8.f * s * p, c.y + dir.y * 8.f * s * p);   // The one pointed at steps out

        if (!filled) {
            Wedge(d, at, inner, outer, mid, step * 0.5f, gap, Fade(IM_COL32(10, 10, 12, 120), a), Fade(IM_COL32(16, 16, 19, 140), a),
                Fade(IM_COL32(255, 255, 255, 14), a), 1.f * s);
            continue;
        }

        // A dance of Fortnite in the color of its rarity, the game's own in the accent
        const auto& entry = entries[values[k]];
        bool fortnite = entry.kind != Emotes::Kind::Game;
        ImU32 tint = fortnite ? Rarity(entry.rarity, 1.f) : Accent(1.f);
        ImU32 dark_in = IM_COL32(12, 12, 14, 215), dark_out = IM_COL32(22, 22, 26, 225);
        float idle = fortnite ? 0.22f : 0.f;
        Wedge(d, at, inner, outer, mid, step * 0.5f, gap,
            Fade(Mix(dark_in, tint, (p * 0.45f) * 0.55f), a), Fade(Mix(dark_out, tint, idle + (0.7f - idle) * p * 0.85f), a),
            Fade(Mix(fortnite ? Fade(tint, 0.35f) : IM_COL32(255, 255, 255, 28), tint, p), a), (1.f + 1.f * p) * s);

        // Its icon above its name when the pack has one
        float radius = (inner + outer) * 0.5f;
        ImU32 text_color = Fade(Mix(IM_COL32(215, 215, 220, 255), IM_COL32(255, 255, 255, 255), p), a);
        if (auto icon = Icon(entry.sequence)) {
            float size = (outer - inner) * (0.48f + 0.04f * p);
            ImVec2 middle(at.x + dir.x * (radius - 8.f * s), at.y + dir.y * (radius - 8.f * s));
            ImVec2 top(middle.x - size * 0.5f, middle.y - size * 0.62f);
            d->AddImage(icon, top, ImVec2(top.x + size, top.y + size), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), Fade(IM_COL32_WHITE, a));
            Label(d, font, (12.f + 1.f * p) * s, ImVec2(middle.x, top.y + size + 9.f * s), (outer - inner) * 0.86f, text_color, entry.name);
        }
        else {
            ImVec2 text(at.x + dir.x * radius, at.y + dir.y * radius);
            Label(d, font, (15.f + 1.5f * p) * s, text, (outer - inner) * 0.86f, text_color, entry.name);
        }
    }

    // The middle: the one pointed at, else the page
    float middle = inner - gap;
    d->AddCircleFilled(c, middle, Fade(IM_COL32(10, 10, 12, 230), a), 64);
    d->AddCircle(c, middle, Fade(IM_COL32(255, 255, 255, 30), a), 64, 1.5f * s);

    int picked = hot >= 0 ? values[hot] : -1;
    if (picked >= 0) {
        const auto& entry = entries[picked];
        Label(d, font, 19.f * s, ImVec2(c.x, c.y - 7.f * s), middle * 1.6f, Fade(IM_COL32(255, 255, 255, 255), a), entry.name);
        Label(d, font, 12.f * s, ImVec2(c.x, c.y + 17.f * s), middle * 1.6f,
            Fade(entry.kind != Emotes::Kind::Game ? Rarity(entry.rarity, 1.f) : Accent(1.f), a), this->toggle ? "CLICK TO PLAY" : "LET GO TO PLAY");
    }
    else {
        Label(d, font, 19.f * s, ImVec2(c.x, c.y - 7.f * s), middle * 1.6f, Fade(IM_COL32(255, 255, 255, 255), a), KindName(shown.kind));
        char text[32];
        std::snprintf(text, sizeof(text), "PAGE %d / %d", shown.number, shown.of);
        Label(d, font, 12.f * s, ImVec2(c.x, c.y + 17.f * s), middle * 1.6f, Fade(IM_COL32(150, 150, 160, 255), a), text);
    }

    // Where the mouse points, a notch on the middle
    float length = std::sqrt(this->pointer_x * this->pointer_x + this->pointer_y * this->pointer_y);
    if (length > DEADZONE * 0.5f) {
        ImVec2 dir(this->pointer_x / length, this->pointer_y / length), side(-dir.y, dir.x);
        float tip = middle - 3.f * s, back = tip - 11.f * s;
        d->AddTriangleFilled(ImVec2(c.x + dir.x * tip, c.y + dir.y * tip),
            ImVec2(c.x + dir.x * back + side.x * 7.f * s, c.y + dir.y * back + side.y * 7.f * s),
            ImVec2(c.x + dir.x * back - side.x * 7.f * s, c.y + dir.y * back - side.y * 7.f * s),
            Fade(picked >= 0 ? Accent(1.f) : IM_COL32(255, 255, 255, 140), a));
    }

    // The pages under it, those of a kind together, then what the keys do
    float below = c.y + outer + 22.f * s;
    int page_count = static_cast<int>(pages.size());
    if (page_count > 1) {
        float spacing = 12.f * s, group = 10.f * s, width = 0.f;
        for (int i = 1; i < page_count; i++)
            width += spacing + (pages[i].kind != pages[i - 1].kind ? group : 0.f);
        float x = c.x - width * 0.5f;
        for (int i = 0; i < page_count; i++) {
            if (i > 0)
                x += spacing + (pages[i].kind != pages[i - 1].kind ? group : 0.f);
            d->AddCircleFilled(ImVec2(x, below), (i == this->page ? 3.5f : 2.5f) * s,
                Fade(i == this->page ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 90), a), 12);
        }
        below += 20.f * s;
    }

    const char* hint = this->toggle
        ? "Mouse to point  \xC2\xB7  Click to play  \xC2\xB7  Scroll for more  \xC2\xB7  Right click to close"
        : "Mouse to point  \xC2\xB7  Let go to play  \xC2\xB7  Scroll for more";
    float size = 13.f * s;
    float width = font->CalcTextSizeA(size, FLT_MAX, 0.f, hint).x;
    d->AddText(font, size, ImVec2(std::floor(c.x - width * 0.5f), std::floor(below)), Fade(IM_COL32(200, 200, 205, 210), a), hint);
}
