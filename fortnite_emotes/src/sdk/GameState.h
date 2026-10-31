#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace emotes::sdk {

struct GameData {
    std::atomic<bool> initialized{false};

    // Interfaces
    void* entitySystem     = nullptr;
    void* cvarSystem       = nullptr;
    void* engineClient     = nullptr;
    void* gameEventManager = nullptr;  // IGameEventManager2
    void* inputSystem      = nullptr;  // cursor hooks
    void* ccsgoInput       = nullptr;  // CCSGOInput (third person)

    // client.dll
    uintptr_t fnFrameStageNotify   = 0;
    uintptr_t fnGetEntityByIndex   = 0;
    uintptr_t fnGetLocalController = 0;
    uintptr_t fnLevelShutdown      = 0;
    uintptr_t fnOnRemoveEntity     = 0;
    uintptr_t fnMouseInputEnabled  = 0;
    // engine2.dll
    uintptr_t fnIsInGame = 0;
    // scenesystem.dll
    uintptr_t fnOnGeneratePrimitives = 0;

    void* gameWindow = nullptr;  // set by the Present hook
};

GameData& Data();

// Scan patterns, resolve interfaces. From the DLL's init thread.
bool Initialize();
void Shutdown();

void* GetEntityByIndex(int index);
void* GetLocalController();
void* GetLocalPawn();  // C_CSPlayerPawn (controller m_hPlayerPawn), also while dead
bool  IsInGame();

// The game's cvar list (VEngineCvar007), no cache; null if absent. Value at +0x58.
void* FindConVar(const char* name);

// Safe memory read — returns default on failure
template <typename T>
T Read(uintptr_t addr) {
    __try {
        if (addr) return *reinterpret_cast<T*>(addr);
    } __except(1) {}
    return T{};
}

template <typename T>
void Write(uintptr_t addr, T value) {
    __try {
        if (addr) *reinterpret_cast<T*>(addr) = value;
    } __except(1) {}
}

}  // namespace emotes::sdk
