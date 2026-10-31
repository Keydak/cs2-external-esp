#include "features/FortniteDances.h"
#include "features/ClientProp.h"
#include "features/Music.h"
#include "hooks/GameHooks.h"
#include "core/Journal.h"
#include "sdk/GameFiles.h"
#include "sdk/GameState.h"
#include "sdk/PatternScan.h"
#include "sdk/ResourceManifest.h"
#include "sdk/SchemaSystem.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace emotes {

FortniteDancesConfig& GetFortniteDancesConfig() {
    static FortniteDancesConfig cfg;
    return cfg;
}

namespace {

#include "features/FortniteDanceTable.inc"
#include "features/FortniteDanceRarity.inc"
static_assert(std::size(kDances) == kFortniteDanceCount, "FortniteDanceTable.inc changed: update kFortniteDanceCount and "
              "the emote slot ranges in emotes.ini");
static_assert(std::size(kDanceRarity) == std::size(kDances), "regenerate FortniteDanceRarity.inc ");
constexpr int kCount = static_cast<int>(std::size(kDances));

// client.dll, each unique on disk (PLAN.md section 1). FollowEntity embeds the "!bonemerge" token below.
constexpr const char* kFollowPattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 48 8B F2 48 8B F9 45 84 C0 74 ? 48 8B 41 10 4C 8B 00 4D 85 C0 74 ? "
    "48 8B 40 08 48 63 15 ? ? ? ? 48 8B 88 ? ? ? ? 0F B7 04 51 B9 FF FF 00 00 66 3B C1 74 ? 4A 8B 0C 00 EB ? 48 8B CB 48 8B "
    "49 08 48 8B 01 FF 50 ? 48 85 C0 74 ? 48 39 98 ? ? ? ? 74 ? BB E6 48 97 1C";
constexpr const char* kSetAnimPattern = "48 85 D2 0F 84 ? ? ? ? 55 56 41 56 48 83 EC 50 C6 81 ? ? 00 00 00 45 8B F0 80 3A 00";
constexpr uint32_t kBonemergeToken = 0x1C9748E6;  // KeyHash("!bonemerge"), asserted in ClientProp.cpp
constexpr const char* kModelFile = "characters/kolka/fortnite_dance.vmdl_c";

using FollowFn = void(__fastcall*)(void* child, void* parent, bool boneMerge);
using SetAnimFn = void(__fastcall*)(void* prop, const char* seq, int loopMode);  // 0 once (holds), 1 looping

struct Dancer {                       // 64 max
    std::atomic<uintptr_t> pawn{0}, prop{0}, clone{0};   // clone 0 = fallback (prop visible)
    int dance = -1; bool intro = false; double start = 0; float yaw = 0; bool local = false;
    bool fixedYaw = false;            // faces d.yaw (the emote) instead of turning with the pawn
    const char* model = nullptr;      // pawn m_ModelName pointer at spawn (symbol pointers are stable)
    double nextMusic = 0;
};
Dancer g_d[64];
std::atomic<bool> g_any{false};
struct Want { uintptr_t pawn; int dance; bool fixedYaw; } g_want[65];
int g_wantN = 0;  // game thread only
std::atomic<uintptr_t> g_orphans[64];  // clones whose dance prop the game destroyed; removed next frame

FollowFn g_follow = nullptr;  // null = fallback only (stock dance model)
SetAnimFn g_setAnim = nullptr;
int32_t g_nodeOff = -1, g_ownerHOff = -1, g_nodeOwnerOff = -1, g_parentOff = -1, g_absOriginOff = -1, g_absRotOff = -1,
        g_attachOff = -1, g_modelStateOff = -1, g_modelNameOff = -1;
int g_state = 0;  // 0 unresolved, 1 ready, -1 unavailable (not found, or too many faults)
int g_faults = 0;
std::atomic<int> g_status{static_cast<int>(DanceStatus::Off)};
std::atomic<bool> g_releasing{false}, g_inFrame{false};

std::filesystem::path g_game;  // <game>/csgo
std::mutex g_dirMu;
std::filesystem::path g_dir;   // <data>/emotes
std::atomic<bool> g_filesFound{false};
bool g_mounted = false;
double g_nextFileCheck = 0;

std::atomic<void*> g_manifest{nullptr};
bool g_modelReady = false;
int g_levelFails = 0;
double g_nextAttempt = 0, g_loadStart = 0;
struct Cool { uintptr_t pawn; double until; } g_cool[64];  // per-pawn spawn cooldown after a failure
int g_coolNext = 0;
bool g_music = false;  // we started music that may still play
bool g_warnedMerge = false;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
DanceStatus Status() { return static_cast<DanceStatus>(g_status.load()); }
void SetStatus(DanceStatus s) {
    if (g_state >= 0) g_status = static_cast<int>(s);  // Unavailable is final for the session
}

std::filesystem::path Dir() {
    std::lock_guard lk(g_dirMu);
    return g_dir;
}

// ---- SEH-guarded game calls (no C++ objects with destructors in these) ----

// 1 ok, 0 no scene node or non-finite pose, -1 fault
int ReadPawn(uintptr_t pawn, float* pos, float& yaw, const char*& model) {
    __try {
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(pawn + g_nodeOff);
        if (!node) return 0;
        for (int k = 0; k < 3; ++k) pos[k] = *reinterpret_cast<float*>(node + g_absOriginOff + 4 * k);
        yaw = *reinterpret_cast<float*>(node + g_absRotOff + 4);
        model = *reinterpret_cast<const char**>(node + g_modelStateOff + g_modelNameOff);
        return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) && std::isfinite(yaw) ? 1 : 0;
    } __except(1) {
        return -1;
    }
}

