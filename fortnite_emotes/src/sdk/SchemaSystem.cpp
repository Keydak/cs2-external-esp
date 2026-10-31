#include "sdk/SchemaSystem.h"
#include "core/Journal.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace emotes::sdk {
namespace {

void* g_schemaSystem = nullptr;
void* g_clientScope  = nullptr;
std::unordered_map<std::string, int32_t> g_offsetCache;
std::mutex g_cacheMutex;

template <typename T>
T ReadMem(uintptr_t addr) {
    __try { return *reinterpret_cast<T*>(addr); }
    __except(1) { return T{}; }
}

template <typename Ret, typename... Args>
Ret CallVFunc(void* instance, int index, Args... args) {
    auto vtable = *reinterpret_cast<void***>(instance);
    auto fn = reinterpret_cast<Ret(__fastcall*)(void*, Args...)>(vtable[index]);
    return fn(instance, args...);
}

// vtable[13] on SchemaSystem = FindTypeScopeForModule
void* FindTypeScope(const char* moduleName) {
    if (!g_schemaSystem) return nullptr;
    __try {
        return CallVFunc<void*>(g_schemaSystem, 13, moduleName, nullptr);
    } __except(1) {}
    return nullptr;
}

// vtable[2] on TypeScope = FindDeclaredClass
// Returns class info pointer with fields at +0x30, count at +0x24
uintptr_t FindDeclaredClass(void* scope, const char* className) {
    if (!scope) return 0;
    __try {
        uintptr_t classInfo = 0;
        CallVFunc<void>(scope, 2, &classInfo, className);
        return classInfo;
    } __except(1) {}
    return 0;
}

bool SafeCopyStr(const char* src, char* dst, size_t n) {
    __try {
        strncpy_s(dst, n, src, _TRUNCATE);
        return dst[0] != '\0';
    } __except(1) { return false; }
}

// Walk fields of a class and cache them all
int CacheClassFields(const char* module, const char* className, uintptr_t classInfo) {
    uint16_t fieldCount = ReadMem<uint16_t>(classInfo + 0x24);
    uintptr_t fields    = ReadMem<uintptr_t>(classInfo + 0x30);
    if (!fields || fieldCount == 0 || fieldCount > 500) return 0;

    int cached = 0;
    char name[128];
    for (uint16_t i = 0; i < fieldCount; ++i) {
        uintptr_t fieldAddr = fields + i * 0x20;
        const char* fieldName = ReadMem<const char*>(fieldAddr + 0x00);
        int32_t     fieldOff  = ReadMem<int32_t>(fieldAddr + 0x10);
        if (!fieldName || fieldOff < 0 || fieldOff > 0xFFFF || !SafeCopyStr(fieldName, name, sizeof(name)))
            continue;
        g_offsetCache[std::string(module) + "::" + className + "::" + name] = fieldOff;
        ++cached;
    }
    return cached;
}

// Safety net when the schema walk fails; verified against client.dll static schema tables 2026-09-23.
int32_t HardcodedFallback(const char* className, const char* fieldName) {
    struct Entry { const char* cls; const char* field; int32_t off; };
    static const Entry kTable[] = {
        {"C_BaseEntity",          "m_pGameSceneNode", 0x330},
        {"C_BaseEntity",          "m_iHealth",        0x34C},
        {"C_BaseEntity",          "m_iTeamNum",       0x3E7},
        {"C_BaseEntity",          "m_hOwnerEntity",   0x520},
        {"CGameSceneNode",        "m_vecAbsOrigin",   0xC8},
        {"CGameSceneNode",        "m_bDormant",       0x103},
        {"CGameSceneNode",        "m_pOwner",         0x30},
        {"CGameSceneNode",        "m_pParent",        0x38},
        {"CCSPlayerController",   "m_hPlayerPawn",    0x92C},
        {nullptr, nullptr, 0},
    };
    for (const Entry* e = kTable; e->cls; ++e)
        if (std::strcmp(className, e->cls) == 0 && std::strcmp(fieldName, e->field) == 0) return e->off;
    return -1;
}

// On-demand: look up a single class, cache all its fields
int32_t LookupAndCache(const char* module, const char* className, const char* fieldName) {
    // Each module declares its own classes: client.dll's scope does not know animationsystem.dll's.
    const bool client = strcmp(module, "client.dll") == 0;
    void* scope = client && g_clientScope ? g_clientScope : FindTypeScope(module);
    if (client && !g_clientScope) g_clientScope = scope;
    if (!scope) return -1;

    uintptr_t classInfo = FindDeclaredClass(scope, className);
    if (!classInfo) return -1;

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    CacheClassFields(module, className, classInfo);

    std::string key = std::string(module) + "::" + className + "::" + fieldName;
    auto it = g_offsetCache.find(key);
    if (it != g_offsetCache.end()) return it->second;
    g_offsetCache[key] = -1;
    return -1;
}

}  // namespace

bool InitSchemaSystem() {
    HMODULE hSchema = GetModuleHandleA("schemasystem.dll");
    if (!hSchema) {
        journal().warn("schema", "schemasystem.dll not loaded");
        return false;
    }
    using CreateInterfaceFn = void* (*)(const char*, int*);
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(hSchema, "CreateInterface"));
    if (!createInterface) return false;
    g_schemaSystem = createInterface("SchemaSystem_001", nullptr);
    if (!g_schemaSystem) g_schemaSystem = createInterface("SchemaSystem_002", nullptr);
    if (!g_schemaSystem) {
        journal().warn("schema", "SchemaSystem interface not found");
        return false;
    }
    g_clientScope = FindTypeScope("client.dll");
    return g_clientScope != nullptr;
}

int32_t GetSchemaOffset(const char* module, const char* className, const char* fieldName) {
    std::string key = std::string(module) + "::" + className + "::" + fieldName;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_offsetCache.find(key);
        if (it != g_offsetCache.end()) return it->second;
    }

    int32_t result = LookupAndCache(module, className, fieldName);
    if (result >= 0) return result;

    result = HardcodedFallback(className, fieldName);
    if (result >= 0) {
        journal().warn("schema", "using hardcoded fallback for " + key);
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_offsetCache[key] = result;
        return result;
    }
    journal().warn("schema", "failed to resolve " + key);
    return -1;
}

}  // namespace emotes::sdk
