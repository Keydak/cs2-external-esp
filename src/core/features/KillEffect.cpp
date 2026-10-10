#include "KillEffect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/GameThread.hpp"
#include "core/features/HitEffects.hpp"
#include "core/features/MaterialChams.hpp"
#include "core/features/View.hpp"
#include "core/offsets/Dumper.hpp"

namespace {
    constexpr auto POLL = std::chrono::milliseconds(16);
    constexpr auto DEATH_MATCH = std::chrono::milliseconds(600);    // A kill & a death this close are the same
    constexpr auto RESPAWN_AFTER = std::chrono::milliseconds(400);  // Alive this long after the kill: respawned

    // The effects: particle systems of the game, never loaded by us (asking it to load them crashed it). follow: on
    // the body, the effects made for bodies use its bones (it is not drawn, they still go over its shape as it falls);
    // else in the world at the middle of the body. burst: from each part of the body, outward (the shape of the body
    // like Ashes, blown apart). copies: at once in each place, the thin ones show more. waves: played again that many
    // times, WAVE_EVERY apart. Dissolve, Ghost & the body Electric in the world showed nothing; Electric on the body
    // looked odd with the body gone, the sparks of a zeus hit in the world showed nothing either (not loaded without a
    // zeus around), the zeus effect alone in the world neither, & put on the body once it still went with the ragdoll
    // (it runs over the bones of the body). Electric now: the zeus effect on the body, which stays (keep_body) like a
    // real zeus kill, many copies & waves to make it big (the user's pick, its big bolts showed with the body seen).
    // floats: no particles, the body stays & floats (ragdoll_gravity_scale). overlay: no particles either, drawn by the
    // overlay where the body was (Overlays::RenderStrikes): KillEffect::StrikeKind
    struct Effect {
        const char* name;
        const char* file;
        bool follow;
        bool burst;
        int copies;
        int waves;
        bool floats;
        bool keep_body = false;
        int overlay = 0;    // KillEffect::StrikeKind
    };
    constexpr Effect EFFECTS[] = {
        { "Ashes",      "particles/blood_impact/impact_taser_bodyfx_ashes.vpcf",    true,   false,  4,  3,  false },
        { "Explosion",  "particles/explosions_fx/explosion_basic.vpcf",             false,  false,  2,  1,  false },
        { "Blood",      "particles/blood_impact/blood_impact_heavy.vpcf",           false,  true,   2,  1,  false },
        { "Electric",   "particles/blood_impact/impact_taser_bodyfx.vpcf",          true,   false,  6,  5,  false,  true },
        { "Zeus",       nullptr,                                                    false,  false,  0,  1,  false,  false,  1 },
        { "Singularity", nullptr,                                                   false,  false,  0,  1,  false,  false,  2 },
        { "Shatter",    nullptr,                                                    false,  false,  0,  1,  false,  false,  3 },
        { "Glitch",     nullptr,                                                    false,  false,  0,  1,  false,  false,  4 },
        { "Ascend",     nullptr,                                                    false,  false,  0,  1,  false,  false,  5 },
        { "Frost",      nullptr,                                                    false,  false,  0,  1,  false,  false,  7 },
        { "Slash",      nullptr,                                                    false,  false,  0,  1,  false,  false,  8 },
        { "Pixels",     nullptr,                                                    false,  false,  0,  1,  false,  false,  10 },
        { "Meteor",     nullptr,                                                    false,  false,  0,  1,  false,  false,  11 },
        { "Tornado",    nullptr,                                                    false,  false,  0,  1,  false,  false,  12 },
        { "TV Off",     nullptr,                                                    false,  false,  0,  1,  false,  false,  13 },
        { "Supernova",  nullptr,                                                    false,  false,  0,  1,  false,  false,  14 },
        { "No gravity", nullptr,                                                    false,  false,  0,  0,  true },
    };
    constexpr auto WAVE_EVERY = std::chrono::milliseconds(300);