bool CopyName(const char* src, char* out, size_t n) {
    __try {
        size_t i = 0;
        for (; src && i + 1 < n && src[i]; ++i) out[i] = src[i];
        out[i] = 0;
        return true;
    } __except(1) {
        out[0] = 0;
        return false;
    }
}

// merged = the child got the "!bonemerge" attachment (it only does when it already has a model)
bool Follow(uintptr_t child, uintptr_t parent, bool& merged) {
    merged = false;
    __try {
        g_follow(reinterpret_cast<void*>(child), reinterpret_cast<void*>(parent), true);
        const uintptr_t node = *reinterpret_cast<uintptr_t*>(child + g_nodeOff);
        merged = node && *reinterpret_cast<uint32_t*>(node + g_attachOff) == kBonemergeToken;
        return true;
    } __except(1) {
        return false;
    }
}

bool SetAnim(uintptr_t prop, const char* seq, int loopMode) {
    __try {
        g_setAnim(reinterpret_cast<void*>(prop), seq, loopMode);
        return true;
    } __except(1) {
        return false;
    }
}

// ---- lock-free helpers for render threads  ----

uintptr_t EntFromHandle(uint32_t h) {
    if (h == 0 || h == 0xFFFFFFFF) return 0;
    const uintptr_t ent = reinterpret_cast<uintptr_t>(sdk::GetEntityByIndex(h & 0x7FFF));
    const uintptr_t identity = ent ? sdk::Read<uintptr_t>(ent + 0x10) : 0;
    return identity && sdk::Read<uint32_t>(identity + 0x10) == h ? ent : 0;
}

// m_hOwnerEntity, else node->m_pParent->m_pOwner
uintptr_t NextLink(uintptr_t ent) {
    if (const uintptr_t owner = EntFromHandle(sdk::Read<uint32_t>(ent + g_ownerHOff))) return owner;
    const uintptr_t node = sdk::Read<uintptr_t>(ent + g_nodeOff);
    const uintptr_t parent = node ? sdk::Read<uintptr_t>(node + g_parentOff) : 0;
    return parent ? sdk::Read<uintptr_t>(parent + g_nodeOwnerOff) : 0;
}

// ---- game thread ----

