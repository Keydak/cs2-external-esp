#include "hooks/GameHooks.h"
#include "hooks/Hooks.h"
#include "core/Journal.h"
#include "features/EmoteWheel.h"
#include "features/FortniteDances.h"
#include "features/Music.h"
#include "features/Thirdperson.h"
#include "sdk/GameEvents.h"
#include "sdk/GameState.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <string>

namespace emotes::hooks {
namespace {

// FrameStageNotify hook
using FrameStageNotifyFn = void(__fastcall*)(void* self, int stage);
FrameStageNotifyFn g_origFSN = nullptr;

void __fastcall HkFrameStageNotify(void* self, int stage) {
    if (stage == 5 && FortniteDancesNeedDrawHook()) InstallDrawHookIfNeeded();  // the dances hide the real player there
    g_origFSN(self, stage);
    // Stage 12 runs the render-time interpolation (SetInterpolationPhase "rendering"); writes after it reach the frame.
    if (stage == 12) {
        EmoteWheelFrame();
        FortniteDancesFrame();
    }
}

using LevelShutdownFn = void*(__fastcall*)(void* self);
LevelShutdownFn g_origLevelShutdown = nullptr;

void* __fastcall HkLevelShutdown(void* self) {
    EmoteWheelLevelShutdown();
    FortniteDancesLevelShutdown();
    return g_origLevelShutdown(self);
}

using OnEntityFn = void*(__fastcall*)(void* entitySystem, void* ent, uint32_t handle);
OnEntityFn g_origOnRemoveEntity = nullptr;

void* __fastcall HkOnRemoveEntity(void* entitySystem, void* ent, uint32_t handle) {
    FortniteDancesOnEntityRemoved(ent);
    return g_origOnRemoveEntity(entitySystem, ent, handle);
}

// ──────────────────────────────────────────────── Draw hook
std::atomic<bool> g_drawHooked{false};
std::atomic<bool> g_drawDisabled{false};

using OnGenPrimsFn = void(__fastcall*)(void*, void*, void*, void*);
OnGenPrimsFn g_origOnGenPrims = nullptr;

void SafeCallOriginal(void* pThis, void* sceneObject, void* pUnk, void* primitiveBuffer) {
    __try {
        g_origOnGenPrims(pThis, sceneObject, pUnk, primitiveBuffer);
    } __except(1) {}
}

void __fastcall HkOnGenPrims(void* pThis, void* sceneObject, void* pUnk, void* primitiveBuffer) {
    if (FortniteDancesHides(sceneObject)) return;  // a dancing player: its stand-in draws instead
    SafeCallOriginal(pThis, sceneObject, pUnk, primitiveBuffer);
}

bool InstallSingleHook(const char* name, uintptr_t target, void* detour, void** original) {
    if (!target) return false;
    if (MH_CreateHook(reinterpret_cast<void*>(target), detour, original) != MH_OK) {
        journal().warn("hooks", std::string("failed to hook ") + name);
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        *original = nullptr;
        journal().warn("hooks", std::string("failed to enable the ") + name + " hook");
        return false;
    }
    ::emotes::hooks::RecordPrologue(reinterpret_cast<void*>(target));
    return true;
}

struct ExtraHook { uintptr_t target; };
ExtraHook g_extraHooks[16];
int g_extraHookCount = 0;
// HookInstaller for feature units: installs + enables, remembers the target for RemoveGameHooks.
bool InstallFeatureHook(const char* name, uintptr_t target, void* detour, void** original) {
    if (g_extraHookCount >= 16 || !InstallSingleHook(name, target, detour, original)) return false;
    g_extraHooks[g_extraHookCount++] = {target};
    return true;
}

}  // namespace

bool DrawHookFailed() { return !g_drawHooked.load() && g_drawDisabled.load(); }

void InstallDrawHookIfNeeded() {
    if (g_drawHooked.load() || g_drawDisabled.load()) return;
    const uintptr_t target = sdk::Data().fnOnGeneratePrimitives;
    if (!target) journal().warn("hooks", "GenerateScenePrimitives pattern not found: dances unavailable");
    if (!target || !InstallSingleHook("GenerateScenePrimitives", target, reinterpret_cast<void*>(&HkOnGenPrims),
                                      reinterpret_cast<void**>(&g_origOnGenPrims))) {
        g_drawDisabled.store(true);
        return;
    }
    g_drawHooked.store(true);
}

bool InstallGameHooks() {
    auto& data = sdk::Data();
    if (!data.initialized) return false;

    const bool ok = InstallSingleHook("FrameStageNotify", data.fnFrameStageNotify, reinterpret_cast<void*>(&HkFrameStageNotify),
                                      reinterpret_cast<void**>(&g_origFSN));
    InstallSingleHook("LevelShutdown", data.fnLevelShutdown, reinterpret_cast<void*>(&HkLevelShutdown),
                      reinterpret_cast<void**>(&g_origLevelShutdown));
    InstallSingleHook("OnRemoveEntity", data.fnOnRemoveEntity, reinterpret_cast<void*>(&HkOnRemoveEntity),
                      reinterpret_cast<void**>(&g_origOnRemoveEntity));
    EmoteWheelInstallHooks(&InstallFeatureHook);   // ApplyAnimGraphPose
    ThirdpersonInstallHooks(&InstallFeatureHook);  // CAM_Think
    sdk::InstallGameEventListener();
    if (!ok) journal().warn("hooks", "FrameStageNotify not hooked: the emotes can't run");
    return ok;
}

bool RemoveGameHooks() {
    g_drawDisabled.store(true);
    EmoteWheelRelease();
    FortniteDancesRelease();  // removes the props via the still-hooked stage 12
    ThirdpersonRelease();     // after EmoteWheelRelease put the emote's setting back

    Sleep(150);

    sdk::RemoveGameEventListener();
    MusicShutdown();

    auto& data = sdk::Data();
    bool ok = true;
    auto safe = [&](void* orig, uintptr_t target) {
        if (orig && target) ok &= ::emotes::hooks::SafeRemoveHook(reinterpret_cast<void*>(target));
    };
    safe(g_origOnGenPrims,     data.fnOnGeneratePrimitives);
    safe(g_origFSN,            data.fnFrameStageNotify);
    safe(g_origLevelShutdown,  data.fnLevelShutdown);
    safe(g_origOnRemoveEntity, data.fnOnRemoveEntity);
    for (int i = 0; i < g_extraHookCount; ++i)  // the feature units' hooks (InstallFeatureHook)
        ok &= ::emotes::hooks::SafeRemoveHook(reinterpret_cast<void*>(g_extraHooks[i].target));
    g_extraHookCount = 0;
    return ok;
}

}  // namespace emotes::hooks