    // On our own player: the effects made for bodies (they go over its shape), on it as it moves, or an aura the overlay
    // draws around it (no particles, aura not NONE)
    struct SelfEffect {
        const char* name;
        const char* file;
        int copies;
        KillEffect::SelfAura aura = KillEffect::SelfAura::NONE;
    };
    constexpr SelfEffect SELF_EFFECTS[] = {
        { "Electric",   "particles/blood_impact/impact_taser_bodyfx.vpcf",          2 },
        { "Ashes",      "particles/blood_impact/impact_taser_bodyfx_ashes.vpcf",    2 },
        { "Storm",      nullptr,    0,  KillEffect::SelfAura::STORM },
        { "Void",       nullptr,    0,  KillEffect::SelfAura::VOID_ORBS },
        { "Halo",       nullptr,    0,  KillEffect::SelfAura::HALO },
        { "Frost",      nullptr,    0,  KillEffect::SelfAura::FROST },
        { "Rings",      nullptr,    0,  KillEffect::SelfAura::RINGS },
        { "Fireflies",  nullptr,    0,  KillEffect::SelfAura::FIREFLIES },
        { "Sakura",     nullptr,    0,  KillEffect::SelfAura::SAKURA },
        { "Blades",     nullptr,    0,  KillEffect::SelfAura::BLADES },
        { "Matrix",     nullptr,    0,  KillEffect::SelfAura::MATRIX },
        { "Shield",     nullptr,    0,  KillEffect::SelfAura::SHIELD },
        { "Trail",      nullptr,    0,  KillEffect::SelfAura::TRAIL },
    };
    constexpr int SELF_COUNT = static_cast<int>(std::size(SELF_EFFECTS));
    constexpr int EFFECT_COUNT = static_cast<int>(std::size(EFFECTS));
    constexpr auto GRAVITY_SEARCH = std::chrono::seconds(5);   // ragdoll_gravity_scale not found: looked for again
    constexpr int MAX_PARTS = 12;       // Parts of the body a burst comes out of
    constexpr float PART_APART = 10.f;  // Bones closer than this are one part
    constexpr int MAX_EFFECTS = 64;     // Effects of one call, parts times copies
    constexpr float FEET_BELOW = 36.f;  // The middle of a standing body above its feet

    // ParticleAttachment_t
    constexpr int32_t ATTACH_ABSORIGIN_FOLLOW = 1;
    constexpr int32_t ATTACH_WORLDORIGIN = 8;

    // Page of one call: the code, the index the game gives, what the game is asked for, the name, where each goes
    constexpr size_t PAGE_SIZE = 0x4000;
    constexpr uintptr_t PAGE_INDEX = 0x3000;    // uint32, -1 when no effect was made
    constexpr uintptr_t PAGE_DATA = 0x3040;     // What CreateEffectIndex reads, like CreateParticle of its scripts fills it
    constexpr uintptr_t PAGE_NAME = 0x3100;
    constexpr size_t NAME_SIZE = 0x100;
    constexpr uintptr_t PAGE_SHOTS = 0x3200;    // { float position[3], pad, float forward[3], pad } each
    constexpr size_t SHOT_SIZE = 0x20;
    static_assert(PAGE_SHOTS + MAX_PARTS * SHOT_SIZE <= PAGE_SIZE);

    // The middle of the body: of its bones read last, the origin a little up without them
    Vec3_t Center(const Player& player) {
        Vec3_t sum{};
        int count = 0;
        for (const auto& bone : player.bone_list) {
            if (bone.pos.x == 0.f && bone.pos.y == 0.f && bone.pos.z == 0.f)
                continue;
            sum = sum + bone.pos;
            count++;
        }
        if (!count)
            return player.pos + Vec3_t(0.f, 0.f, 36.f);
        return Vec3_t(sum.x / count, sum.y / count, sum.z / count);
    }
}

bool KillEffect::Init() {
    std::thread(&KillEffect::Thread, &GetInstance()).detach();
    return true;
}

bool KillEffect::IsAvailable() {
    namespace pt = offsets::particles;
    return Engine::IsInsecure() && pt::fnGetManager && pt::fnCreateEffect && pt::fnSetControlPoint;
}

const std::vector<const char*>& KillEffect::GetNames() {
    static const std::vector<const char*> names = [] {
        std::vector<const char*> list;
        for (const auto& effect : EFFECTS)
            list.push_back(effect.name);
        return list;
    }();
    return names;
}

const std::vector<const char*>& KillEffect::GetSelfNames() {
    static const std::vector<const char*> names = [] {
        std::vector<const char*> list;
        for (const auto& effect : SELF_EFFECTS)
            list.push_back(effect.name);
        return list;
    }();
    return names;
}

bool KillEffect::SelfUsesInterval(int effect) {
    return SELF_EFFECTS[std::clamp(effect, 0, SELF_COUNT - 1)].file != nullptr;
}