bool Resolve() {
    if (g_state) return g_state > 0;
    g_state = -1;
    auto off = [](const char* cls, const char* field) { return sdk::GetSchemaOffset("client.dll", cls, field); };
    g_nodeOff = off("C_BaseEntity", "m_pGameSceneNode");
    g_ownerHOff = off("C_BaseEntity", "m_hOwnerEntity");
    g_nodeOwnerOff = off("CGameSceneNode", "m_pOwner");
    g_parentOff = off("CGameSceneNode", "m_pParent");
    g_absOriginOff = off("CGameSceneNode", "m_vecAbsOrigin");
    g_absRotOff = off("CGameSceneNode", "m_angAbsRotation");
    g_attachOff = off("CGameSceneNode", "m_hierarchyAttachName");
    g_modelStateOff = off("CSkeletonInstance", "m_modelState");
    g_modelNameOff = off("CModelState", "m_ModelName");
    const uintptr_t follow = sdk::PatternScan("client.dll", kFollowPattern);
    const uintptr_t anim = sdk::PatternScan("client.dll", kSetAnimPattern);
    const int32_t offs[] = {g_nodeOff, g_ownerHOff, g_nodeOwnerOff, g_parentOff, g_absOriginOff,
                            g_absRotOff, g_attachOff, g_modelStateOff, g_modelNameOff};
    if (!ClientPropReady() || !anim || std::any_of(std::begin(offs), std::end(offs), [](int32_t o) { return o < 0; })) {
        g_status = static_cast<int>(DanceStatus::Unavailable);
        journal().warn("dances", "Fortnite dances unavailable: a game function was not found");
        return false;
    }
    g_setAnim = reinterpret_cast<SetAnimFn>(anim);
    g_follow = reinterpret_cast<FollowFn>(follow);
    if (!follow) journal().warn("dances", "dance copy unavailable (FollowEntity not found); showing the stock model");
    if (!sdk::CanMountGamePaths()) journal().warn("dances", "can't add the emotes folder to the game; only a copy in game\\csgo will load");
    wchar_t buf[1024];
    if (HMODULE c = GetModuleHandleW(L"client.dll"); c && GetModuleFileNameW(c, buf, static_cast<DWORD>(std::size(buf))))
        g_game = std::filesystem::path(buf).parent_path().parent_path().parent_path();  // bin\win64\client.dll -> csgo
    g_state = 1;
    return true;
}

void StopMusic() {
    if (!g_music) return;
    g_music = false;
    MusicStop();
}

// Local dancer only: (re)starts the dance's music if it is on and the file is there.
void StartMusic(Dancer& d, double now) {
    auto& cfg = GetFortniteDancesConfig();
    const DanceDef& def = kDances[d.dance];
    d.nextMusic = INFINITY;
    std::error_code ec;
    const std::filesystem::path wav = def.music ? Dir() / "music" / def.music : std::filesystem::path();
    if (!cfg.music.load() || wav.empty() || !std::filesystem::exists(wav, ec)) {
        if (!cfg.music.load()) journal().info("music", "music is off");
        if (cfg.music.load() && !wav.empty()) {
            journal().warn("music", "song not found: " + std::string(reinterpret_cast<const char*>(wav.u8string().c_str())));
        }
        StopMusic();
        return;
    }
    journal().info("music", "starting " + std::string(reinterpret_cast<const char*>(wav.u8string().c_str())));
    MusicPlay(wav, &cfg.volume);
    g_music = true;
    if (def.loopEvery > 0) d.nextMusic = now + def.loopEvery;
}

// Unpublishes the pawn first (it draws again), then removes the copy and the dance prop. false = a game fault.
bool RemoveSlot(Dancer& d) {
    if (d.local) StopMusic();
    d.pawn = 0;
    const uintptr_t clone = d.clone.exchange(0), prop = d.prop.exchange(0);
    d.dance = -1;
    d.local = false;
    const bool ok = ClientPropRemove(clone);
    return ClientPropRemove(prop) && ok;
}

void RemoveAll() {  // never counts faults: also the "remove what we can" of Fault()
    g_any = false;
    for (auto& d : g_d)
        if (d.prop.load()) RemoveSlot(d);
    for (auto& o : g_orphans) ClientPropRemove(o.exchange(0));
}

