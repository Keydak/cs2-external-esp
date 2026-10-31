#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include "sdk/SchemaSystem.h"
#include "sdk/Signatures.h"
#include "core/Journal.h"
#include <windows.h>
#include <cstring>
#include <string>

namespace emotes::sdk {
namespace {

GameData g_data;
int g_scanFailedCount = 0;
std::string g_scanFailed;

void* GetInterface(const char* moduleName, const char* interfaceName) {
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) return nullptr;
    using CreateInterfaceFn = void* (*)(const char*, int*);
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(hModule, "CreateInterface"));
    return createInterface ? createInterface(interfaceName, nullptr) : nullptr;
}

void LogScan(const char* name, uintptr_t addr) {
    if (addr) return;
    if (g_scanFailedCount++) g_scanFailed += ", ";
    g_scanFailed += name;
}

void* SafeDeref(void* ptr) {
    __try {
        if (ptr) return *reinterpret_cast<void**>(ptr);
    } __except(1) {}
    return nullptr;
}

void* GlobalAt(const char* module, const char* pattern) {
    const uintptr_t scan = PatternScan(module, pattern);
    return scan ? SafeDeref(reinterpret_cast<void*>(ResolveRelative(scan, 3, 7))) : nullptr;
}

void ScanPatterns() {
    auto& d = g_data;
    d.fnFrameStageNotify = PatternScan("client.dll", FRAMESTAGENOTIFY_PATTERN);
    LogScan("FrameStageNotify", d.fnFrameStageNotify);
    d.fnGetEntityByIndex = PatternScan("client.dll", GETENTITYBYINDEX_PATTERN);
    LogScan("GetEntityByIndex", d.fnGetEntityByIndex);
    d.fnGetLocalController = PatternScan("client.dll", GETLOCALCONTROLLER_PATTERN);
    LogScan("GetLocalController", d.fnGetLocalController);
    d.fnLevelShutdown = PatternScan("client.dll", LEVELSHUTDOWN_PATTERN);
    LogScan("LevelShutdown", d.fnLevelShutdown);
    d.fnOnRemoveEntity = PatternScan("client.dll", ONREMOVEENTITY_PATTERN);
    LogScan("OnRemoveEntity", d.fnOnRemoveEntity);
    d.fnMouseInputEnabled = PatternScan("client.dll", MOUSEINPUTENABLED_PATTERN);
    LogScan("MouseInputEnabled", d.fnMouseInputEnabled);
    d.fnIsInGame = PatternScan("engine2.dll", ISINGAME_PATTERN);
    LogScan("IsInGame", d.fnIsInGame);
    d.fnOnGeneratePrimitives = PatternScan("scenesystem.dll", GENERATEPRIMITIVES_PATTERN);
    LogScan("GeneratePrimitives", d.fnOnGeneratePrimitives);
    if (g_scanFailedCount) journal().warn("sdk", "patterns not found: " + g_scanFailed);
}

void ResolveInterfaces() {
    auto& d = g_data;
    // CGameEntitySystem from GameResourceServiceClientV001 + 0x58
    if (void* grs = GetInterface("engine2.dll", "GameResourceServiceClientV001"))
        d.entitySystem = SafeDeref(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(grs) + 0x58));
    if (!d.entitySystem) d.entitySystem = GlobalAt("client.dll", ENTITYSYSTEM_PATTERN);
    d.cvarSystem = GetInterface("tier0.dll", "VEngineCvar007");
    d.engineClient = GetInterface("engine2.dll", "Source2EngineToClient001");
    d.gameEventManager = GlobalAt("client.dll", GAMEEVENTMANAGER_PATTERN);
    d.inputSystem = GetInterface("inputsystem.dll", "InputSystemVersion001");
    d.ccsgoInput = GlobalAt("client.dll", CSGOINPUT_PATTERN);
    if (!d.entitySystem) journal().warn("sdk", "entity system not found");
    if (!d.gameEventManager) journal().warn("sdk", "game event manager not found: shooting and a new round won't stop an emote");
    if (!d.ccsgoInput) journal().warn("sdk", "CCSGOInput not found: no third person");
}

}  // namespace

GameData& Data() { return g_data; }

