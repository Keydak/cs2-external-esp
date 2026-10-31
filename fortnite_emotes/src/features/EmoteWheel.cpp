#include "features/EmoteWheel.h"
#include "features/FortniteDances.h"
#include "features/Thirdperson.h"
#include "core/Journal.h"
#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include "sdk/ResourceManifest.h"
#include "sdk/SchemaSystem.h"
#include "ui/Fonts.h"
#include "ui/ImageTexture.h"
#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace emotes {

EmoteWheelConfig& GetEmoteWheelConfig() {
    static EmoteWheelConfig cfg;
    return cfg;
}

namespace {

// client.dll, each 1 hit in IDA and on disk (research_animation.md; the name setter: wwwant/anim/job_resname.json).
// bool CSkeletonInstance::ApplyAnimGraphPose(skel, CNmPose*): every final AG2 pose passes here before the bones.
constexpr const char* kApplyPosePattern =
    "40 57 41 55 41 56 48 83 EC 30 48 8B FA 4C 8B F1 48 85 D2 0F 84 ? ? ? ? 83 7A 38 00 0F 84 ? ? ? ? 48 8B 81";
// void CNmClip::GetPose(clip, Percentage_t (float bits in EDX), pose, bool allBones, bool floatChannels)
constexpr const char* kClipGetPosePattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 8B 59 08 0F 57 C0 FF CB 66 0F 6E CA";
// sub_18094D830: mov rcx,[ResourceSystem013]; lea rdx,name; xor r8d,r8d; mov rax,[rcx]; call [rax+disp32]
constexpr const char* kResSysCallPattern = "48 8B 0D ? ? ? ? 48 8D 55 ? 45 33 C0 48 8B 01 FF 90 ? ? ? ? 48 8B F0";
// sub_181974FB0: CResourceName setter (the same call site fills its name with it): copies the path, normalises it
// and stores the name/type hashes at +208/+216 that the lookup uses.
constexpr const char* kResNameSetPattern =
    "48 89 5C 24 10 57 48 83 EC 30 8B 41 04 48 8D 79 08 48 8B D9 A9 FF FF FF 3F 76 0F 0F BA E0 1E 48 8B C7 72 03 "
    "48 8B 07 C6 00 00 81 21 00 00 00 C0";

using ApplyPoseFn = bool(__fastcall*)(void* skel, void* pose);
using ClipGetPoseFn = void(__fastcall*)(void* clip, uint32_t pctBits, void* pose, uint8_t allBones, bool floatChannels);
using ResNameSetFn = bool(__fastcall*)(void* name, const char* path);
using ResFindFn = void*(__fastcall*)(void* resSys, const void* name, int flags);

// CResourceName as sub_18094D830 builds it: CBufferStringN<200> + two hashes. Zeroed, flags 0xC00000C8.
struct ResName { uint32_t len; uint32_t flags; char buf[200]; uint64_t nameHash, typeHash; };
static_assert(sizeof(ResName) == 224);

struct Clip { const char* label; const char* path; };
// Append-only: the index is the settings value. All on worldmodel.vnmskel; paths checked in wwwant/anim/clips.txt.
constexpr Clip kClips[] = {
    {"Celebrate 1",    "animation/anims/ui_anims/mvp/banner_celebrate01.vnmclip"},
    {"Celebrate 2",    "animation/anims/ui_anims/mvp/banner_celebrate02.vnmclip"},
    {"Celebrate 3",    "animation/anims/ui_anims/mvp/banner_celebrate03.vnmclip"},
    {"Celebrate 4",    "animation/anims/ui_anims/mvp/banner_celebrate04.vnmclip"},
    {"Knife backflip", "animation/anims/ui_anims/main_menu/ct/ct_main_menu_knife_backflip.vnmclip"},
    {"Knife tricks",   "animation/anims/ui_anims/main_menu/t/t_main_menu_knife_fancymoves.vnmclip"},
    {"Shrug",          "animation/anims/ui_anims/main_menu/ct/ct_main_menu_elite_shrug.vnmclip"},
    {"Look at watch",  "animation/anims/ui_anims/main_menu/ct/ct_main_menu_famas_lookatwatch.vnmclip"},
    {"Butt scratch",   "animation/anims/ui_anims/main_menu/t/t_main_menu_pistol_buttscratch.vnmclip"},
    {"Head scratch",   "animation/anims/ui_anims/main_menu/t/t_main_menu_pistol_headscratch.vnmclip"},
    {"Belly rub",      "animation/anims/ui_anims/main_menu/t/t_main_menu_elite_bellyrub.vnmclip"},
    {"C4 bobble",      "animation/anims/ui_anims/main_menu/t/t_main_menu_c4_bobble.vnmclip"},
    {"Victory 1",      "animation/anims/ui_anims/end_round/celebrate/ct/celebrate_ava_idle01.vnmclip"},
    {"Victory 2",      "animation/anims/ui_anims/end_round/celebrate/t/celebrate_darryl_idle01.vnmclip"},
    {"Defeat",         "animation/anims/ui_anims/end_round/defeat/ct/defeat_ava_idle01.vnmclip"},
    {"Pose 1",         "animation/anims/ui_anims/inventory_pose/ct/cu_ct_pose01.vnmclip"},
    {"Pose 2",         "animation/anims/ui_anims/inventory_pose/t/cu_t_pose01.vnmclip"},
    {"Flashed",        "animation/anims/world/shared/flashed.vnmclip"},
    {"Dead",           "animation/anims/world/shared/death_poses_body.vnmclip"},
};
constexpr int kClipCount = static_cast<int>(sizeof(kClips) / sizeof(kClips[0]));
static_assert(kClipCount == 19);
constexpr const char* kSkeletonPath = "animation/skeletons/characters/worldmodel.vnmskel";

constexpr int kDanceBase = 1000;  // emote slot / request value 1000+i = Fortnite dance i
// Pose clips (Pose 1/2: 0.03 s, Dead: 0.3 s, Flashed: 1 frame, 0 s) show frame 0 and hold; never looped.
constexpr float kStaticClip = 1.0f;  // seconds; a shorter clip is a pose
constexpr float kPoseHold   = 5.0f;  // emote length for a pose (moving still stops it)
constexpr float kPi = 3.14159265f;

// Hook state. Written before the hook is enabled (install) or on the game thread; the hook only loads.
ApplyPoseFn g_origApplyPose = nullptr;
ClipGetPoseFn g_clipGetPose = nullptr;
ResNameSetFn g_resNameSet = nullptr;
uintptr_t g_resSysGlobal = 0;    // &ResourceSystem013 pointer
int32_t g_resFindOff = 0;        // vtable byte offset of the find-by-name method
std::atomic<int> g_state{0};     // 0 not installed, 1 ready, -1 off for the session
bool g_canFind = false;          // resource lookup: the player skeleton
std::atomic<bool> g_canPlay{false};  // lookup + clip sampler + CNmClip schema: the emote wheel
int32_t g_ownerOff = -1, g_healthOff = -1, g_absVelOff = -1, g_clipSkelOff = -1, g_clipDurOff = -1, g_clipAddOff = -1;
int g_schemaState = 0;           // 0 unresolved, 1 ok, -1 missing

// The pawn playing a CS2 clip (your emote), read by the pose hook.
struct Slot {
    std::atomic<void*> pawn{nullptr};
    std::atomic<void*> clip{nullptr};
    std::atomic<double> start{0.0};
};
Slot g_slot;
std::atomic<bool> g_active{false};      // the slot is filled; the detour's fast path
std::atomic<bool> g_releasing{false};
std::atomic<void*> g_playerSkel{nullptr};  // CNmSkeleton* of worldmodel.vnmskel for this level
std::atomic<int> g_hookFaults{0};
int g_faults = 0;                       // game-thread faults

// Clips (game thread; the hook only sees them through slots).
std::atomic<void*> g_manifest{nullptr};
void* g_clips[kClipCount] = {};
bool g_clipsBound = false, g_loadFailed = false;
double g_loadStart = 0.0, g_nextSkelTry = 0.0;

// Emote. g_emoteRequest / g_emoteStop come from the render thread.
std::atomic<int> g_emoteRequest{-1};
std::atomic<bool> g_emoteStop{false};
std::atomic<bool> g_emoteActive{false};
std::atomic<bool> g_savedThird{false};
std::atomic<bool> g_localAlive{false};
std::atomic<bool> g_wheelOpen{false};
std::atomic<uint64_t> g_firedMask{0};  // weapon_fire userids (player slots 0-63), from the event thread
void* g_emoteClip = nullptr;
int g_emoteDance = -1;                  // Fortnite dance index while one plays, else -1
int g_emoteValue = -1;                  // the wheel value that started it (journal name)
void* g_emotePawn = nullptr;
double g_emoteStart = 0.0;
float g_emoteDur = 0.0f;

using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();
double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

// __except filter: catch access violations only. Stack overflow / guard-page exceptions must reach the OS; a swallowed
// overflow leaves the thread without a guard page and the next one kills the process with no dump.
int AvOnly(unsigned long code) { return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }

// Anim worker threads and the main thread. 0 = call the original, -1 = fault.
int EditPose(uintptr_t skel, uintptr_t pose) {
    __try {
        void* owner = *reinterpret_cast<void**>(skel + g_ownerOff);
        if (!owner || g_slot.pawn.load(std::memory_order_acquire) != owner) return 0;
        // The player skeleton's own pose only (not the weapon poses on the same skeleton).
        void* playerSkel = g_playerSkel.load(std::memory_order_relaxed);
        if (!playerSkel || *reinterpret_cast<void**>(pose) != playerSkel || *reinterpret_cast<int32_t*>(pose + 8) <= 0)
            return 0;
        if (!g_clipGetPose) return 0;
        const uintptr_t clip = reinterpret_cast<uintptr_t>(g_slot.clip.load(std::memory_order_relaxed));
        if (!clip || *reinterpret_cast<uint8_t*>(clip + g_clipAddOff)) return 0;  // additive clips need a base pose
        const uintptr_t skelBinding = *reinterpret_cast<uintptr_t*>(clip + g_clipSkelOff);
        if (!skelBinding || *reinterpret_cast<void**>(skelBinding) != playerSkel) return 0;
        const float dur = *reinterpret_cast<float*>(clip + g_clipDurOff);
        if (!(dur >= 0.0f) || !std::isfinite(dur)) return 0;
        // GetPose(allBones) writes skeleton+8 transforms into pose+0x10 unchecked: the pose must hold the full skeleton,
        // and be a live pose (state pose+0x38; the original skips state 0).
        const int32_t bones = *reinterpret_cast<int32_t*>(pose + 8);
        const uintptr_t ps = reinterpret_cast<uintptr_t>(playerSkel);
        if (bones != *reinterpret_cast<int32_t*>(ps + 8) || bones != *reinterpret_cast<int32_t*>(ps + 0x30) ||
            !*reinterpret_cast<void**>(pose + 0x10) || !*reinterpret_cast<int32_t*>(pose + 0x38))
            return 0;
        const bool still = dur < kStaticClip || *reinterpret_cast<int32_t*>(clip + 8) < 2;  // m_nNumFrames
        const double t = still ? 0.0 : (Now() - g_slot.start.load(std::memory_order_relaxed)) / dur;
        const float pct = static_cast<float>(std::clamp(t, 0.0, 1.0));  // an emote plays once
        g_clipGetPose(reinterpret_cast<void*>(clip), std::bit_cast<uint32_t>(pct), reinterpret_cast<void*>(pose), 1, false);
        if (float* root = *reinterpret_cast<float**>(pose + 0x10)) root[0] = root[1] = 0.0f;  // UI clips walk the root
        return 0;
    } __except(AvOnly(GetExceptionCode())) {
        return -1;
    }
}

bool __fastcall HkApplyPose(void* skel, void* pose) {
    if (g_active.load(std::memory_order_acquire) && !g_releasing.load(std::memory_order_relaxed) && skel && pose) {
        const int r = EditPose(reinterpret_cast<uintptr_t>(skel), reinterpret_cast<uintptr_t>(pose));
        if (r < 0 && g_hookFaults.fetch_add(1) + 1 >= 5) g_active = false;  // EmoteWheelFrame warns and turns off
    }
    return g_origApplyPose(skel, pose);
}

void ClearSlots() {
    g_active = false;
    g_slot.pawn.store(nullptr, std::memory_order_release);
}

void RestoreThirdperson() {
    if (g_emoteActive.exchange(false)) GetThirdpersonConfig().thirdperson = g_savedThird.load();
}

const char* EmoteName(int value) {
    return value >= kDanceBase ? FortniteDanceName(value - kDanceBase) : value >= 0 && value < kClipCount ? kClips[value].label : "?";
}

// Game thread. The journal line shows the last step if the game hangs.
void StopEmote(const char* reason) {
    if (g_emoteActive.load()) journal().info("anim", std::string("emote stopped: ") + EmoteName(g_emoteValue) + " (" + reason + ")");
    RestoreThirdperson();
    g_emoteClip = g_emotePawn = nullptr;
    g_emoteDance = g_emoteValue = -1;
}

void DropManifest() {
    if (void* m = g_manifest.exchange(nullptr)) sdk::ManifestDestroy(m);
}

// Game thread. false = the feature just got turned off for the session.
bool Fault() {
    if (++g_faults < 5) return true;
    g_state = -1;
    ClearSlots();
    StopEmote("errors");
    journal().warn("anim", "emote wheel turned off after repeated errors");
    return false;
}

bool ResolveSchema() {
    if (g_schemaState) return g_schemaState > 0;
    g_schemaState = -1;
    auto client = [](const char* cls, const char* field) { return sdk::GetSchemaOffset("client.dll", cls, field); };
    g_ownerOff  = client("CGameSceneNode", "m_pOwner");
    g_healthOff = client("C_BaseEntity", "m_iHealth");
    g_absVelOff = client("C_BaseEntity", "m_vecAbsVelocity");  // missing = the emote does not stop on moving
    if (g_ownerOff < 0 || g_healthOff < 0) {
        g_state = -1;
        journal().warn("anim", "emote wheel unavailable: player fields not found");
        return false;
    }
    g_clipSkelOff = sdk::GetSchemaOffset("animationsystem.dll", "CNmClip", "m_skeleton");
    g_clipDurOff  = sdk::GetSchemaOffset("animationsystem.dll", "CNmClip", "m_flDuration");
    g_clipAddOff  = sdk::GetSchemaOffset("animationsystem.dll", "CNmClip", "m_bIsAdditive");
    if (g_canPlay.load() && (g_clipSkelOff < 0 || g_clipDurOff < 0 || g_clipAddOff < 0)) {
        g_canPlay = false;
        journal().warn("anim", "emote wheel unavailable: clip fields not found");
    }
    g_schemaState = 1;
    return true;
}

// Resource binding for a loaded name -> its m_pData, or null (unknown, not loaded, or flagged bad). false = fault.
bool FindResource(const char* path, void*& data) {
    data = nullptr;
    __try {
        ResName name;
        std::memset(&name, 0, sizeof(name));
        name.flags = 0xC00000C8u;
        // ponytail: our paths fit the 200-byte inline buffer, so no heap string to CBufferString::Purge
        if (!g_resNameSet(&name, path)) return true;
        void* resSys = *reinterpret_cast<void**>(g_resSysGlobal);
        if (!resSys) return true;
        auto find = *reinterpret_cast<ResFindFn*>(*reinterpret_cast<uintptr_t*>(resSys) + g_resFindOff);
        const uintptr_t binding = reinterpret_cast<uintptr_t>(find(resSys, &name, 0));
        if (binding && !(*reinterpret_cast<uint8_t*>(binding + 0x18) & 2)) data = *reinterpret_cast<void**>(binding);
        return true;
    } __except(AvOnly(GetExceptionCode())) {
        return false;
    }
}

// A clip the hook can sample onto the player skeleton. false = fault.
bool ClipUsable(uintptr_t clip, void* playerSkel, bool& ok) {
    ok = false;
    __try {
        const uintptr_t skelBinding = *reinterpret_cast<uintptr_t*>(clip + g_clipSkelOff);
        const float dur = *reinterpret_cast<float*>(clip + g_clipDurOff);
        ok = skelBinding && *reinterpret_cast<void**>(skelBinding) == playerSkel &&
             !*reinterpret_cast<uint8_t*>(clip + g_clipAddOff) && dur >= 0.0f && std::isfinite(dur) &&
             *reinterpret_cast<int32_t*>(clip + 8) > 0;  // m_nNumFrames (GetPose reads +8); 1 frame has 0 s
        return true;
    } __except(AvOnly(GetExceptionCode())) {
        return false;
    }
}

// The player skeleton is resident while players exist, so it resolves without the manifest. Retried once a second.
bool EnsureSkeleton(double now) {
    if (g_playerSkel.load() || now < g_nextSkelTry) return true;
    g_nextSkelTry = now + 1.0;
    void* skel;
    if (!FindResource(kSkeletonPath, skel)) return Fault();
    g_playerSkel = skel;
    return true;
}

// One manifest per level for every clip + the skeleton; bindings resolved once it has loaded.
bool LoadClips(double now) {
    if (g_clipsBound || g_loadFailed) return true;
    if (!g_manifest.load()) {
        const char* names[kClipCount + 1];
        for (int i = 0; i < kClipCount; ++i) names[i] = kClips[i].path;
        names[kClipCount] = kSkeletonPath;
        g_manifest = sdk::ManifestCreate(names, kClipCount + 1);
        g_loadStart = now;
        if (!g_manifest.load()) {
            g_loadFailed = true;
            journal().warn("anim", "animation clips could not be loaded on this map");
            return true;
        }
    }
    if (!sdk::ManifestLoaded(g_manifest.load())) {
        if (now - g_loadStart > 30.0) {
            g_loadFailed = true;
            journal().warn("anim", "animation clips did not finish loading on this map");
        }
        return true;
    }
    g_clipsBound = true;
    void* skel;
    if (!FindResource(kSkeletonPath, skel)) return Fault();
    if (skel) g_playerSkel = skel;
    int loaded = 0;
    for (int i = 0; i < kClipCount && g_playerSkel.load(); ++i) {
        void* clip;
        bool ok = false;
        if (!FindResource(kClips[i].path, clip) || (clip && !ClipUsable(reinterpret_cast<uintptr_t>(clip), g_playerSkel.load(), ok)))
            return Fault();
        if (clip && ok) g_clips[i] = clip, ++loaded;
    }
    if (loaded < kClipCount)
        journal().warn("anim", std::to_string(loaded) + " of " + std::to_string(kClipCount) + " animation clips loaded");
    return true;
}

void SetSlot(void* pawn, void* clip, double start) {
    Slot& s = g_slot;
    const bool same = s.pawn.load() == pawn;
    if (!same) s.pawn.store(nullptr, std::memory_order_release);
    s.clip.store(clip, std::memory_order_relaxed);
    s.start.store(start, std::memory_order_relaxed);
    if (!same) s.pawn.store(pawn, std::memory_order_release);
}

// ─── Emote wheel (render thread): Fortnite's layout. 6 wedges per page, the hovered item's name in the middle,
// pages with the mouse wheel or the arrows. Fortnite entries show their icon on their rarity's colours.

constexpr int kWheelSlots = 6;

struct WheelPage {
    const char* category;          // FAVOURITES, DANCES, EMOTES, CS2
    int number, of;                // page within its category, 1-based
    int values[kWheelSlots];       // clip index, kDanceBase + dance, or -1
};

struct DanceLists {
    int list[2][160];              // [0] dances, [1] emotes: Fortnite table indices in table order
    int n[2];
};
const DanceLists& Lists() {
    static const DanceLists lists = [] {
        DanceLists d{};
        for (int i = 0; i < FortniteDanceCount(); ++i) {
            const int e = FortniteDanceIsEmote(i) ? 1 : 0;
            if (d.n[e] < 160) d.list[e][d.n[e]++] = i;
        }
        return d;
    }();
    return lists;
}

constexpr int PagesFor(int count) { return (count + kWheelSlots - 1) / kWheelSlots; }

int WheelPageCount(bool fortnite) {
    const DanceLists& l = Lists();
    return 1 + (fortnite ? PagesFor(l.n[0]) + PagesFor(l.n[1]) : 0) + PagesFor(kClipCount);
}

// Page p of: Favourites, the dances, the emotes (only when the Fortnite model is ready), the CS2 clips.
WheelPage WheelPageAt(int p, bool fortnite) {
    WheelPage pg{"FAVOURITES", 1, 1, {-1, -1, -1, -1, -1, -1}};
    if (p <= 0) {
        for (int k = 0; k < kWheelSlots; ++k) pg.values[k] = GetEmoteWheelConfig().emoteSlots[k].load();
        return pg;
    }
    --p;
    const DanceLists& l = Lists();
    for (int e = 0; fortnite && e < 2; ++e) {
        if (p < PagesFor(l.n[e])) {
            pg = {e ? "EMOTES" : "DANCES", p + 1, PagesFor(l.n[e]), {-1, -1, -1, -1, -1, -1}};
            for (int k = 0; k < kWheelSlots && p * kWheelSlots + k < l.n[e]; ++k)
                pg.values[k] = kDanceBase + l.list[e][p * kWheelSlots + k];
            return pg;
        }
        p -= PagesFor(l.n[e]);
    }
    p = std::min(p, PagesFor(kClipCount) - 1);
    pg = {"CS2", p + 1, PagesFor(kClipCount), {-1, -1, -1, -1, -1, -1}};
    for (int k = 0; k < kWheelSlots && p * kWheelSlots + k < kClipCount; ++k) pg.values[k] = p * kWheelSlots + k;
    return pg;
}

bool WheelSelectable(int v, bool fortnite) {
    return (v >= 0 && v < kClipCount) || (fortnite && v >= kDanceBase && v - kDanceBase < FortniteDanceCount());
}

// Fortnite's rarity colours (locker tile gradient: light = centre, dark = edge). -1 = a CS2 clip.
struct Rarity { ImU32 light, dark; const char* name; };
Rarity RarityOf(int r) {
    switch (r) {
        case 1:  return {IM_COL32(190, 190, 190, 255), IM_COL32(99, 99, 99, 255), "COMMON"};
        case 2:  return {IM_COL32(105, 187, 30, 255), IM_COL32(23, 81, 23, 255), "UNCOMMON"};
        case 4:  return {IM_COL32(195, 89, 255, 255), IM_COL32(75, 36, 131, 255), "EPIC"};
        case 5:  return {IM_COL32(234, 141, 35, 255), IM_COL32(120, 55, 29, 255), "LEGENDARY"};
        case 6:  return {IM_COL32(197, 51, 52, 255), IM_COL32(118, 27, 27, 255), "MARVEL SERIES"};
        case 7:  return {IM_COL32(54, 183, 183, 255), IM_COL32(12, 90, 90, 255), "ICON SERIES"};
        case 8:  return {IM_COL32(125, 91, 239, 255), IM_COL32(49, 36, 151, 255), "GAMING LEGENDS"};
        case 9:  return {IM_COL32(84, 117, 199, 255), IM_COL32(36, 52, 107, 255), "DC SERIES"};
        case 10: return {IM_COL32(32, 84, 139, 255), IM_COL32(8, 23, 43, 255), "STAR WARS SERIES"};
        case -1: return {IM_COL32(222, 155, 53, 255), IM_COL32(43, 47, 54, 255), "CS2"};
        case 3:  return {IM_COL32(44, 193, 255, 255), IM_COL32(20, 57, 119, 255), "RARE"};
        default: return {IM_COL32(44, 193, 255, 255), IM_COL32(20, 57, 119, 255), ""};
    }
}

struct WheelItem { const char* name; const char* kind; int rarity; int dance; };
WheelItem ItemOf(int v) {
    if (v >= kDanceBase && v - kDanceBase < FortniteDanceCount()) {
        const int d = v - kDanceBase;
        return {FortniteDanceName(d), FortniteDanceIsEmote(d) ? "EMOTE" : "DANCE", FortniteDanceRarity(d), d};
    }
    if (v >= 0 && v < kClipCount) return {kClips[v].label, "ANIMATION", -1, -1};
    return {"", "", 0, -1};
}

// Icons: <pack>/icons/<seq>.png, loaded on the render thread the first time their page shows.
struct IconSlot { ID3D11ShaderResourceView* srv = nullptr; int state = 0; };  // 0 not tried, 1 loaded, -1 missing
IconSlot g_icons[kFortniteDanceCount];

ID3D11ShaderResourceView* IconFor(int dance, int side, int& budget) {
    if (dance < 0 || dance >= kFortniteDanceCount) return nullptr;
    IconSlot& s = g_icons[dance];
    if (s.state == 0 && budget > 0) {
        const std::filesystem::path data = FortniteDancesDir();
        const std::filesystem::path dir = data.empty() ? data : data / "icons";
        if (!dir.empty()) {  // config not read yet: try again later
            --budget;
            s.srv = ui::LoadImageTexture(dir / (std::string(FortniteDanceSeq(dance)) + ".png"), side);
            s.state = s.srv ? 1 : -1;
        }
    }
    return s.state == 1 ? s.srv : nullptr;
}

ImU32 Fade(ImU32 c, float a) {
    const float alpha = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) {
        const float x = static_cast<float>((a >> s) & 0xFF), y = static_cast<float>((b >> s) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t + 0.5f) << s;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

// Ring segment centred on angle mid (half-width half), with a gap of constant width to its neighbours.
// Vertex colours give Fortnite's inner-to-outer gradient; the outline smooths the edges.
void Wedge(ImDrawList* dl, ImVec2 c, float r0, float r1, float mid, float half, float gap, ImU32 inner, ImU32 outer,
           ImU32 line, float lineW) {
    constexpr int N = 28;
    const float gi = std::asin(std::min(0.9f, gap * 0.5f / r0)), go = std::asin(std::min(0.9f, gap * 0.5f / r1));
    ImVec2 pin[N + 1], pout[N + 1];
    for (int j = 0; j <= N; ++j) {
        const float t = static_cast<float>(j) / N;
        const float ai = mid - half + gi + (2.0f * (half - gi)) * t, ao = mid - half + go + (2.0f * (half - go)) * t;
        pin[j] = ImVec2(c.x + std::cos(ai) * r0, c.y + std::sin(ai) * r0);
        pout[j] = ImVec2(c.x + std::cos(ao) * r1, c.y + std::sin(ao) * r1);
    }
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(N * 6, (N + 1) * 2);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int j = 0; j <= N; ++j) {
        dl->PrimWriteVtx(pin[j], uv, inner);
        dl->PrimWriteVtx(pout[j], uv, outer);
    }
    for (int j = 0; j < N; ++j) {
        const ImDrawIdx a = static_cast<ImDrawIdx>(base + 2 * j);
        dl->PrimWriteIdx(a), dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
        dl->PrimWriteIdx(a), dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 2));
    }
    if ((line & IM_COL32_A_MASK) && lineW > 0.0f) {
        for (int j = 0; j <= N; ++j) dl->PathLineTo(pout[j]);
        for (int j = N; j >= 0; --j) dl->PathLineTo(pin[j]);
        dl->PathStroke(line, ImDrawFlags_Closed, lineW);
    }
}

