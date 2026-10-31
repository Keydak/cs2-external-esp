// Third person through a detour on CInput::CAM_Think: while `want` is on (an emote plays) the camera is held in third
// person at cam_idealdist; when it goes off the view goes back to first person once.
#pragma once
#include <atomic>
#include <cstdint>

namespace emotes {

using HookInstaller = bool (*)(const char* name, uintptr_t target, void* detour, void** original);

struct ThirdpersonConfig {
    std::atomic<bool> thirdperson{false};
};
ThirdpersonConfig& GetThirdpersonConfig();

void ThirdpersonInstallHooks(HookInstaller install);  // InstallGameHooks
bool ThirdpersonAvailable();  // any thread; false when the camera function was not found or hooked
bool InThirdperson();         // any thread: the view is third person right now
void ThirdpersonRelease();    // RemoveGameHooks step 1: back to first person if we switched

}  // namespace emotes
