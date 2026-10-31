#include "sdk/GameEvents.h"
#include "core/Journal.h"
#include "features/EmoteWheel.h"
#include "sdk/GameState.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace emotes::sdk {
namespace {

const char* EventGetName(void* event) {
    __try {
        auto vtable = *reinterpret_cast<void***>(event);
        using Fn = const char*(__fastcall*)(void*);
        return reinterpret_cast<Fn>(vtable[1])(event);
    } __except(1) {}
    return nullptr;
}

struct EventKey {
    uint32_t    hash;
    int32_t     unk;
    const char* name;
};

// MurmurHash2 over the lower-cased key, seed 0x31415926 ^ len (client.dll sub_1801657F0)
EventKey MakeEventKey(const char* s) {
    constexpr uint32_t m = 0x5BD1E995;
    auto lc = [](uint8_t c) -> uint32_t { return static_cast<uint8_t>(c - 'A') < 26 ? c + 32u : c; };
    auto p = reinterpret_cast<const uint8_t*>(s);
    uint32_t len = static_cast<uint32_t>(strlen(s));
    uint32_t h = 0x31415926u ^ len;
    for (; len >= 4; len -= 4, p += 4) {
        uint32_t k = lc(p[0]) | lc(p[1]) << 8 | lc(p[2]) << 16 | lc(p[3]) << 24;
        k *= m; k ^= k >> 24; k *= m;
        h = h * m ^ k;
    }
    switch (len) {
        case 3: h ^= lc(p[2]) << 16; [[fallthrough]];
        case 2: h ^= lc(p[1]) << 8;  [[fallthrough]];
        case 1: h ^= lc(p[0]); h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return {h, -1, s};
}

int EventGetInt(void* event, const char* key, int def = 0) {
    EventKey k = MakeEventKey(key);
    __try {
        auto vtable = *reinterpret_cast<void***>(event);
        using Fn = int(__fastcall*)(void*, const EventKey*, int);
        return reinterpret_cast<Fn>(vtable[7])(event, &k, def);
    } __except(1) {}
    return def;
}

// CS2 IGameEventListener2 vtable layout:
//   [0] destructor (unused, nullptr)
//   [1] FireGameEvent
//   [2] GetEventDebugID
// A C++ virtual destructor occupies TWO vtable slots on MSVC/Clang-CL,
// which shifts FireGameEvent to [2] and breaks everything. Build manually.
void* __fastcall listenerFireEvent(void*, void* event) {
    if (!event) return nullptr;
    char name[64] = {};
    __try {
        const char* n = EventGetName(event);
        if (n) strncpy_s(name, n, _TRUNCATE);
    } __except(1) {}
    if (name[0]) EmoteWheelOnGameEvent(name, EventGetInt(event, "userid"));
    return nullptr;
}

int __fastcall listenerGetDebugID(void*) { return 42; }

void* g_listenerVtable[3] = {
    nullptr,
    reinterpret_cast<void*>(&listenerFireEvent),
    reinterpret_cast<void*>(&listenerGetDebugID),
};

struct GameEventListener {
    void* vtable_ptr = g_listenerVtable;
    int   debug_id   = 42;
};

GameEventListener g_listener;
bool g_installed = false;

bool ManagerAddListener(void* manager, void* listener, const char* name) {
    __try {
        auto vtable = *reinterpret_cast<void***>(manager);
        using Fn = int(__fastcall*)(void*, void*, const char*, bool);
        return reinterpret_cast<Fn>(vtable[3])(manager, listener, name, false) != -1;
    } __except(1) {}
    return false;
}

void ManagerRemoveListener(void* manager, void* listener) {
    __try {
        auto vtable = *reinterpret_cast<void***>(manager);
        using Fn = void(__fastcall*)(void*, void*);
        reinterpret_cast<Fn>(vtable[5])(manager, listener);
    } __except(1) {}
}

}  // namespace

void InstallGameEventListener() {
    auto& d = Data();
    if (!d.gameEventManager) return;
    static const char* kEvents[] = {"weapon_fire", "round_start"};
    int count = 0;
    for (const char* ev : kEvents)
        if (ManagerAddListener(d.gameEventManager, &g_listener, ev)) ++count;
    g_installed = true;
    if (count < static_cast<int>(std::size(kEvents)))
        journal().warn("events", "listening to " + std::to_string(count) + " of 2 game events");
}

void RemoveGameEventListener() {
    if (!g_installed) return;
    if (auto& d = Data(); d.gameEventManager) ManagerRemoveListener(d.gameEventManager, &g_listener);
    g_installed = false;
}

}  // namespace emotes::sdk
