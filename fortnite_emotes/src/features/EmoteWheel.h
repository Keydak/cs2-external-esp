// The emote wheel: Fortnite dances (FortniteDances) and CS2's own celebration clips, played on your player.
// The clips are client-side pose edits in CSkeletonInstance::ApplyAnimGraphPose.
#pragma once
#include <atomic>
#include <cstdint>
struct ImDrawList;
namespace emotes {
using HookInstaller = bool (*)(const char* name, uintptr_t target, void* detour, void** original);
struct EmoteWheelConfig {
    std::atomic<bool>  emoteEnabled{false};
    std::atomic<int>   emoteKey{0};                  // virtual key, 0 = none
    std::atomic<int>   emoteSlots[6]{0, 1, 2, 3, 4, 5};  // Favourites page: clip index, 1000+i = Fortnite dance i, -1 = empty
};
EmoteWheelConfig& GetEmoteWheelConfig();
void EmoteWheelInstallHooks(HookInstaller install);        // ApplyAnimGraphPose
void EmoteWheelFrame();                                    // game thread, after FSN stage 12
void EmoteWheelLevelShutdown();                            // HkLevelShutdown, before the original
void EmoteWheelRelease();                                  // RemoveGameHooks step 1 (waits <= 100 ms)
void EmoteWheelDraw(ImDrawList* dl, float scroll);         // render thread, every frame; scroll = wheel notches
bool EmoteWheelWantsCursor();                              // Hooks.cpp: cursor free while the wheel is open
void EmoteWheelCancel();                                   // WndProc: Esc closes the wheel
void EmoteWheelReleaseTextures();                          // Hooks shutdown, after the last Present: the icons
void EmoteWheelOnGameEvent(const char* name, int userid);  // any thread
}  // namespace emotes