// Upper-case copy (the names are ASCII).
const char* Upper(const char* s, char (&buf)[64]) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < sizeof(buf); ++i) buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    buf[i] = '\0';
    return buf;
}

// Text centred on x, top at y, shrunk to maxW; splits at a space into two lines first. Returns the height used.
float CenteredText(ImDrawList* dl, ImFont* font, float size, float minSize, ImVec2 at, float maxW, ImU32 col,
                   const char* text, bool draw = true) {
    float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    if (w <= maxW) {
        if (draw) dl->AddText(font, size, ImVec2(std::floor(at.x - w * 0.5f), std::floor(at.y)), col, text);
        return size;
    }
    const char* split = nullptr;  // the space closest to the middle
    const size_t len = std::strlen(text);
    for (const char* p = std::strchr(text, ' '); p; p = std::strchr(p + 1, ' '))
        if (!split || std::abs(static_cast<long>(p - text) - static_cast<long>(len / 2)) <
                          std::abs(static_cast<long>(split - text) - static_cast<long>(len / 2)))
            split = p;
    if (!split) {
        size = std::max(minSize, size * maxW / w);
        w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
        if (draw) dl->AddText(font, size, ImVec2(std::floor(at.x - w * 0.5f), std::floor(at.y)), col, text);
        return size;
    }
    float w1 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, split).x;
    float w2 = font->CalcTextSizeA(size, FLT_MAX, 0.0f, split + 1).x;
    if (std::max(w1, w2) > maxW) {
        const float k = maxW / std::max(w1, w2);
        size = std::max(minSize, size * k), w1 *= k, w2 *= k;
    }
    const float lineH = size * 0.92f;
    if (!draw) return lineH + size;
    dl->AddText(font, size, ImVec2(std::floor(at.x - w1 * 0.5f), std::floor(at.y)), col, text, split);
    dl->AddText(font, size, ImVec2(std::floor(at.x - w2 * 0.5f), std::floor(at.y + lineH)), col, split + 1);
    return lineH + size;
}