bool Initialize() {
    if (g_data.initialized.load()) return true;

    bool waited = false;  // injected before the game had it loaded
    for (int i = 0; i < 300 && !GetModuleHandleA("client.dll"); ++i) {
        waited = true;
        Sleep(100);
    }
    if (!GetModuleHandleA("client.dll")) return false;
    if (waited) Sleep(500);  // just loaded: let it finish setting up

    if (!InitSchemaSystem()) journal().warn("sdk", "schema system failed to initialize");
    ResolveInterfaces();
    ScanPatterns();

    g_data.initialized.store(true);
    return true;
}

void Shutdown() { g_data.initialized.store(false); }

void* GetEntityByIndex(int index) {
    auto& d = g_data;
    if (!d.initialized.load() || !d.entitySystem) return nullptr;

    auto valid = [](void* p) {
        auto a = reinterpret_cast<uintptr_t>(p);
        return a > 0x10000 && a < 0x7FFF'FFFF'FFFFull;
    };

    if (d.fnGetEntityByIndex) {
        __try {
            using Fn = void*(__fastcall*)(void*, int);
            void* r = reinterpret_cast<Fn>(d.fnGetEntityByIndex)(d.entitySystem, index);
            return valid(r) ? r : nullptr;
        } __except(1) {}
        return nullptr;
    }

    // Same walk as the game: inline chunk array at +0x10, 512 x 0x70-byte identities per chunk
    if (static_cast<uint32_t>(index) > 0x7FFE) return nullptr;
    uintptr_t chunk = Read<uintptr_t>(reinterpret_cast<uintptr_t>(d.entitySystem) + 0x10 + (index >> 9) * 8);
    if (!chunk) return nullptr;
    uintptr_t identity = chunk + static_cast<uintptr_t>(index & 0x1FF) * 0x70;
    if ((Read<uint32_t>(identity + 0x10) & 0x7FFF) != static_cast<uint32_t>(index)) return nullptr;
    void* r = Read<void*>(identity);
    return valid(r) ? r : nullptr;
}

void* GetLocalController() {
    if (!g_data.initialized.load() || !g_data.fnGetLocalController) return nullptr;
    __try {
        using Fn = void*(__fastcall*)(int);
        return reinterpret_cast<Fn>(g_data.fnGetLocalController)(0);
    } __except(1) {}
    return nullptr;
}

void* GetLocalPawn() {
    static int32_t s_pawnOff = -1;
    void* controller = GetLocalController();
    if (!controller) return nullptr;
    if (s_pawnOff < 0) s_pawnOff = GetSchemaOffset("client.dll", "CCSPlayerController", "m_hPlayerPawn");
    if (s_pawnOff < 0) return nullptr;
    const uint32_t pawnHandle = Read<uint32_t>(reinterpret_cast<uintptr_t>(controller) + s_pawnOff);
    if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFF) return nullptr;
    return GetEntityByIndex(pawnHandle & 0x7FFF);
}

static bool CallEngineBool(uintptr_t fn, bool& out) {
    if (!fn) return false;
    __try {
        out = reinterpret_cast<bool(__fastcall*)(void*)>(fn)(g_data.engineClient);
        return true;
    } __except(1) {}
    return false;
}

bool IsInGame() {
    if (!g_data.initialized.load()) return false;
    bool r = false;
    if (CallEngineBool(g_data.fnIsInGame, r)) return r;
    return GetLocalController() != nullptr;
}

// CCvar (tier0 VEngineCvar007) holds a CUtlLinkedList<ConVarData*, uint16>: element array at +0x50, head index
// (uint16) at +0x58, 0xFFFF terminates. Each entry is 0x10 bytes: the cvar at +0, the next index at +0xA.
// A cvar's name is at +0.
void* FindConVar(const char* name) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_data.cvarSystem);
    if (!g_data.initialized.load() || !base) return nullptr;
    __try {
        const uintptr_t container = *reinterpret_cast<uintptr_t*>(base + 0x50);
        if (!container) return nullptr;
        uint16_t idx = *reinterpret_cast<uint16_t*>(base + 0x58);
        for (int safety = 0; safety < 0x10000 && idx != 0xFFFF; ++safety) {
            const uintptr_t entry = container + idx * 0x10ull;
            const uintptr_t cv = *reinterpret_cast<uintptr_t*>(entry);
            const char* cvName = cv ? *reinterpret_cast<const char**>(cv) : nullptr;
            if (cvName && std::strcmp(cvName, name) == 0) return reinterpret_cast<void*>(cv);
            idx = *reinterpret_cast<uint16_t*>(entry + 0xA);
        }
    } __except(1) {}
    return nullptr;
}

}  // namespace emotes::sdk