KillEffect::SelfAura KillEffect::GetSelfAura() {
    namespace se = cfg::world::self_effect;
    if (!se::enabled || !IsAvailable())
        return SelfAura::NONE;
    if (se::third_person_only && !View::IsThirdPersonOn())
        return SelfAura::NONE;
    return SELF_EFFECTS[std::clamp(se::effect, 0, SELF_COUNT - 1)].aura;
}

void KillEffect::UpdateSelf() {
    namespace se = cfg::world::self_effect;
    if (!se::enabled || !IsAvailable())
        return;
    if (se::third_person_only && !View::IsThirdPersonOn())
        return;

    auto now = std::chrono::steady_clock::now();
    if (now < this->self_next)
        return;

    auto snapshot = Cache::Current();
    auto pawn = Engine::GetLocalPawn();
    if (!snapshot || !pawn || !snapshot->local.alive)
        return;

    auto interval = std::chrono::duration<float>(std::clamp(se::interval, 0.2f, 5.f));
    this->self_next = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);

    // Logged once, not each time it plays. An aura is the overlay's
    const auto& effect = SELF_EFFECTS[std::clamp(se::effect, 0, SELF_COUNT - 1)];
    if (!effect.file)
        return;
    this->quiet = this->self_logged;
    Play(pawn, effect.file, true, effect.copies, {});
    this->quiet = false;
    this->self_logged = true;
}

std::vector<KillEffect::Strike> KillEffect::GetStrikes() {
    auto& i = GetInstance();
    std::lock_guard lock(i.strikes_mutex);
    auto now = std::chrono::steady_clock::now();
    std::erase_if(i.strikes, [&](const Strike& strike) {
        return std::chrono::duration<float>(now - strike.at).count() > Duration(strike.kind);
    });
    return i.strikes;
}

bool KillEffect::Floats(int effect) {
    return EFFECTS[std::clamp(effect, 0, EFFECT_COUNT - 1)].floats;
}

void KillEffect::Thread() {
    while (!this->stopping) {
        std::this_thread::sleep_for(POLL);
        UpdateGravity();
        Update();
        UpdateSelf();
    }
}

void KillEffect::UpdateGravity() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    // All the time it is picked: the game reads it once, as the ragdoll is made at the death, before we see the kill.
    // The setting is the game's, for every body: the bodies of others float too
    namespace ke = cfg::world::kill_effect;
    const auto& effect = EFFECTS[std::clamp(ke::effect, 0, EFFECT_COUNT - 1)];
    bool wanted = ke::enabled && effect.floats && Engine::IsInsecure();
    auto now = std::chrono::steady_clock::now();

    if (!this->gravity_convar) {
        if (!wanted || now < this->gravity_search)
            return;
        this->gravity_search = now + GRAVITY_SEARCH;
        this->gravity_convar = View::FindConVar("ragdoll_gravity_scale");
        if (!this->gravity_convar)
            return;
        LOGF(VERBOSE, "Ragdoll gravity: ragdoll_gravity_scale at 0x{:X}, {}", this->gravity_convar,
            p->read<float>(this->gravity_convar + View::CONVAR_VALUE));
    }

    auto address = this->gravity_convar + View::CONVAR_VALUE;
    float current = p->read<float>(address);
    if (wanted) {
        // What the player had, put back when turned off
        if (!this->gravity_applied) {
            this->gravity_original = current;
            this->gravity_applied = true;
        }
        float value = std::clamp(ke::gravity, -1.f, 1.f);
        if (current != value)
            p->write<float>(address, value);
    }
    else if (this->gravity_applied) {
        p->write<float>(address, this->gravity_original);
        this->gravity_applied = false;
    }
}