void Keycap(ImDrawList* dl, ImFont* font, float size, ImVec2& at, const char* key, const char* label, float s, float alpha) {
    const ImVec2 kw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, key);
    const float padX = 6.0f * s, h = size + 6.0f * s;
    dl->AddRect(ImVec2(at.x, at.y), ImVec2(at.x + kw.x + padX * 2.0f, at.y + h), Fade(IM_COL32(255, 255, 255, 170), alpha),
                4.0f * s, 0, 1.5f * s);
    dl->AddText(font, size, ImVec2(at.x + padX, at.y + 3.0f * s), Fade(IM_COL32(255, 255, 255, 235), alpha), key);
    at.x += kw.x + padX * 2.0f + 7.0f * s;
    const ImVec2 lw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
    dl->AddText(font, size, ImVec2(at.x, at.y + 3.0f * s), Fade(IM_COL32(255, 255, 255, 200), alpha), label);
    at.x += lw.x + 22.0f * s;
}

struct WheelGeom {
    ImVec2 c;
    float s, R, r0, gap;
    ImVec2 arrow[2];  // previous / next page buttons
    float arrowR;
};
WheelGeom GeomFor(float open) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    WheelGeom g{};
    g.c = ImVec2(size.x * 0.5f, size.y * 0.5f);
    g.s = std::max(0.5f, size.y / 1080.0f);
    const float k = 0.9f + 0.1f * open;  // Fortnite's wheel grows into place
    g.R = 290.0f * g.s * k;
    g.r0 = 112.0f * g.s * k;
    g.gap = 7.0f * g.s;
    g.arrowR = 19.0f * g.s;
    const float y = g.c.y + g.R + 44.0f * g.s;
    g.arrow[0] = ImVec2(g.c.x - 170.0f * g.s, y);
    g.arrow[1] = ImVec2(g.c.x + 170.0f * g.s, y);
    return g;
}

