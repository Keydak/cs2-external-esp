// Documents\Emotes\emotes.ini (written with the defaults the first time). The Emote Pack is Documents\Emotes of
// whoever runs it: Emotes\csgo_addons (the dance model), Emotes\music, Emotes\icons.
#pragma once
#include <windows.h>
#include <filesystem>

namespace emotes {

struct Config {
    int wheelKey = 'B';          // virtual key: tap to open the wheel, or hold and let go on an emote
    int unloadKey = VK_END;      // unloads the DLL
    bool music = true;
    float volume = 0.7f;         // 0-1
    int slots[6] = {0, 1, 2, 3, 4, 5};  // Favourites page: clip index, 1000+i = Fortnite dance i, -1 empty
};

// Reads emotes.ini (creating it with the defaults), applies it to the features and points them at the pack.
void LoadConfig(HMODULE self);
const Config& GetConfig();
std::filesystem::path DataDir();  // <Documents>\Emotes

}  // namespace emotes