void DropManifest() {
    sdk::ManifestDestroy(g_manifest.exchange(nullptr));
    g_modelReady = false;
}

// true = the feature just got turned off for the session
bool Fault() {
    if (++g_faults < 5) return false;
    RemoveAll();
    StopMusic();
    g_state = -1;
    g_status = static_cast<int>(DanceStatus::Unavailable);
    journal().warn("dances", "Fortnite dances turned off after repeated errors");
    return true;
}

bool Cooling(uintptr_t pawn, double now) {
    for (const auto& c : g_cool)
        if (c.pawn == pawn && now < c.until) return true;
    return false;
}
void CoolDown(uintptr_t pawn, double now) { g_cool[g_coolNext++ % 64] = {pawn, now + 5.0}; }

// Finds the model (Documents copy mounted on GAME, or the loose game\csgo copy). Pure file checks, every 2 s.
bool EnsureFiles(double now) {
    if (g_filesFound.load()) return true;
    if (now < g_nextFileCheck) return false;
    g_nextFileCheck = now + 2.0;
    const std::filesystem::path dir = Dir(), addonDir = dir / "csgo_addons";
    std::error_code ec;
    const bool addon = !dir.empty() && std::filesystem::exists(addonDir / kModelFile, ec);
    const bool loose = !g_game.empty() && std::filesystem::exists(g_game / kModelFile, ec);
    if (!addon && !loose) return false;
    if (addon && !g_mounted && sdk::CanMountGamePaths()) {  // once per session, always before the first ManifestCreate
        g_mounted = true;
        const std::u8string utf8 = addonDir.lexically_normal().make_preferred().u8string();
        if (!sdk::MountGamePath(reinterpret_cast<const char*>(utf8.c_str())))
            journal().warn("dances", "adding the emotes folder to the game failed");
    }
    g_filesFound = true;
    return true;
}

// Async load, 15 s timeout, 5 s cooldown, 3 fails per level.
bool ModelReady(double now) {
    if (g_modelReady && g_manifest.load()) return true;
    g_modelReady = false;
    if (g_levelFails >= 3) {
        SetStatus(DanceStatus::LoadFailed);
        return false;
    }
    SetStatus(DanceStatus::Loading);
    if (!g_manifest.load()) {
        if (now < g_nextAttempt) return false;
        const char* names[1] = {kDanceModel};
        g_manifest = sdk::ManifestCreate(names, 1);
        g_loadStart = now;
    }
    const bool ready = g_manifest.load() && sdk::ManifestLoaded(g_manifest.load());
    if (!g_manifest.load() || (!ready && now - g_loadStart > 15.0)) {
        DropManifest();
        g_nextAttempt = now + 5.0;
        if (++g_levelFails >= 3) {
            SetStatus(DanceStatus::LoadFailed);
            journal().warn("dances", "the game did not load the Fortnite dance model; giving up until the next map");
        }
        return false;
    }
    if (ready) SetStatus(DanceStatus::Ready);
    return g_modelReady = ready;
}

// The agent copy bonemerged onto prop, or 0 (fallback: the dance prop stays visible). fault = the game faulted.
uintptr_t MakeClone(uintptr_t prop, const char* model, const float* pos, const float* ang, bool& fault) {
    fault = false;
    char name[261];
    if (!g_follow || !CopyName(model, name, sizeof(name)) || !name[0]) return 0;
    uintptr_t clone = 0;
    bool merged = false;
    if (!ClientPropSpawn(name, pos, ang, nullptr, 0, clone) || (clone && !Follow(clone, prop, merged))) fault = true;
    if (clone && !merged) {
        ClientPropRemove(clone);
        clone = 0;
        if (!fault && !g_warnedMerge) {
            g_warnedMerge = true;
            journal().warn("dances", "dance copy could not follow the dance; showing the stock model");
        }
    }
    return clone;
}