int HotSlot(const WheelGeom& g, ImVec2 cur) {
    const float dx = cur.x - g.c.x, dy = cur.y - g.c.y;
    if (dx * dx + dy * dy < (g.r0 * 0.45f) * (g.r0 * 0.45f)) return -1;
    const float step = 2.0f * kPi / kWheelSlots;  // slot 0 at the top, then clockwise
    const int k = static_cast<int>(std::floor((std::atan2(dy, dx) + kPi * 0.5f + step * 0.5f) / step));
    return ((k % kWheelSlots) + kWheelSlots) % kWheelSlots;
}

int HotArrow(const WheelGeom& g, ImVec2 cur) {
    for (int i = 0; i < 2; ++i) {
        const float dx = cur.x - g.arrow[i].x, dy = cur.y - g.arrow[i].y, r = g.arrowR * 1.4f;
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

// open 0-1 (eased), pop[k] 0-1 hover emphasis per wedge; iconBudget = icons that may load this frame.
void DrawWheel(ImDrawList* dl, const WheelPage& pg, int pageIndex, int pageCount, int hot, int arrowHot, float open,
               const float* pop, ImVec2 cursor, bool drawCursor, bool fortnite, int iconBudget) {
    const float a = open;  // alpha
    const WheelGeom g = GeomFor(open);
    const float s = g.s, step = 2.0f * kPi / kWheelSlots;
    ImFont* heavy = ui::HeavyFont();
    ImFont* body = ui::BodyFont();
    if (!heavy) heavy = ImGui::GetFont();
    if (!body) body = ImGui::GetFont();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const int iconSide = static_cast<int>(std::ceil((g.R - g.r0) * 0.80f / (0.9f + 0.1f * open) * 1.1f));

    dl->AddRectFilled(ImVec2(0, 0), screen, Fade(IM_COL32(0, 0, 0, 110), a));  // the world dims behind the wheel

    for (int k = 0; k < kWheelSlots; ++k) {
        const int v = pg.values[k];
        const WheelItem it = ItemOf(v);
        const bool ok = WheelSelectable(v, fortnite);
        const float p = pop[k];
        const float mid = -kPi * 0.5f + k * step;
        const ImVec2 dir(std::cos(mid), std::sin(mid));
        const ImVec2 c(g.c.x + dir.x * 12.0f * s * p, g.c.y + dir.y * 12.0f * s * p);  // the hovered wedge steps out
        if (v < 0) {
            Wedge(dl, c, g.r0, g.R, mid, step * 0.5f, g.gap, Fade(IM_COL32(8, 12, 22, 150), a), Fade(IM_COL32(20, 27, 40, 170), a),
                  Fade(IM_COL32(255, 255, 255, 28), a), 1.5f * s);
            continue;
        }
        const Rarity r = RarityOf(it.rarity);
        const float dim = ok ? 1.0f : 0.45f;
        // Resting: dark with the rarity coming through at the rim. Hovered: the full locker gradient + white edge.
        const ImU32 inner = Mix(Mix(IM_COL32(8, 12, 22, 190), r.dark, 0.55f), r.dark, p);
        const ImU32 outer = Mix(Mix(IM_COL32(14, 20, 34, 205), r.light, 0.38f), r.light, p);
        Wedge(dl, c, g.r0, g.R, mid, step * 0.5f, g.gap, Fade(inner, a * dim * (0.92f + 0.08f * p)),
              Fade(outer, a * dim * (0.92f + 0.08f * p)), Fade(Mix(Fade(r.light, 0.55f), IM_COL32(255, 255, 255, 255), p), a * dim),
              (1.5f + 2.0f * p) * s);
        // rarity rim along the outer edge
        dl->PathArcTo(c, g.R - 3.0f * s, mid - step * 0.5f + g.gap / g.R, mid + step * 0.5f - g.gap / g.R, 24);
        dl->PathStroke(Fade(r.light, a * dim), 0, 4.0f * s);

        const float rm = (g.R + g.r0) * 0.5f;
        const ImVec2 at(c.x + dir.x * rm, c.y + dir.y * rm);
        const float side = (g.R - g.r0) * 0.80f * (1.0f + 0.10f * p);
        ID3D11ShaderResourceView* icon = it.dance >= 0 ? IconFor(it.dance, iconSide, iconBudget) : nullptr;
        if (icon) {
            dl->AddImage(reinterpret_cast<ImTextureID>(icon), ImVec2(at.x - side * 0.5f, at.y - side * 0.5f),
                         ImVec2(at.x + side * 0.5f, at.y + side * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                         Fade(IM_COL32(255, 255, 255, 255), a * (ok ? 1.0f : 0.35f)));
        } else {  // CS2 clips and missing icons: the name on the tile
            char up[64];
            const float size = 26.0f * s * (1.0f + 0.08f * p);
            const float maxW = (g.R - g.r0) * 0.92f;
            CenteredText(dl, heavy, size, 16.0f * s, ImVec2(at.x, at.y - size * 0.95f), maxW,
                         Fade(IM_COL32(255, 255, 255, 240), a * dim), Upper(it.name, up));
            if (it.rarity < 0)
                CenteredText(dl, body, 13.0f * s, 11.0f * s, ImVec2(at.x, at.y + size * 1.05f), maxW,
                             Fade(Mix(r.light, IM_COL32(255, 255, 255, 255), p), a * dim), "CS2");
        }
    }

    // Centre: the hovered item, or which page this is.
    dl->AddCircleFilled(g.c, g.r0 - g.gap, Fade(IM_COL32(6, 10, 20, 225), a), 64);
    dl->AddCircle(g.c, g.r0 - g.gap, Fade(IM_COL32(255, 255, 255, 38), a), 64, 2.0f * s);
    char up[64], line[64];
    const float innerW = (g.r0 - g.gap) * 1.62f;
    if (hot >= 0 && pg.values[hot] >= 0) {
        const WheelItem it = ItemOf(pg.values[hot]);
        const Rarity r = RarityOf(it.rarity);
        const bool ok = WheelSelectable(pg.values[hot], fortnite);
        std::snprintf(line, sizeof(line), "%s%s%s", ok ? r.name : "", ok && r.name[0] ? " " : "",
                      ok ? it.kind : "EMOTE PACK NEEDED");
        const float nameSize = 40.0f * s, subSize = 19.0f * s, gapY = 6.0f * s;
        Upper(it.name, up);
        const float nameH = CenteredText(dl, heavy, nameSize, 20.0f * s, g.c, innerW, 0, up, false);
        const float subH = CenteredText(dl, heavy, subSize, 12.0f * s, g.c, innerW, 0, line, false);
        const float y0 = g.c.y - (nameH + gapY + subH) * 0.5f;
        CenteredText(dl, heavy, nameSize, 20.0f * s, ImVec2(g.c.x, y0), innerW, Fade(IM_COL32(255, 255, 255, 255), a), up);
        CenteredText(dl, heavy, subSize, 12.0f * s, ImVec2(g.c.x, y0 + nameH + gapY), innerW,
                     Fade(ok ? r.light : IM_COL32(170, 170, 170, 255), a), line);
        // pointer notch on the inner ring
        const float mid = -kPi * 0.5f + hot * step, n = g.r0 - g.gap - 3.0f * s;
        const ImVec2 d(std::cos(mid), std::sin(mid)), t(-d.y, d.x);
        dl->AddTriangleFilled(ImVec2(g.c.x + d.x * n, g.c.y + d.y * n),
                              ImVec2(g.c.x + d.x * (n - 12.0f * s) + t.x * 9.0f * s, g.c.y + d.y * (n - 12.0f * s) + t.y * 9.0f * s),
                              ImVec2(g.c.x + d.x * (n - 12.0f * s) - t.x * 9.0f * s, g.c.y + d.y * (n - 12.0f * s) - t.y * 9.0f * s),
                              Fade(IM_COL32(255, 255, 255, 230), a));
    } else {
        const float size = 36.0f * s, subSize = 18.0f * s, gapY = 6.0f * s;
        const float y0 = g.c.y - (size + gapY + subSize) * 0.5f;
        CenteredText(dl, heavy, size, 18.0f * s, ImVec2(g.c.x, y0), innerW, Fade(IM_COL32(255, 255, 255, 255), a), pg.category);
        CenteredText(dl, heavy, subSize, 12.0f * s, ImVec2(g.c.x, y0 + size + gapY), innerW,
                     Fade(IM_COL32(170, 180, 200, 255), a), "PICK ONE");
    }

    // Page bar: < CATEGORY 2/8 >
    for (int i = 0; i < 2 && pageCount > 1; ++i) {
        const bool h = arrowHot == i;
        const ImVec2 ac = g.arrow[i];
        dl->AddCircleFilled(ac, g.arrowR, Fade(h ? IM_COL32(255, 255, 255, 240) : IM_COL32(8, 12, 22, 200), a), 32);
        dl->AddCircle(ac, g.arrowR, Fade(IM_COL32(255, 255, 255, 150), a), 32, 1.5f * s);
        const float w = 6.0f * s, d = i ? 1.0f : -1.0f;
        dl->AddTriangleFilled(ImVec2(ac.x + d * w, ac.y), ImVec2(ac.x - d * w * 0.7f, ac.y - w * 1.2f),
                              ImVec2(ac.x - d * w * 0.7f, ac.y + w * 1.2f),
                              Fade(h ? IM_COL32(8, 12, 22, 255) : IM_COL32(255, 255, 255, 235), a));
    }
    char pageText[48];
    std::snprintf(pageText, sizeof(pageText), "%d/%d", pg.number, pg.of);
    const float ps = 30.0f * s;
    const float cw = heavy->CalcTextSizeA(ps, FLT_MAX, 0.0f, pg.category).x, nw = heavy->CalcTextSizeA(ps, FLT_MAX, 0.0f, pageText).x;
    const float gapW = 10.0f * s, x0 = g.c.x - (cw + gapW + nw) * 0.5f, y0 = g.arrow[0].y - ps * 0.52f;
    dl->AddText(heavy, ps, ImVec2(std::floor(x0), std::floor(y0)), Fade(IM_COL32(255, 255, 255, 255), a), pg.category);
    dl->AddText(heavy, ps, ImVec2(std::floor(x0 + cw + gapW), std::floor(y0)), Fade(IM_COL32(150, 162, 185, 255), a), pageText);
    if (pageCount > 1 && pageIndex >= 0) {  // where this page sits among all of them
        const float dotStep = 11.0f * s, dx0 = g.c.x - dotStep * (pageCount - 1) * 0.5f, dy = g.arrow[0].y + g.arrowR + 12.0f * s;
        for (int i = 0; i < pageCount; ++i)
            dl->AddCircleFilled(ImVec2(dx0 + i * dotStep, dy), (i == pageIndex ? 3.4f : 2.4f) * s,
                                Fade(i == pageIndex ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 90), a), 12);
    }

    // Key hints, Fortnite style.
    const float hs = 15.0f * s;
    const char* keys[][2] = {{"LMB", "PLAY"}, {"RMB", "CLOSE"}, {"WHEEL", "PAGE"}};
    float total = 0.0f;
    for (auto& k : keys)
        total += body->CalcTextSizeA(hs, FLT_MAX, 0.0f, k[0]).x + 12.0f * s + 7.0f * s + body->CalcTextSizeA(hs, FLT_MAX, 0.0f, k[1]).x + 22.0f * s;
    ImVec2 at(g.c.x - (total - 22.0f * s) * 0.5f, g.arrow[0].y + g.arrowR + 30.0f * s);
    for (auto& k : keys) Keycap(dl, body, hs, at, k[0], k[1], s, a);

    const float cdx = cursor.x - g.c.x, cdy = cursor.y - g.c.y;
    if (drawCursor && cdx * cdx + cdy * cdy > g.r0 * g.r0) {  // the game hides the Windows cursor; ours, outside the centre
        dl->AddCircleFilled(cursor, 6.0f * s, Fade(IM_COL32(255, 255, 255, 245), a), 16);
        dl->AddCircle(cursor, 6.0f * s, Fade(IM_COL32(0, 0, 0, 200), a), 16, 1.5f * s);
    }
}

// Cursor in backbuffer pixels.
ImVec2 CursorInBackbuffer(HWND hwnd) {
    POINT pt{};
    RECT rc{};
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return ImVec2(0, 0);
    const ImVec2 size = ImGui::GetIO().DisplaySize;  // the backbuffer size (Hooks.cpp)
    return ImVec2(pt.x * size.x / rc.right, pt.y * size.y / rc.bottom);
}

void CenterCursor(HWND hwnd) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;
    POINT p{rc.right / 2, rc.bottom / 2};
    if (ClientToScreen(hwnd, &p)) SetCursorPos(p.x, p.y);
}

std::atomic<bool> g_wheelCancel{false};  // Esc from the window procedure

}  // namespace