void KillEffect::Update() {
    auto p = Engine::GetProcess();
    auto snapshot = Cache::Current();
    if (!p || !snapshot)
        return;

    // Our kills, taken also while it is off so none of before plays once it is turned on
    auto kills = HitEffects::DrainKills();
    bool on = cfg::world::kill_effect::enabled && IsAvailable();
    auto now = std::chrono::steady_clock::now();

    // Enemies that died just now
    for (const auto& player : snapshot->players) {
        if (player.localplayer || player.index < 0)
            continue;
        auto& alive = this->was_alive[player.index];
        if (alive && !player.alive && snapshot->game.IsEnemy(snapshot->local.team, player.team))
            this->deaths.push_back({ player.index, now });
        alive = player.alive;
        if (player.alive) {
            this->last_center[player.index] = Center(player);
            auto& bones = this->last_bones[player.index];
            bones.clear();
            for (const auto& bone : player.bone_list)
                if (!bone.pos.zero())
                    bones.push_back(bone.pos);
        }
    }
    std::erase_if(this->deaths, [&](const Death& death) { return now - death.at > DEATH_MATCH * 2; });

    if (!on) {
        if (!this->hidden_last.empty())
            MaterialChams::SetHiddenBodies({});
        this->hidden_last.clear();
        this->bodies.clear();
        return;
    }

    // New kills: their body, by the victim HitEffects told or the enemy that died with it
    for (const auto& kill : kills) {
        int victim = kill.victim;
        if (victim < 0) {
            Death* best = nullptr;
            for (auto& death : this->deaths) {
                auto apart = death.at > kill.time ? death.at - kill.time : kill.time - death.at;
                if (!death.used && apart <= DEATH_MATCH && (!best || death.at > best->at))
                    best = &death;
            }
            if (!best) {
                LOGF(VERBOSE, "Kill effect: a kill without a victim found");
                continue;
            }
            victim = best->index;
        }
        for (auto& death : this->deaths)
            if (death.index == victim)
                death.used = true;

        for (const auto& player : snapshot->players) {
            if (player.index != victim || player.localplayer)
                continue;
            auto pawn = player.GetPawnAddress();
            if (!pawn || !snapshot->game.IsEnemy(snapshot->local.team, player.team) || this->bodies.contains(pawn))
                break;

            // Where it died: its middle while it was alive, else its origin a little up
            Vec3_t center{};
            if (auto it = this->last_center.find(victim); it != this->last_center.end())
                center = it->second;
            if (center.zero())
                center = p->read<Vec3_t>(pawn + offsets::pawn::m_vOldOrigin) + Vec3_t(0.f, 0.f, 36.f);

            Body body;
            body.center = center;
            body.killed = now;
            if (auto it = this->last_bones.find(victim); it != this->last_bones.end())
                body.bones = it->second;
            this->bodies[pawn] = std::move(body);
            LOGF(VERBOSE, "Kill effect: body 0x{:X} at {:.0f} {:.0f} {:.0f}", pawn, center.x, center.y, center.z);
            break;
        }
    }

    // The bodies: the effect right away, the dead pawn (it is the body the game shows, no ragdoll entity of its own)
    // left out of the drawing while it is dead
    std::vector<uintptr_t> hidden;
    for (auto it = this->bodies.begin(); it != this->bodies.end();) {
        auto pawn = it->first;
        auto& body = it->second;

        bool dead = false;
        for (const auto& player : snapshot->players)
            if (player.GetPawnAddress() == pawn)
                dead = !player.alive;
        if (!dead && now - body.killed > RESPAWN_AFTER) {
            it = this->bodies.erase(it);
            continue;
        }

        const auto& effect = EFFECTS[std::clamp(cfg::world::kill_effect::effect, 0, EFFECT_COUNT - 1)];
        if (effect.file && body.waves < effect.waves && now >= body.next_wave) {
            body.waves++;
            body.next_wave = now + WAVE_EVERY;
            Play(pawn, effect.file, effect.follow, effect.copies, effect.burst ? Burst(body) : std::vector<Shot>{ Shot{ body.center, Vec3_t{} } });
        }
        if (effect.overlay && body.waves == 0) {
            body.waves = 1;
            this->strike_seed = this->strike_seed * 1664525u + 1013904223u;
            auto kind = static_cast<StrikeKind>(effect.overlay);
            std::lock_guard lock(this->strikes_mutex);
            this->strikes.push_back({ kind, body.center - Vec3_t(0.f, 0.f, FEET_BELOW), body.center, body.bones, now, this->strike_seed });
            LOGF(VERBOSE, "Kill effect: {} at {:.0f} {:.0f} {:.0f}", effect.name, body.center.x, body.center.y, body.center.z);
        }
        if (dead && !effect.floats && !effect.keep_body)
            hidden.push_back(pawn);
        ++it;
    }
    if (hidden != this->hidden_last) {
        MaterialChams::SetHiddenBodies(hidden);
        this->hidden_last = hidden;
    }
}