// false = the game faulted
bool Spawn(Dancer& d, uintptr_t pawn, int dance, bool fixedYaw, bool local, double now) {
    float pos[3], yaw;
    const char* model = nullptr;
    const int r = ReadPawn(pawn, pos, yaw, model);
    if (r <= 0) {
        CoolDown(pawn, now);
        return r == 0;
    }
    const DanceDef& def = kDances[dance];
    const char* const key[1][2] = {{def.afterSeq ? "StartingAnim" : "DefaultAnim", def.seq}};
    const float ang[3] = {0.0f, yaw, 0.0f};
    uintptr_t prop = 0;
    if (!ClientPropSpawn(kDanceModel, pos, ang, key, 1, prop)) {
        ClientPropRemove(prop);  // half-spawned: drop it rather than orphan it
        CoolDown(pawn, now);
        return false;
    }
    if (!prop) {
        CoolDown(pawn, now);
        return true;
    }
    bool fault;
    const uintptr_t clone = MakeClone(prop, model, pos, ang, fault);
    d.dance = dance;
    d.intro = def.afterSeq != nullptr;
    d.start = now;
    d.yaw = yaw;
    d.local = local;
    d.fixedYaw = fixedYaw;
    d.model = model;
    d.nextMusic = INFINITY;
    // prop, clone, then pawn: the hide test never hides a pawn before its stand-in exists
    d.prop.store(prop, std::memory_order_release);
    d.clone.store(clone, std::memory_order_release);
    d.pawn.store(pawn, std::memory_order_release);
    g_any.store(true, std::memory_order_release);
    journal().info("dances", std::string("dance started: ") + def.name + (local ? " (you)" : " (not you: no music)"));
    if (local) StartMusic(d, now);
    return !fault;
}

// false = the game faulted
bool Step(Dancer& d, int dance, bool fixedYaw, double now) {
    float pos[3], yaw;
    const char* model = nullptr;
    const uintptr_t pawn = d.pawn.load();
    const int r = ReadPawn(pawn, pos, yaw, model);
    if (r < 0) return false;
    if (r == 0) {  // no scene node / bad pose: stop dancing, retry in 5 s
        CoolDown(pawn, now);
        return RemoveSlot(d);
    }
    if (fixedYaw && !d.fixedYaw) d.yaw = yaw;  // an emote took over a turning dance: hold the current yaw
    d.fixedYaw = fixedYaw;
    const float ang[3] = {0.0f, fixedYaw ? d.yaw : yaw, 0.0f};
    const uintptr_t prop = d.prop.load();
    bool ok = true;
    if (model != d.model) {  // agent changed (halftime swap): new copy first, then drop the old one
        d.model = model;
        bool fault;
        const uintptr_t old = d.clone.exchange(MakeClone(prop, model, pos, ang, fault));
        ok = ClientPropRemove(old) && !fault;
    }
    if (dance != d.dance) {
        const DanceDef& def = kDances[dance];
        ok = SetAnim(prop, def.seq, def.afterSeq ? 0 : 1) && ok;
        d.dance = dance;
        d.start = now;
        d.intro = def.afterSeq != nullptr;
        if (d.local) StartMusic(d, now);
    }
    ok = ClientPropMove(prop, pos, ang) && ok;  // stage 12 is after interpolation; the copy follows via bonemerge
    const DanceDef& def = kDances[d.dance];
    if (d.intro && now - d.start >= (def.switchAt > 0 ? def.switchAt : def.seqSeconds)) {
        d.intro = false;
        ok = SetAnim(prop, def.afterSeq, 1) && ok;
    }
    if (d.local) {
        if (g_music && !GetFortniteDancesConfig().music.load()) {
            StopMusic();
            d.nextMusic = INFINITY;
        } else if (now >= d.nextMusic) {
            StartMusic(d, now);
        }
    }
    return ok;
}

int FindWant(uintptr_t pawn) {
    for (int w = 0; w < g_wantN; ++w)
        if (g_want[w].pawn == pawn) return w;
    return -1;
}

