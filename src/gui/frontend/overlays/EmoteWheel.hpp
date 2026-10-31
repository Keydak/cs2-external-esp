#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// The wheel of the emotes over the game, held like the radio wheel of CS2: hold its key, move the mouse toward an
// emote & let go to play it. Tapped it stays open: a click plays the one pointed at, the mouse wheel turns the pages,
// right click or the key again closes it. The game gets none of the mouse meanwhile (Freecam lends it to us).
// The pages: the Fortnite dances & emotes of the pack once loaded (their icons & the colors of their rarity), then the
// game's own
class EmoteWheel {
public:
    ~EmoteWheel()                            = default;
    EmoteWheel(const EmoteWheel&)            = delete;
    EmoteWheel(EmoteWheel&&)                 = delete;
    EmoteWheel& operator=(const EmoteWheel&) = delete;
    EmoteWheel& operator=(EmoteWheel&&)      = delete;

    static void Render();
private:
    EmoteWheel() {};

    static EmoteWheel& GetInstance()
    {
        static EmoteWheel i{};
        return i;
    }

    static constexpr int SLOTS = 8;

    // A page of the wheel: entries of one kind, from first
    struct Page {
        int kind;
        int first, count;
        int number, of;         // Of the pages of its kind
    };

    void RenderImpl();
    void Open();
    void Close();
    // hot: the slot pointed at, -1 for none. values: the entry of each slot, -1 empty
    void Draw(int hot, const int (&values)[SLOTS], const std::vector<Page>& pages);
    // The icon of a dance from the pack, made into a texture the first time it shows (two a frame at most). 0 while
    // it is not loaded, or when there is none
    ImTextureID Icon(const char* sequence);

private:
    bool open = false;
    bool toggle = false;        // Tapped: open until a click, right click or the key again
    bool key_was_down = false;
    std::chrono::steady_clock::time_point opened_at{};

    float appear = 0.f;         // 0 to 1 as it opens
    float pop[SLOTS]{};         // 0 to 1, the slot pointed at steps out
    float pointer_x = 0.f, pointer_y = 0.f; // Where the mouse moved from the middle, pixels of the cursor
    int page = 0;               // Kept from one opening to the next

    std::unordered_map<std::string, ImTextureID> icons;    // 0 for one that has none
    std::filesystem::path icon_pack;
    int icon_missing = -1;      // Files of the pack not there when the icons were looked for
    int icons_made = 0;         // This frame
};