void EmoteWheelInstallHooks(HookInstaller install) {
    if (g_state.load()) return;
    g_state = -1;
    auto scan = [](const char* p) { return sdk::PatternScan("client.dll", p); };
    const uintptr_t apply = scan(kApplyPosePattern), getPose = scan(kClipGetPosePattern), call = scan(kResSysCallPattern),
                    nameSet = scan(kResNameSetPattern);
    if (!apply || !install ||
        !install("ApplyAnimGraphPose", apply, reinterpret_cast<void*>(&HkApplyPose), reinterpret_cast<void**>(&g_origApplyPose))) {
        journal().warn("anim", "emote wheel unavailable: pose function not found");
        return;
    }
    const int32_t findOff = call ? sdk::Read<int32_t>(call + 19) : -1;
    if (call && nameSet && findOff > 0 && findOff < 0x1000 && findOff % 8 == 0) {
        g_resNameSet = reinterpret_cast<ResNameSetFn>(nameSet);
        g_resSysGlobal = sdk::ResolveRelative(call, 3, 7);
        g_resFindOff = findOff;
        g_canFind = true;
    }
    g_clipGetPose = reinterpret_cast<ClipGetPoseFn>(getPose);
    g_canPlay = g_canFind && getPose;
    if (!g_canFind)
        journal().warn("anim", "emote wheel unavailable: resource lookup not found");
    else if (!getPose)
        journal().warn("anim", "emote wheel unavailable: clip sampler not found");
    g_state = 1;
}

