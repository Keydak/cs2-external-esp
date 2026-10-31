#include "features/ClientProp.h"
#include "core/Journal.h"
#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include "sdk/ResourceManifest.h"
#include "sdk/SchemaSystem.h"
#include "sdk/Signatures.h"
#include <windows.h>
#include <cstdio>

namespace emotes {
namespace {

// client.dll (IDA-verified, each unique in the old and current build). The spawn mirrors the portrait-world
// spawner sub_18016A830: KV block -> CreateEntityByName -> Precache -> QueueSpawnEntity -> ExecuteQueuedCreation.
constexpr const char* kKvCtorPattern      = "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8B D9 48 89 79 28 48 89 79 30 44 88 41 25";
constexpr const char* kKvDtorPattern      = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 79 08 48 8B D9 48 85 FF 74 27";
constexpr const char* kKvFindPattern      = "48 83 EC 38 8B 02 45 33 C0 89 44 24 20 8B 42 04 89 44 24 24 48 8B 42 08 48 8D 54 24 20 48 89 44 24 28 E8 ? ? ? ? 48 83 C4 38 C3";
constexpr const char* kKvSetStringPattern = "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 83 39 00 49 8B F0 48 8B EA 48 8B D9";
constexpr const char* kCreateByNamePattern = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 50 49 8B F8 44 8B F2 33 DB";
constexpr const char* kQueueSpawnPattern  = "40 56 57 41 56 48 83 EC 30 F7 42 30 00 08 00 00 49 8B F0 4C 8B F2 48 8B F9";  // identity form
constexpr const char* kExecQueuedPattern  = "48 89 5C 24 08 57 48 81 EC 00 01 00 00 FF 81 48 0C 00 00 48 8D 44 24 28";
constexpr const char* kUtilRemovePattern  = "48 83 EC 28 48 85 C9 0F 84 ? ? ? ? 48 8B 51 10 8B 42 30 0F BA E0 0B";
constexpr const char* kSetOriginPattern   = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B D9 48 8B F2 48 83 E9 80 E8 ? ? ? ? 84 C0";
constexpr const char* kSpawnGroupCallPattern = "48 8B 0D ? ? ? ? 48 85 C9 74 0E E8 ? ? ? ? 48 8B 0D";  // E8 at +12 -> mov eax,[rcx+1FC0h]; ret
// engine2.dll IGameResourceService::PrecacheEntitiesAndConfirmResourcesAreLoaded (0x1801B1280); vtable[35] must be it.
constexpr const char* kPrecachePattern = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC F0 00 00 00 48 83 79 58 00 49 8B F1 41 8B F8 8B EA 48 8B D9 74 ? 45 85 C0 74";
// vt[35] only confirms: it never registers a model the map's manifest lacks. So the dance model is loaded by name
// first, like C_CSGO_PreviewModel, through sdk/ResourceManifest.

// CKV3MemberName hash: MurmurHash2 of the lowercased key, seed 0x31415926 (Source 2 MakeStringToken).
constexpr uint32_t KeyHash(const char* s) {
    uint32_t len = 0;
    while (s[len]) ++len;
    auto lc = [s](uint32_t i) -> uint32_t { return static_cast<uint8_t>(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]); };
    constexpr uint32_t m = 0x5BD1E995;
    uint32_t h = 0x31415926u ^ len, i = 0;
    for (; len - i >= 4; i += 4) {
        uint32_t k = lc(i) | lc(i + 1) << 8 | lc(i + 2) << 16 | lc(i + 3) << 24;
        k *= m;
        k ^= k >> 24;
        k *= m;
        h = h * m ^ k;
    }
    switch (len - i) {
        case 3: h ^= lc(i + 2) << 16; [[fallthrough]];
        case 2: h ^= lc(i + 1) << 8;  [[fallthrough]];
        case 1: h ^= lc(i); h *= m;
    }
    h ^= h >> 13;
    h *= m;
    return h ^ h >> 15;
}
static_assert(KeyHash("classname") == 0xC61B1C62 && KeyHash("origin") == 0xE4200216 && KeyHash("angles") == 0xBA98DACF &&
              KeyHash("model") == 0xC8BFBC06 && KeyHash("solid") == 0xCC9643E1 &&
              KeyHash("DefaultAnim") == 0x87219ED5 && KeyHash("StartingAnim") == 0xD1DB7AA5 &&
              KeyHash("!bonemerge") == 0x1C9748E6,
              "KeyHash no longer matches the game's key hashes");

struct KV3MemberName { uint32_t hash; int32_t unk; const char* name; };
constexpr KV3MemberName Key(const char* s) { return {KeyHash(s), -1, s}; }
constexpr KV3MemberName kKeyClassname = Key("classname"), kKeyModel = Key("model"), kKeyOrigin = Key("origin"),
                        kKeyAngles = Key("angles"), kKeySolid = Key("solid");

struct EntitySpawnInfo { uintptr_t identity; uintptr_t kv; uint64_t flags; };
alignas(16) constexpr float kIdentity3x4[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
constexpr const char* kClass = "prop_dynamic_override";  // non-solid with solid "0"; override survives a bad prop_data

using KvCtorFn       = void*(__fastcall*)(uintptr_t kv, void* ctx, uint8_t allocType);
using KvDtorFn       = void(__fastcall*)(uintptr_t kv);
using KvFindFn       = void*(__fastcall*)(uintptr_t kv, const KV3MemberName* name);
using KvSetStringFn  = void(__fastcall*)(uintptr_t kv, void* value, const char* str);
using CreateByNameFn = uintptr_t(__fastcall*)(void* es, uint32_t group, const char* name, int mode, int index, int serial, bool);
using QueueSpawnFn   = void(__fastcall*)(void* es, uintptr_t identity, uintptr_t kv);
using ExecQueuedFn   = void(__fastcall*)(void* es);
using UtilRemoveFn   = void(__fastcall*)(uintptr_t ent);
using SetVecFn       = void(__fastcall*)(uintptr_t node, const float* v);  // SetLocalOrigin / SetLocalAngles
using SpawnGroupFn   = uint32_t(__fastcall*)(void* es);
using PrecacheFn     = void(__fastcall*)(void* grs, uint32_t group, int count, const EntitySpawnInfo*, const float* matrix);
using MemAllocFn     = void*(__fastcall*)(void* self, size_t size);
using MemFreeFn      = void(__fastcall*)(void* self, void* p);

KvCtorFn g_kvCtor; KvDtorFn g_kvDtor; KvFindFn g_kvFind; KvSetStringFn g_kvSetString;
CreateByNameFn g_create; QueueSpawnFn g_queueSpawn; ExecQueuedFn g_execQueued; UtilRemoveFn g_remove;
SetVecFn g_setOrigin, g_setAngles; SpawnGroupFn g_spawnGroup;
void* g_grs = nullptr;       // sdk::GameResourceService() (vtable[35] = PrecacheEntitiesAndConfirmResourcesAreLoaded)
void* g_memAlloc = nullptr;  // tier0 g_pMemAlloc (vtable[1] Alloc, [3] Free)
int32_t g_nodeOff = -1, g_ownerOff = -1;
int g_state = 0;  // 0 unresolved, 1 ready, -1 unavailable (not found)

bool Resolve() {
    if (g_state) return g_state > 0;
    g_state = -1;
    auto scan = [](const char* p) { return sdk::PatternScan("client.dll", p); };
    g_nodeOff  = sdk::GetSchemaOffset("client.dll", "C_BaseEntity", "m_pGameSceneNode");
    g_ownerOff = sdk::GetSchemaOffset("client.dll", "CGameSceneNode", "m_pOwner");
    const int32_t angOff = sdk::GetSchemaOffset("client.dll", "CGameSceneNode", "m_angRotation");
    const uintptr_t kvCtor = scan(kKvCtorPattern), kvDtor = scan(kKvDtorPattern), kvFind = scan(kKvFindPattern),
                    kvSet = scan(kKvSetStringPattern), create = scan(kCreateByNamePattern), queue = scan(kQueueSpawnPattern),
                    exec = scan(kExecQueuedPattern), remove = scan(kUtilRemovePattern), origin = scan(kSetOriginPattern),
                    angles = scan(SETLOCALANGLES_PATTERN), groupCall = scan(kSpawnGroupCallPattern);
    const uintptr_t group = groupCall ? sdk::ResolveRelative(groupCall + 12, 1, 5) : 0;
    g_grs = sdk::GameResourceService();  // vt[27]/[29]/[31] already checked there
    const uintptr_t precache = g_grs ? sdk::PatternScan("engine2.dll", kPrecachePattern) : 0;
    if (!precache || sdk::Read<uintptr_t>(sdk::Read<uintptr_t>(reinterpret_cast<uintptr_t>(g_grs)) + 35 * 8) != precache)
        g_grs = nullptr;  // vtable layout changed: don't call an unknown method on the spawn group
    if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
        if (auto slot = reinterpret_cast<uintptr_t>(GetProcAddress(tier0, "g_pMemAlloc")))
            g_memAlloc = sdk::Read<void*>(slot);
    // The getter must still be `mov eax,[rcx+disp32]; ret`; SetLocalAngles embeds the m_angRotation offset it writes.
    // OnRemoveEntity is what clears a slot before the game frees a prop; without it slots could dangle.
    if (g_nodeOff < 0 || g_ownerOff < 0 || !kvCtor || !kvDtor || !kvFind || !kvSet || !create || !queue || !exec ||
        !remove || !origin || !angles || !group || !g_grs || !g_memAlloc || !sdk::Data().fnOnRemoveEntity ||
        sdk::Read<uint16_t>(group) != 0x818B || sdk::Read<uint8_t>(group + 6) != 0xC3 ||
        sdk::Read<int32_t>(angles + 27) != angOff) {
        journal().warn("props", "dances unavailable: a spawn function, GameResourceService or g_pMemAlloc was not found");
        return false;
    }
    g_kvCtor = reinterpret_cast<KvCtorFn>(kvCtor);
    g_kvDtor = reinterpret_cast<KvDtorFn>(kvDtor);
    g_kvFind = reinterpret_cast<KvFindFn>(kvFind);
    g_kvSetString = reinterpret_cast<KvSetStringFn>(kvSet);
    g_create = reinterpret_cast<CreateByNameFn>(create);
    g_queueSpawn = reinterpret_cast<QueueSpawnFn>(queue);
    g_execQueued = reinterpret_cast<ExecQueuedFn>(exec);
    g_remove = reinterpret_cast<UtilRemoveFn>(remove);
    g_setOrigin = reinterpret_cast<SetVecFn>(origin);
    g_setAngles = reinterpret_cast<SetVecFn>(angles);
    g_spawnGroup = reinterpret_cast<SpawnGroupFn>(group);
    g_state = 1;
    return true;
}

void SetKey(uintptr_t kv, const KV3MemberName& key, const char* value) {
    if (void* v = g_kvFind(kv, &key)) g_kvSetString(kv, v, value);
}

// ent = the new client-only entity (index >= 0x4000), or 0. false = the game faulted; ent then holds whatever
// CreateEntityByName already built, so the caller can remove it instead of orphaning it.
// extra: additional string keys, e.g. {"DefaultAnim", "..."}.
bool SpawnProp(const char* model, const char* origin, const char* angles, const char* const (*extra)[2], int extraN,
               uintptr_t& ent) {
    ent = 0;
    __try {
        void* es = sdk::Data().entitySystem;
        void** allocVt = *reinterpret_cast<void***>(g_memAlloc);
        const uintptr_t kv = reinterpret_cast<uintptr_t>(reinterpret_cast<MemAllocFn>(allocVt[1])(g_memAlloc, 56));
        if (!es || !kv) return true;
        g_kvCtor(kv, nullptr, 0);  // null context: the block allocates (and frees) its own
        ++*reinterpret_cast<int16_t*>(kv + 0x20);
        // Every key before QueueSpawnEntity: it locks the block (+0x22) and FindOrCreate returns null afterwards.
        SetKey(kv, kKeyClassname, kClass);
        SetKey(kv, kKeyModel, model);
        SetKey(kv, kKeyOrigin, origin);
        SetKey(kv, kKeyAngles, angles);
        SetKey(kv, kKeySolid, "0");  // SOLID_NONE -> C_DynamicProp m_bCreateNonSolid: no contact, trace or physics body
        for (int k = 0; k < extraN; ++k) SetKey(kv, {KeyHash(extra[k][0]), -1, extra[k][0]}, extra[k][1]);

        const uint32_t group = g_spawnGroup(es);
        ent = g_create(es, group, kClass, 0 /*client-only*/, -1, -1, false);
        const uintptr_t identity = ent ? *reinterpret_cast<uintptr_t*>(ent + 0x10) : 0;
        if (identity) {
            const EntitySpawnInfo info{identity, kv, 0x1FFFFFFFFFFFFull};
            using PrecacheVt = PrecacheFn*;
            (*reinterpret_cast<PrecacheVt*>(g_grs))[35](g_grs, group, 1, &info, kIdentity3x4);
            if (*reinterpret_cast<uint32_t*>(identity + 0x30) & 0x200) {
                ent = 0;  // died during precache; the game skips the queue too
            } else {
                g_queueSpawn(es, identity, kv);
                g_execQueued(es);
            }
        } else {
            ent = 0;
        }
        if (--*reinterpret_cast<int16_t*>(kv + 0x20) <= 0) {  // drop our ref (QueueSpawnEntity holds its own)
            g_kvDtor(kv);
            reinterpret_cast<MemFreeFn>(allocVt[3])(g_memAlloc, reinterpret_cast<void*>(kv));
        }
        return true;
    } __except(1) {
        return false;  // ponytail: a fault mid-spawn leaks the 56-byte KV block
    }
}

bool RemoveOne(uintptr_t ent) {
    __try {
        g_remove(ent);
        return true;
    } __except(1) {
        return false;
    }
}

bool MoveOne(uintptr_t ent, const float* pos, const float* ang) {
    __try {
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(ent + g_nodeOff);
        if (node && *reinterpret_cast<uintptr_t*>(node + g_ownerOff) == ent) {
            g_setOrigin(node, pos);
            g_setAngles(node, ang);
        }
        return true;
    } __except(1) {
        return false;
    }
}

}  // namespace

bool ClientPropReady() { return Resolve(); }

bool ClientPropSpawn(const char* model, const float* origin, const float* angles, const char* const (*keys)[2],
                     int keyCount, uintptr_t& ent) {
    ent = 0;
    if (!Resolve() || !model) return false;
    char o[64], a[48];
    std::snprintf(o, sizeof(o), "%.2f %.2f %.2f", origin[0], origin[1], origin[2]);
    std::snprintf(a, sizeof(a), "%.2f %.2f %.2f", angles[0], angles[1], angles[2]);
    return SpawnProp(model, o, a, keys, keyCount, ent);
}

bool ClientPropRemove(uintptr_t ent) { return !ent || (Resolve() && RemoveOne(ent)); }

bool ClientPropMove(uintptr_t ent, const float* pos, const float* ang) { return !ent || (Resolve() && MoveOne(ent, pos, ang)); }

}  // namespace emotes