bool HasSlot(uintptr_t pawn) {
    for (auto& d : g_d)
        if (d.pawn.load() == pawn) return true;
    return false;
}

Dancer* FreeSlot() {
    for (auto& d : g_d)
        if (!d.pawn.load() && !d.prop.load()) return &d;
    return nullptr;
}

bool AnyProp() {
    for (auto& d : g_d)
        if (d.prop.load()) return true;
    for (auto& o : g_orphans)
        if (o.load()) return true;
    return false;
}

}  // namespace

DanceStatus FortniteDancesStatus() { return Status(); }
int FortniteDanceCount() { return kCount; }
const char* FortniteDanceName(int i) { return i >= 0 && i < kCount ? kDances[i].name : ""; }
bool FortniteDanceIsEmote(int i) { return i >= 0 && i < kCount && kDances[i].isEmote; }
const char* FortniteDanceSeq(int i) { return i >= 0 && i < kCount ? kDances[i].seq : ""; }
int FortniteDanceRarity(int i) { return i >= 0 && i < kCount ? kDanceRarity[i] : 0; }
std::filesystem::path FortniteDancesDir() { return Dir(); }
float FortniteDanceLength(int i) {
    if (i < 0 || i >= kCount) return 0.0f;
    return kDances[i].endAfter > 0 ? kDances[i].endAfter : INFINITY;
}

void FortniteDancesSetDir(const std::filesystem::path& dir) {
    std::lock_guard lk(g_dirMu);
    if (dir == g_dir) return;
    g_dir = dir;
    g_filesFound = false;  // look again (the mount stays once per session)
}

void FortniteDancesWant(void* pawn, int dance, bool fixedYaw) {
    if (!pawn) return;
    dance = std::clamp(dance, 0, kCount - 1);
    const auto p = reinterpret_cast<uintptr_t>(pawn);
    if (const int w = FindWant(p); w >= 0) g_want[w] = {p, dance, fixedYaw};  // later request wins
    else if (g_wantN < static_cast<int>(std::size(g_want))) g_want[g_wantN++] = {p, dance, fixedYaw};
}

// ponytail: no lock vs OnRemoveEntity; both run on the game thread
void FortniteDancesFrame() {
    g_inFrame = true;
    struct Done { ~Done() { g_wantN = 0; g_inFrame = false; } } done;  // want list is cleared on every return
    auto& cfg = GetFortniteDancesConfig();
    const double now = Now();
    if (g_releasing.load() || !cfg.enabled.load()) {
        RemoveAll();
        DropManifest();
        StopMusic();
        SetStatus(DanceStatus::Off);
        return;
    }
    if (!Resolve()) return;
    if (hooks::DrawHookFailed()) {  // stage 5 tried it this frame; without it the real player draws under the copy
        g_state = -1;
        g_status = static_cast<int>(DanceStatus::Unavailable);
        journal().warn("dances", "Fortnite dances unavailable: the draw hook could not be installed");
        return;
    }
    if (!EnsureFiles(now)) {  // before the in-game check: the status is right in the main menu too
        SetStatus(DanceStatus::NoFiles);
        return;
    }
    if (!sdk::IsInGame()) {  // LevelShutdown already forgot the props
        if (Status() == DanceStatus::NoFiles) SetStatus(DanceStatus::Off);  // nothing loads out of a match
        return;
    }
    for (auto& o : g_orphans)
        if (const uintptr_t e = o.exchange(0); e && !ClientPropRemove(e) && Fault()) return;
    if (!ModelReady(now)) return;

    for (auto& d : g_d) {
        const uintptr_t pawn = d.pawn.load();
        if (!pawn) continue;
        const int w = FindWant(pawn);  // an unwanted slot pawn is only compared, never read
        if (w < 0) {
            if (!RemoveSlot(d) && Fault()) return;
        } else if (!Step(d, g_want[w].dance, g_want[w].fixedYaw, now)) {
            RemoveSlot(d);
            CoolDown(pawn, now);
            if (Fault()) return;
        }
    }
    const uintptr_t localPawn = reinterpret_cast<uintptr_t>(sdk::GetLocalPawn());
    for (int w = 0, spawned = 0; w < g_wantN && spawned < 2 && !g_releasing.load(); ++w) {
        const uintptr_t pawn = g_want[w].pawn;
        if (HasSlot(pawn) || Cooling(pawn, now)) continue;
        Dancer* d = FreeSlot();
        if (!d) break;
        ++spawned;
        if (!Spawn(*d, pawn, g_want[w].dance, g_want[w].fixedYaw, pawn == localPawn, now) && Fault()) return;
    }
    bool any = false;
    for (auto& d : g_d) any |= d.pawn.load() != 0;
    g_any.store(any && !g_releasing.load(), std::memory_order_release);
}