void EmoteWheelLevelShutdown() {
    ClearSlots();
    g_playerSkel = nullptr;
    for (void*& c : g_clips) c = nullptr;
    StopEmote("map change");
    g_emoteRequest = -1;
    DropManifest();
    g_clipsBound = g_loadFailed = false;
    g_nextSkelTry = 0.0;
}

void EmoteWheelRelease() {
    g_releasing = true;
    ClearSlots();
    if (g_emoteActive.load()) journal().info("anim", std::string("emote stopped: ") + EmoteName(g_emoteValue) + " (unload)");
    RestoreThirdperson();  // StopEmote's pointers belong to the game thread; ThirdpersonRelease switches the view back
    g_wheelOpen = false;
    for (int i = 0; i < 20 && g_manifest.load(); ++i)  // the next stage-12 pass drops it; bounded if no frames run
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void EmoteWheelFrame() {
    if (g_releasing.load()) {
        ClearSlots();
        StopEmote("unload");
        DropManifest();
        return;
    }
    if (g_state.load() <= 0) return;
    if (g_hookFaults.load() >= 5) {
        g_faults = 4;
        Fault();
        return;
    }
    auto& cfg = GetEmoteWheelConfig();
    const bool emoteOn = cfg.emoteEnabled.load() && g_canPlay.load() && ThirdpersonAvailable();
    if (!emoteOn && !g_emoteActive.load() && !g_active.load()) {
        g_localAlive = false;
        return;
    }
    if (!ResolveSchema() || !sdk::IsInGame()) {
        ClearSlots();
        StopEmote("left the game");
        g_localAlive = false;
        return;
    }
    const double now = Now();
    const uintptr_t localCtrl = reinterpret_cast<uintptr_t>(sdk::GetLocalController());
    const uintptr_t localPawn = reinterpret_cast<uintptr_t>(sdk::GetLocalPawn());
    const bool localAlive = localPawn && sdk::Read<int32_t>(localPawn + g_healthOff) > 0;
    g_localAlive = localAlive;

    if (g_canFind && emoteOn && !EnsureSkeleton(now)) return;
    if (g_canPlay.load() && emoteOn && !LoadClips(now)) return;

    // Local player slot for weapon_fire userids: the controller's entity index - 1.
    static uintptr_t s_slotCtrl = 0;
    static int s_localSlot = -1;
    if (localCtrl != s_slotCtrl) {
        s_slotCtrl = localCtrl;
        s_localSlot = -1;
        for (int i = 1; i <= 64 && localCtrl; ++i)
            if (reinterpret_cast<uintptr_t>(sdk::GetEntityByIndex(i)) == localCtrl) {
                s_localSlot = i - 1;
                break;
            }
    }
    const uint64_t fired = g_firedMask.exchange(0);

    // Emote: stop first, then take a new request.
    const int request = g_emoteRequest.exchange(-1);
    if (g_emoteActive.load()) {
        const char* why = g_emoteStop.exchange(false) ? "key or new round"
                          : !emoteOn ? "turned off"
                          : !localAlive ? "died"
                          : reinterpret_cast<void*>(localPawn) != g_emotePawn ? "player changed"
                          : now - g_emoteStart >= g_emoteDur ? "finished"
                          : s_localSlot >= 0 && (fired >> s_localSlot) & 1 ? "shot"
                          : g_emoteDance >= 0 && FortniteDancesStatus() != DanceStatus::Ready ? "dance model gone"
                          : nullptr;
        if (!why && g_absVelOff >= 0 &&
            std::hypot(sdk::Read<float>(localPawn + g_absVelOff), sdk::Read<float>(localPawn + g_absVelOff + 4)) > 10.0f)
            why = "moved";
        if (why) StopEmote(why);
    } else {
        g_emoteStop = false;
    }
    const int dance = request - kDanceBase;
    const bool danceRequest = request >= kDanceBase && dance < FortniteDanceCount() &&
                              FortniteDancesStatus() == DanceStatus::Ready;
    const bool clipRequest = request >= 0 && request < kClipCount && g_clips[request];
    if ((clipRequest || danceRequest) && emoteOn && localAlive && !g_emoteActive.load()) {
        const float dur = danceRequest ? FortniteDanceLength(dance)  // INFINITY = until cancelled
                                       : sdk::Read<float>(reinterpret_cast<uintptr_t>(g_clips[request]) + g_clipDurOff);
        if (danceRequest || (dur >= 0.0f && std::isfinite(dur))) {
            g_emoteClip = danceRequest ? nullptr : g_clips[request];
            g_emoteDance = danceRequest ? dance : -1;
            g_emoteValue = request;
            g_emotePawn = reinterpret_cast<void*>(localPawn);
            g_emoteStart = now;
            g_emoteDur = danceRequest ? dur : dur < kStaticClip ? kPoseHold : dur;
            auto& tp = GetThirdpersonConfig();
            g_savedThird = tp.thirdperson.load();
            tp.thirdperson = true;  // the CAM_Think detour switches the view on the next frame
            g_emoteActive = true;
            journal().info("anim", std::string("emote started: ") + EmoteName(request));
        }
    }

    bool clipPlaying = false;
    if (g_emoteActive.load()) {
        if (g_emoteDance >= 0) {
            FortniteDancesWant(g_emotePawn, g_emoteDance, true);  // faces one way
        } else {
            SetSlot(g_emotePawn, g_emoteClip, g_emoteStart);
            clipPlaying = true;
        }
    }
    if (!clipPlaying && g_slot.pawn.load()) g_slot.pawn.store(nullptr, std::memory_order_release);
    g_active = clipPlaying;
}

void EmoteWheelDraw(ImDrawList* dl, float scroll) {
    auto& cfg = GetEmoteWheelConfig();
    static bool wasHeld = false, wasL = false, wasR = false, toggle = false;
    static double openedAt = 0.0;
    static float open = 0.0f, pop[kWheelSlots] = {};
    static int page = 0;  // kept across openings, like Fortnite
    const int key = cfg.emoteKey.load();
    const bool usable = dl && !g_releasing.load() && g_state.load() > 0 && g_canPlay.load() && cfg.emoteEnabled.load() &&
                        ThirdpersonAvailable() && key > 0 && key < 0xFF;
    const HWND hwnd = static_cast<HWND>(sdk::Data().gameWindow);
    const bool foreground = hwnd && GetForegroundWindow() == hwnd;
    const bool held = usable && foreground && (GetAsyncKeyState(key) & 0x8000);
    const bool lDown = foreground && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    const bool rDown = foreground && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    const bool press = held && !wasHeld, release = !held && wasHeld, lClick = lDown && !wasL, rClick = rDown && !wasR;
    wasHeld = held, wasL = lDown, wasR = rDown;
    const bool cancel = g_wheelCancel.exchange(false);
    const bool fortnite = FortniteDancesStatus() == DanceStatus::Ready;
    const int pages = WheelPageCount(fortnite);
    if (page >= pages) page = 0;
    const double now = Now();
    auto close = [&] {
        g_wheelOpen = false;
        toggle = false;
    };

    if (!g_wheelOpen.load()) {
        if (!press) return;
        if (g_emoteActive.load()) {
            g_emoteStop = true;  // a tap while an emote plays stops it
            return;
        }
        if (!g_localAlive.load()) return;
        g_wheelOpen = true;
        toggle = false;
        openedAt = now;
        open = 0.0f;
        for (float& p : pop) p = 0.0f;
        CenterCursor(hwnd);
    }
    // Right-click, Esc, the key again (after a tap) or anything that makes it unusable closes it.
    if (!usable || !foreground || !g_localAlive.load() || cancel || rClick || (toggle && press)) {
        close();
        return;
    }

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    open = std::min(1.0f, open + dt / 0.14f);
    const float eased = 1.0f - (1.0f - open) * (1.0f - open) * (1.0f - open);
    const WheelGeom g = GeomFor(eased);
    const ImVec2 cur = CursorInBackbuffer(hwnd);
    const int arrow = pages > 1 ? HotArrow(g, cur) : -1;
    const int hot = arrow >= 0 ? -1 : HotSlot(g, cur);
    if (scroll != 0.0f && pages > 1) page = (page + (scroll < 0.0f ? 1 : pages - 1)) % pages;  // down = next, wraps
    if (lClick && arrow >= 0) page = (page + (arrow ? 1 : pages - 1)) % pages;
    const WheelPage pg = WheelPageAt(page, fortnite);
    const int value = hot >= 0 ? pg.values[hot] : -1;
    if ((lClick && arrow < 0 && WheelSelectable(value, fortnite)) ||
        (release && !toggle && WheelSelectable(value, fortnite))) {  // click it, or let go of the key on it
        g_emoteRequest = value;
        close();
        return;
    }
    if (release && !toggle) {
        if (now - openedAt >= 0.3) {  // held and let go on nothing
            close();
            return;
        }
        toggle = true;  // a tap: stays open until a click, right-click, Esc or the key again
    }
    for (int k = 0; k < kWheelSlots; ++k)
        pop[k] += ((k == hot && pg.values[k] >= 0 ? 1.0f : 0.0f) - pop[k]) * std::min(1.0f, dt * 16.0f);
    DrawWheel(dl, pg, page, pages, hot, arrow, eased, pop, cur, true, fortnite, 2);
}

bool EmoteWheelWantsCursor() { return g_wheelOpen.load(); }

void EmoteWheelCancel() {
    if (g_wheelOpen.load()) g_wheelCancel = true;
}

void EmoteWheelReleaseTextures() {
    for (IconSlot& s : g_icons) {
        if (s.srv) s.srv->Release();
        s = IconSlot{};
    }
}

// Shooting and a new round stop your emote; EmoteWheelFrame reads both.
void EmoteWheelOnGameEvent(const char* name, int userid) {
    if (!name || !g_emoteActive.load()) return;
    if (!std::strcmp(name, "weapon_fire")) {
        if (userid >= 0 && userid < 64) g_firedMask.fetch_or(1ull << userid);
    } else if (!std::strcmp(name, "round_start")) {
        g_emoteStop = true;
    }
}

}  // namespace emotes
