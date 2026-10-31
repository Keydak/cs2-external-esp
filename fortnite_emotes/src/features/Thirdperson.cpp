#include "features/Thirdperson.h"
#include "core/Journal.h"
#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include <windows.h>

namespace emotes {
namespace {

ThirdpersonConfig g_config;
constexpr int kConVarValue = 0x58;

// client sub_180B61650: void CAM_Think(CCSGOInput* input, int slot), called once a frame from the client frame with
// slot 0 (per-slot block stride 0x928). m_nCamCommand [input+0x6A8]: 1 = CAM_ToThirdPerson (sub_180B62520: if
// m_fCameraInThirdPerson [input+0x229] is 0, sets it and puts the camera at the view angles, distance 30), 2 = back to
// first person. Both offsets are read from this one: prologue, the m_nCamCommand switch and the to-first-person
// branch. Camera offset: pitch/yaw [input+0x230/0x234], distance 0x238. 1 hit in IDA (0x180B61650) and on disk.
constexpr const char* kCamThinkPattern =
    "40 55 53 56 57 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 63 DA 48 8B F9 48 69 F3 ? ? ? ? 4C 8B FB 48 83 BC "
    "0E ? ? ? ? 00 74 ? 8B D3 48 81 C4 ? ? ? ? 41 5F 5F 5E 5B 5D E9 ? ? ? ? 8B 8C 0E ? ? ? ? 4C 89 B4 24 ? ? ? ? 45 "
    "33 F6 83 E9 01 74 ? 83 F9 01 75 ? 44 88 B4 3E ? ? ? ? 44 89 B4 3E";
constexpr int kCmdOffAt = 0x48, kThirdOffAt = 0x65, kCmdOff2At = 0x6D;  // disp32s: mov ecx,[..]; mov [..],r14b; mov [..],r14d
constexpr int32_t kCamToThird = 1, kCamToFirst = 2;

using CamThinkFn = void(__fastcall*)(void* input, int slot);
CamThinkFn g_origCamThink = nullptr;
int32_t g_camCmdOff = 0, g_camThirdOff = 0;  // set before the hook is enabled
std::atomic<bool> g_camHooked{false};
std::atomic<bool> g_camForcing{false};    // CAM_Think (main thread) is holding third person
std::atomic<bool> g_camReleasing{false};

uintptr_t InputPtr() { return reinterpret_cast<uintptr_t>(sdk::Data().ccsgoInput); }

void __fastcall HkCamThink(void* input, int slot) {
    const auto in = reinterpret_cast<uintptr_t>(input);
    const bool want = slot == 0 && in && !g_camReleasing.load() && g_config.thirdperson.load();
    if (want) {  // m_fCameraInThirdPerson = false; m_nCamCommand = 1
        sdk::Write<uint8_t>(in + g_camThirdOff, 0);
        sdk::Write<int32_t>(in + g_camCmdOff, kCamToThird);
        g_camForcing = true;
    } else if (slot == 0 && in && g_camForcing.exchange(false)) {
        sdk::Write<int32_t>(in + g_camCmdOff, kCamToFirst);  // once; the original switches back and clears it
    }
    g_origCamThink(input, slot);
    // With sv_cheats 0 the original drops third person right after switching to it (client 0x180B616DB). Keep the
    // flag; the camera angles it just set stay.
    if (!want) return;
    if (!sdk::Read<uint8_t>(in + g_camThirdOff)) sdk::Write<uint8_t>(in + g_camThirdOff, 1);
    // CAM_ToThirdPerson (sub_180B62520) re-seeds the distance to 30 every frame (flag+0xF = the v4+16 store), and with
    // sv_cheats 0 nothing moves it out again (sv_cheats 1 only lerps one step): push it to cam_idealdist.
    // ponytail: this skips the game's collision pull-in; the camera can clip into walls behind you
    static void* s_idealDist = sdk::FindConVar("cam_idealdist");
    const float ideal = s_idealDist ? sdk::Read<float>(reinterpret_cast<uintptr_t>(s_idealDist) + kConVarValue) : 0.0f;
    const float dist = ideal >= 30.0f && ideal <= 400.0f ? ideal : 120.0f;
    if (sdk::Read<float>(in + g_camThirdOff + 0xF) < dist) sdk::Write<float>(in + g_camThirdOff + 0xF, dist);
}

}  // namespace

ThirdpersonConfig& GetThirdpersonConfig() { return g_config; }

bool ThirdpersonAvailable() { return g_camHooked.load(); }

bool InThirdperson() {
    const uintptr_t input = InputPtr();
    return g_camHooked.load() && input && sdk::Read<uint8_t>(input + g_camThirdOff) != 0;
}

void ThirdpersonInstallHooks(HookInstaller install) {
    if (g_camHooked.load() || g_camThirdOff) return;
    const uintptr_t hit = sdk::PatternScan("client.dll", kCamThinkPattern);
    const int32_t cmd = hit ? sdk::Read<int32_t>(hit + kCmdOffAt) : 0, third = hit ? sdk::Read<int32_t>(hit + kThirdOffAt) : 0;
    g_camThirdOff = -1;  // tried once
    if (!hit || cmd <= 0 || cmd >= 0x928 || cmd != sdk::Read<int32_t>(hit + kCmdOff2At) || third <= 0 || third >= 0x928) {
        journal().warn("camera", "third person and the emote wheel unavailable: camera function not found");
        return;
    }
    g_camCmdOff = cmd;
    g_camThirdOff = third;
    if (!install || !install("CAM_Think", hit, reinterpret_cast<void*>(&HkCamThink), reinterpret_cast<void**>(&g_origCamThink))) {
        journal().warn("camera", "third person and the emote wheel unavailable: camera hook failed");
        return;
    }
    g_camHooked = true;
}

void ThirdpersonRelease() {
    g_camReleasing = true;  // the next CAM_Think switches back to first person once (m_nCamCommand 2)
    for (int i = 0; i < 40 && g_camForcing.load(); ++i) Sleep(5);
    const uintptr_t input = InputPtr();
    if (g_camForcing.exchange(false) && input)  // no frame ran: the game's own CAM_Think picks it up later
        sdk::Write<int32_t>(input + g_camCmdOff, kCamToFirst);
}

}  // namespace emotes