std::vector<KillEffect::Shot> KillEffect::Burst(const Body& body) {
    // The parts: bones spread over the body, the close ones as one
    std::vector<Vec3_t> parts;
    for (const auto& bone : body.bones) {
        bool merged = false;
        for (const auto& part : parts)
            if ((part - bone).length_sqr() < PART_APART * PART_APART)
                merged = true;
        if (!merged)
            parts.push_back(bone);
        if (parts.size() >= MAX_PARTS)
            break;
    }

    std::vector<Shot> shots;
    for (const auto& part : parts) {
        // Away from the middle of the body, a little up
        auto out = part - body.center + Vec3_t(0.f, 0.f, 4.f);
        float length = std::sqrt(out.length_sqr());
        Vec3_t forward = length > 0.01f ? out * (1.f / length) : Vec3_t(0.f, 0.f, 1.f);
        shots.push_back({ part, forward });
    }
    if (shots.empty())
        shots.push_back({ body.center, Vec3_t(0.f, 0.f, 1.f) });
    return shots;
}

void KillEffect::Play(uintptr_t entity, const char* file, bool follow, int copies, std::vector<Shot> shots) {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient().base;
    namespace pt = offsets::particles;

    if (!GameThread::Ensure())
        return;
    if (!this->page)
        this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return;

    // On the body: one place, the body. More copies of each, as many as fit
    if (follow || shots.empty())
        shots.resize(1);
    if (shots.size() > MAX_PARTS)
        shots.resize(MAX_PARTS);
    copies = std::max(1, std::min(copies, MAX_EFFECTS / static_cast<int>(shots.size())));

    std::vector<uint8_t> data(PAGE_SIZE, 0xCC);
    std::fill(data.begin() + PAGE_INDEX, data.end(), 0);
    auto put = [&](uintptr_t at, const void* value, size_t size) { std::memcpy(&data[at], value, size); };
    auto put32 = [&](uintptr_t at, uint32_t value) { put(at, &value, 4); };
    auto put64 = [&](uintptr_t at, uint64_t value) { put(at, &value, 8); };

    put32(PAGE_INDEX, 0xFFFFFFFF);
    put(PAGE_NAME, file, std::min(std::strlen(file), NAME_SIZE - 1));     // The rest of the page is 0
    for (size_t k = 0; k < shots.size(); k++) {
        float position[3] = { shots[k].position.x, shots[k].position.y, shots[k].position.z };
        float forward[3] = { shots[k].forward.x, shots[k].forward.y, shots[k].forward.z };
        put(PAGE_SHOTS + k * SHOT_SIZE, position, sizeof(position));
        put(PAGE_SHOTS + k * SHOT_SIZE + 0x10, forward, sizeof(forward));
    }

    // The request: name, how it is attached, the entity, no extras. Like CreateParticle of the scripts fills it
    const float none = std::numeric_limits<float>::max();
    put64(PAGE_DATA + 0x00, this->page + PAGE_NAME);
    put32(PAGE_DATA + 0x08, follow ? ATTACH_ABSORIGIN_FOLLOW : ATTACH_WORLDORIGIN);
    put64(PAGE_DATA + 0x10, follow ? entity : 0);
    put32(PAGE_DATA + 0x3C, 0xBF800000);    // -1.f
    put32(PAGE_DATA + 0x40, 0xFFFFFFFF);
    put32(PAGE_DATA + 0x44, 0xBF800000);
    float nowhere[3] = { none, none, none };
    put(PAGE_DATA + 0x48, nowhere, sizeof(nowhere));
    put(PAGE_DATA + 0x58, nowhere, sizeof(nowhere));

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    std::vector<size_t> to_end;
    auto jump_end = [&](std::initializer_list<uint8_t> opcode) { emit(opcode); emit32(0); to_end.push_back(code.size()); };

    emit({ 0x53 });                                         // push rbx
    emit({ 0x48, 0x83, 0xEC, 0x30 });                       // sub rsp, 0x30

    // The particle manager of the game
    emit({ 0x48, 0xB8 }); emit64(client + pt::fnGetManager);    // mov rax, GetManager
    emit({ 0xFF, 0xD0 });                                       // call rax
    emit({ 0x48, 0x8B, 0xD8 });                                 // mov rbx, rax
    emit({ 0x48, 0x85, 0xDB });                                 // test rbx, rbx
    jump_end({ 0x0F, 0x84 });                                   // jz end

    for (size_t shot = 0; shot < shots.size(); shot++) {
        auto position = this->page + PAGE_SHOTS + shot * SHOT_SIZE;
        bool directed = !shots[shot].forward.zero() && pt::fnSetControlPointForward;

        for (int copy = 0; copy < copies; copy++) {
            // CreateEffectIndex(manager, &index, &request)
            emit({ 0x48, 0xB8 }); emit64(this->page + PAGE_INDEX);  // mov rax, &index
            emit({ 0xC7, 0x00 }); emit32(0xFFFFFFFF);               // mov dword ptr [rax], -1
            emit({ 0x48, 0x8B, 0xCB });                             // mov rcx, rbx
            emit({ 0x48, 0xBA }); emit64(this->page + PAGE_INDEX);  // mov rdx, &index
            emit({ 0x49, 0xB8 }); emit64(this->page + PAGE_DATA);   // mov r8, &request
            emit({ 0x48, 0xB8 }); emit64(client + pt::fnCreateEffect); // mov rax, CreateEffectIndex
            emit({ 0xFF, 0xD0 });                                   // call rax

            emit({ 0x48, 0xB8 }); emit64(this->page + PAGE_INDEX);  // mov rax, &index
            emit({ 0x8B, 0x10 });                                   // mov edx, [rax]
            emit({ 0x83, 0xFA, 0xFF });                             // cmp edx, -1
            jump_end({ 0x0F, 0x84 });                               // je end

            // In the world: control point 0 at its place, the way it goes too when it has one
            if (!follow) {
                emit({ 0x48, 0x8B, 0xCB });                         // mov rcx, rbx
                emit({ 0x45, 0x31, 0xC0 });                         // xor r8d, r8d
                emit({ 0x49, 0xB9 }); emit64(position);             // mov r9, &position
                if (directed) {
                    // SetControlPointForward(manager, index, 0, &position, &forward, 0.f)
                    emit({ 0x48, 0xB8 }); emit64(position + 0x10);  // mov rax, &forward
                    emit({ 0x48, 0x89, 0x44, 0x24, 0x20 });         // mov [rsp + 0x20], rax
                    emit({ 0xC7, 0x44, 0x24, 0x28 }); emit32(0);    // mov dword ptr [rsp + 0x28], 0
                    emit({ 0x48, 0xB8 }); emit64(client + pt::fnSetControlPointForward); // mov rax, SetControlPointForward
                }
                else {
                    // SetControlPoint(manager, index, 0, &position, 0.f)
                    emit({ 0xC7, 0x44, 0x24, 0x20 }); emit32(0);    // mov dword ptr [rsp + 0x20], 0
                    emit({ 0x48, 0xB8 }); emit64(client + pt::fnSetControlPoint); // mov rax, SetControlPoint
                }
                emit({ 0xFF, 0xD0 });                               // call rax
                emit({ 0x48, 0xB8 }); emit64(this->page + PAGE_INDEX); // mov rax, &index
                emit({ 0x8B, 0x10 });                               // mov edx, [rax]
            }

            // The index is let go, the effect plays to its end. ReleaseParticleIndex: vtable + 0x18 of the manager
            emit({ 0x48, 0x8B, 0xCB });                             // mov rcx, rbx
            emit({ 0x48, 0x8B, 0x03 });                             // mov rax, [rbx]
            emit({ 0xFF, 0x50, 0x18 });                             // call [rax + 0x18]
        }
    }

    for (auto at : to_end) {
        int32_t rel = static_cast<int32_t>(code.size() - at);
        std::memcpy(&code[at - 4], &rel, 4);
    }
    emit({ 0x48, 0x83, 0xC4, 0x30 });                           // add rsp, 0x30
    emit({ 0x5B });                                             // pop rbx
    emit({ 0x33, 0xC0 });                                       // xor eax, eax
    emit({ 0xC3 });                                             // ret

    if (code.size() > PAGE_INDEX) {
        LOGF(WARNING, "Kill effect: the code of {} effects is too large", shots.size() * copies);
        return;
    }
    std::copy(code.begin(), code.end(), data.begin());
    p->write_bytes(this->page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), data.size());

    if (!GameThread::Call(this->page, 500)) {
        // Might still be running, never written over
        LOGF(WARNING, "Kill effect: the call did not run in time");
        this->page = 0;
        return;
    }

    int32_t index = p->read<int32_t>(this->page + PAGE_INDEX);
    if (!this->quiet)
        LOGF(VERBOSE, "Kill effect {} x{} at {} places on 0x{:X}: last index {}", file, copies, shots.size(), entity, index);
}

void KillEffect::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;
    std::this_thread::sleep_for(POLL * 2);
    i.bodies.clear();
    MaterialChams::SetHiddenBodies({});

    // The gravity of the player back
    auto p = Engine::GetProcess();
    if (i.gravity_applied && i.gravity_convar && p) {
        p->write<float>(i.gravity_convar + View::CONVAR_VALUE, i.gravity_original);
        i.gravity_applied = false;
    }
}
