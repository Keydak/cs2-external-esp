#include "sdk/ResourceManifest.h"
#include "core/Journal.h"
#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace emotes::sdk {

namespace {

// engine2.dll IGameResourceService (1 match each in IDA and on disk): vt[27] CreateGameResourceManifest (0x1801B1780),
// vt[29] IsManifestLoaded (0x1801B2160, true for null), vt[31] DestroyResourceManifest (0x1801B1E50).
constexpr const char* kCreateManifestPattern  = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 83 79 58 00 45 0F B6 F1 49 8B D8 8B F2 48 8B F9";
constexpr const char* kManifestLoadedPattern  = "48 85 D2 74 0C 48 83 BA A0 00 00 00 00 0F 94 C0 C3 B0 01 C3";
constexpr const char* kDestroyManifestPattern = "48 85 D2 74 4D 53 48 83 EC 20 48 8B 8A A0 00 00 00 48 8B DA 48 85 C9 74";

using CreateManifestFn  = void*(__fastcall*)(void* grs, int count, const char* const* names, uint8_t loadBehavior,
                                             const char* reason, uint8_t priority);
using ManifestLoadedFn  = bool(__fastcall*)(void* grs, void* manifest);
using DestroyManifestFn = void(__fastcall*)(void* grs, void* manifest);
using MemAllocFn        = void*(__fastcall*)(void* self, size_t size);

void* g_grs = nullptr;
const char* g_reason = nullptr;
CreateManifestFn g_create = nullptr;
ManifestLoadedFn g_loaded = nullptr;
DestroyManifestFn g_destroy = nullptr;
int g_state = 0;  // 0 unresolved, 1 ready, -1 unavailable

bool Resolve() {
    if (g_state) return g_state > 0;
    g_state = -1;
    void* grs = nullptr;
    if (HMODULE engine = GetModuleHandleA("engine2.dll")) {
        using CreateInterfaceFn = void*(*)(const char*, int*);
        if (auto ci = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")))
            grs = ci("GameResourceServiceClientV001", nullptr);
    }
    // A vtable slot that is not the expected method means a new layout: never call an unknown method.
    auto slot = [grs](int index, const char* p) {
        const uintptr_t hit = grs ? PatternScan("engine2.dll", p) : 0;
        return hit && Read<uintptr_t>(Read<uintptr_t>(reinterpret_cast<uintptr_t>(grs)) + index * 8) == hit ? hit : 0;
    };
    const uintptr_t create = slot(27, kCreateManifestPattern), loaded = slot(29, kManifestLoadedPattern),
                    destroy = slot(31, kDestroyManifestPattern);
    // vt[27] hands the raw reason pointer to the resource system, which may keep it past our unload.
    // ponytail: 8-byte tier0 copy, intentionally never freed so it outlives emotes.dll
    void* memAlloc = nullptr;
    if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
        if (auto p = reinterpret_cast<uintptr_t>(GetProcAddress(tier0, "g_pMemAlloc"))) memAlloc = Read<void*>(p);
    char* reason = nullptr;
    if (create && loaded && destroy && memAlloc) {
        reason = static_cast<char*>(reinterpret_cast<MemAllocFn>((*reinterpret_cast<void***>(memAlloc))[1])(memAlloc, 8));
        if (reason) std::memcpy(reason, "emotes", 7);
    }
    if (!reason) {
        journal().warn("resources", "model loading unavailable: GameResourceService or g_pMemAlloc not found");
        return false;
    }
    g_grs = grs;
    g_reason = reason;
    g_create = reinterpret_cast<CreateManifestFn>(create);
    g_loaded = reinterpret_cast<ManifestLoadedFn>(loaded);
    g_destroy = reinterpret_cast<DestroyManifestFn>(destroy);
    g_state = 1;
    return true;
}

}  // namespace

void* GameResourceService() { return Resolve() ? g_grs : nullptr; }

const char* ResourceReason() { return Resolve() ? g_reason : nullptr; }

void* ManifestCreate(const char* const* names, int count) {
    if (!names || count <= 0 || !Resolve()) return nullptr;
    __try {
        return g_create(g_grs, count, names, 1, g_reason, 2);  // same args as C_CSGO_PreviewModel
    } __except(1) {
        return nullptr;  // ponytail: a fault here may leak the manifest
    }
}

bool ManifestLoaded(void* manifest) {
    if (!manifest || g_state <= 0) return false;
    __try {
        return g_loaded(g_grs, manifest);
    } __except(1) {
        return false;
    }
}

// Safe mid-load (vt[31] unhooks the load callback first) and while entities still use the models: each model's
// CStrongHandle keeps it resident until its entity is destroyed.
void ManifestDestroy(void* manifest) {
    if (!manifest || g_state <= 0) return;
    __try {
        g_destroy(g_grs, manifest);
    } __except(1) {
    }
}

}  // namespace emotes::sdk