void FortniteDancesOnEntityRemoved(void* ent) {
    const auto e = reinterpret_cast<uintptr_t>(ent);
    if (!e) return;
    for (auto& o : g_orphans) {
        uintptr_t x = e;
        if (o.compare_exchange_strong(x, 0)) return;
    }
    for (auto& d : g_d) {
        uintptr_t x = e;
        if (d.clone.compare_exchange_strong(x, 0)) return;  // the slot falls back to the visible dance prop
        if (d.prop.load() != e) continue;
        if (d.local) StopMusic();
        d.pawn = 0;  // draw the real player again
        const uintptr_t clone = d.clone.exchange(0);
        d.prop = 0;
        d.dance = -1;
        d.local = false;
        // Usually the game destroys the copy with it (and OnRemoveEntity clears the orphan); else next frame removes it.
        // ponytail: 64 orphan slots; a full list leaks the copy until the map ends
        for (auto& o : g_orphans) {
            uintptr_t zero = 0;
            if (!clone || o.compare_exchange_strong(zero, clone)) break;
        }
        return;
    }
}

void FortniteDancesLevelShutdown() {
    g_any = false;
    for (auto& d : g_d) {
        d.pawn = 0;
        d.clone = 0;
        d.prop = 0;
        d.dance = -1;
        d.local = false;
    }
    for (auto& o : g_orphans) o = 0;
    DropManifest();  // before the original shutdown
    StopMusic();
    g_levelFails = 0;
    g_nextAttempt = Now() + 5.0;  // no spawn into a group being torn down
    for (auto& c : g_cool) c = {};
    if (Status() == DanceStatus::Ready || Status() == DanceStatus::LoadFailed) SetStatus(DanceStatus::Off);
}

void FortniteDancesRelease() {
    g_any = false;  // pawns draw again at once
    g_releasing = true;
    // Let a frame in flight finish, then the next stage-12 pass removes the props.
    for (int i = 0; i < 1000 && g_inFrame.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // ponytail: if no frame runs in time the manifest leaks until process exit (harmless)
    for (int i = 0; i < 20 && (g_inFrame.load() || AnyProp() || g_manifest.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    MusicStop();
}

bool FortniteDancesHides(void* so) {
    if (!g_any.load(std::memory_order_acquire) || !so) return false;
    __try {
        const uint32_t h = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(so) + 0xC0);  // the entity handle of a scene object
        if (!h || h == 0xFFFFFFFF) return false;
        uintptr_t e = reinterpret_cast<uintptr_t>(sdk::GetEntityByIndex(h & 0x7FFF));  // works for client-only ents
        for (auto& d : g_d) {
            if (!d.pawn.load(std::memory_order_acquire)) continue;
            if (e == d.clone.load()) return false;                // our copy: draw it (checked before the walk)
            if (e == d.prop.load()) return d.clone.load() != 0;   // dance prop hidden unless fallback
        }
        for (int link = 0; e && link < 5; ++link) {  // pawn, weapons, modules, gloves
            for (auto& d : g_d)
                if (e == d.pawn.load()) return true;
            e = NextLink(e);
        }
    } __except(1) {}
    return false;
}

bool FortniteDancesNeedDrawHook() { return GetFortniteDancesConfig().enabled.load(); }

}  // namespace emotes
