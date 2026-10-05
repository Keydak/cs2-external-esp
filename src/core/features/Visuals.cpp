#include "Visuals.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/engine/GameThread.hpp"

namespace {
    namespace vis = offsets::visuals;

    constexpr auto POLL = 3ms;                   // The game puts its own values back, so the local ones are written often
    constexpr auto TARGETS_REFRESH = 100ms;      // What glows, from the cache
    constexpr auto GLOW_POLL = 15ms;             // Few reads per entity, but many entities
    constexpr float FLASH_FULL = 255.f;          // m_flFlashMaxAlpha of the game
    constexpr int GLOW_THROUGH_WALLS = 3;
    constexpr auto CHAMS_RETRY = 1s;             // After a call into the game that did not work

    // Glow chams page: the original updater, a mark to find it again, the table of pawns & our updater
    constexpr size_t GLOW_PAGE_SIZE = 0x2000;
    constexpr size_t GLOW_ORIGINAL = 0x00;
    constexpr size_t GLOW_MARK = 0x08;
    constexpr uint64_t GLOW_MARK_VALUE = 0x574F4C47534D4148; // "HAMSGLOW"
    // { pawn, 0, color, strength: float[4] each }, ends with pawn 0
    constexpr size_t GLOW_TABLE = 0x100;
    constexpr size_t GLOW_ENTRY = 0x30;
    constexpr size_t GLOW_MAX_PAWNS = 64;
    constexpr size_t GLOW_CODE = 0x1800;
    constexpr uintptr_t ALLOCATION_GRANULARITY = 0x10000; // Where VirtualAllocEx starts our pages

    constexpr size_t UPDATER_FUNCTION = 0x08;   // In the updater handle, after its parameter (the pawn)
    constexpr uint32_t SPAWN_INVULNERABILITY = 0x244EC9B0;  // murmur2 of the attribute names
    constexpr uint32_t INVULNERABILITY_COLOR = 0xB2CAF4DF;
    constexpr float GLOW_PARTIAL = 0.5f;          // Strength of the glow types over the model
    constexpr float SMOKE_AGE = 1000.f;          // Seconds a hidden cloud is made older, it is gone after 22

    uint32_t Pack(const color_t& color) {
        auto byte = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f); };
        return byte(color.r) | byte(color.g) << 8 | byte(color.b) << 16 | byte(color.a) << 24;
    }
}

const color_t& Visuals::ThrownColor(GrenadeType type) {
    namespace thrown = cfg::visuals::glow::thrown_colors;

    switch (type) {
    case GrenadeType::Smoke:    return thrown::smoke;
    case GrenadeType::Molotov:  return thrown::molotov;
    case GrenadeType::Flash:    return thrown::flash;
    case GrenadeType::HE:       return thrown::he;
    default:                    return thrown::decoy;
    }
}

bool Visuals::Init() {
    return GetInstance().InitImpl();
}

bool Visuals::IsAvailable() {
    return Engine::IsInsecure();
}

bool Visuals::InitImpl() {
    std::thread(&Visuals::Thread, this).detach();

    LOGF(INFO, "Successfully initialized visuals...");
    return true;
}

bool Visuals::IsLive(uintptr_t entity) {
    auto p = Engine::GetProcess();
    if (!p || !entity)
        return false;

    auto identity = p->read<uintptr_t>(entity + 0x10);
    return identity && p->read<uintptr_t>(identity) == entity;
}

uintptr_t Visuals::GetActiveWeapon(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    auto services = p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices);
    return services ? Engine::GetEntityFromHandle(p->read<uint32_t>(services + offsets::pawn::m_hActiveWeapon)) : 0;
}

bool Visuals::IsZoomed(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    if (!p || !pawn)
        return false;

    if (p->read<bool>(pawn + offsets::pawn::m_bIsScoped))
        return true;

    auto weapon = GetActiveWeapon(pawn);
    if (!weapon)
        return false;

    // m_zoomLevel is a field of guns only: on a knife, grenade or the bomb the same place holds something else
    switch (p->read<uint16_t>(weapon + offsets::pawn::m_AttributeManager + offsets::pawn::m_Item + offsets::pawn::m_iItemDefinitionIndex)) {
    case 8:     // AUG
    case 9:     // AWP
    case 11:    // G3SG1
    case 38:    // SCAR-20
    case 39:    // SG 553
    case 40:    // SSG 08
        return p->read<int>(weapon + vis::m_zoomLevel) > 0;
    default:
        return false;
    }
}

void Visuals::Thread() {
    auto next_targets = std::chrono::steady_clock::now();
    auto next_glow = next_targets;
    bool was_available = false;

    while (!this->stopping) {
        std::this_thread::sleep_for(POLL);

        auto p = Engine::GetProcess();
        if (!p)
            continue;

        auto pawn = Engine::GetLocalPawn();
        if (pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0)
            pawn = 0;

        if (!IsAvailable())
            continue;
        was_available = true;

        UpdateFlash(pawn);

        // The cache only while something needs it, or something of ours is left to turn off
        namespace glow = cfg::visuals::glow;
        bool needed = cfg::visuals::no_smoke || glow::enemies || glow::team || glow::bomb || glow::items || glow::utility || glow::dropped_bomb
            || glow::thrown || !this->glowing.empty() || !this->hidden_smokes.empty() || !this->glow_targets.empty()
            || cfg::visuals::chams::enemies || cfg::visuals::chams::team || !this->tinted.empty() || !this->glow_hooked.empty();

        auto now = std::chrono::steady_clock::now();
        if (needed && now >= next_targets) {
            next_targets = now + TARGETS_REFRESH;
            RefreshTargets();
            UpdateSmoke();
        }

        if (needed && now >= next_glow) {
            next_glow = now + GLOW_POLL;
            UpdateGlow();
            UpdateChams();
        }
    }

    if (was_available)
        Restore();
}

void Visuals::UpdateFlash(uintptr_t pawn) {
    // Dead: the pawn keeps our value, put back once alive again
    if (!pawn)
        return;

    auto p = Engine::GetProcess();
    auto address = pawn + vis::m_flFlashMaxAlpha;

    if (cfg::visuals::no_flash) {
        float wanted = std::clamp(cfg::visuals::flash_alpha, 0.f, FLASH_FULL);
        if (p->read<float>(address) != wanted)
            p->write<float>(address, wanted);

        this->flash_written = true;
        return;
    }

    // Turned off: the game keeps our value otherwise
    if (this->flash_written)
        p->write<float>(address, FLASH_FULL);
    this->flash_written = false;
}

void Visuals::RefreshTargets() {
    namespace glow = cfg::visuals::glow;

    auto snapshot = Cache::CopySnapshot();
    const auto& local = snapshot.local;

    this->glow_targets.clear();
    this->chams_targets.clear();
    this->smokes.clear();
    this->known.clear();

    auto add = [&](uintptr_t entity, const color_t& color) {
        if (entity)
            this->glow_targets.push_back({ entity, Pack(color) });
    };

    for (const auto& player : snapshot.players) {
        auto pawn = player.GetPawnAddress();
        this->known.insert(pawn);

        if (player.localplayer || !player.alive)
            continue;

        bool enemy = snapshot.game.IsEnemy(local.team, player.team);
        // Like the rest of the player ESP, off with the switch of its group
        if (enemy && glow::enemies && cfg::esp::enemy.enabled)
            add(pawn, glow::enemy_color);
        else if (!enemy && glow::team && cfg::esp::team.enabled)
            add(pawn, glow::team_color);

        // Chams: always opaque, a see through render color needs a render mode the pawn does not have
        namespace chams = cfg::visuals::chams;
        const auto& group = enemy ? cfg::esp::enemy : cfg::esp::team;
        if (pawn && group.enabled && (enemy ? chams::enemies : chams::team)) {
            int type = enemy ? chams::enemy_type : chams::team_type;
            if (!HasModelGlow())
                type = chams::TYPE_TEXTURED;

            ChamsTarget target;
            target.pawn = pawn;
            target.color = enemy ? chams::enemy_color : chams::team_color;
            target.tint = type == chams::TYPE_TEXTURED || type == chams::TYPE_BOTH;
            target.glow = type == chams::TYPE_GLOW || type == chams::TYPE_BOTH;
            // Solid: the model black under the glow, only the color of the glow left
            target.tint_color = target.color;
            // At full strength the glow covers the whole model, whatever is under it: half lets the model show
            target.glow_strength = GLOW_PARTIAL;
            this->chams_targets.push_back(target);
        }
    }

    if (snapshot.bomb.is_planted) {
        this->known.insert(snapshot.bomb.GetAddress());
        if (glow::bomb)
            add(snapshot.bomb.GetAddress(), glow::bomb_color);
    }

    for (const auto& item : snapshot.items) {
        this->known.insert(item.entity);

        // A dropped bomb is an item, the bomb glow is the planted one
        switch (item.kind) {
        case ItemKind::Utility: if (glow::utility) add(item.entity, glow::utility_color);   break;
        case ItemKind::Bomb:    if (glow::dropped_bomb) add(item.entity, glow::dropped_bomb_color); break;
        default:                if (glow::items) add(item.entity, glow::item_color);        break;
        }
    }

    for (const auto& grenade : snapshot.grenades) {
        this->known.insert(grenade.entity);

        if (grenade.type == GrenadeType::Smoke && grenade.detonated)
            this->smokes.push_back(grenade.entity);

        // In flight, popped ones are only an effect
        if (glow::thrown && !grenade.detonated && grenade.type != GrenadeType::Fire)
            add(grenade.entity, ThrownColor(grenade.type));
    }
}

void Visuals::UpdateSmoke() {
    auto p = Engine::GetProcess();

    // Puts back the start of a cloud, while it is still there
    auto restore = [&](uintptr_t entity, const HiddenSmoke& hidden) {
        if (!IsLive(entity))
            return;

        auto volume = entity + vis::smokeVolume;
        p->write<float>(volume + vis::smokeVolumeStart, hidden.volume_start);
        if (hidden.render && p->read<uintptr_t>(volume + vis::smokeRenderObject) == hidden.render)
            p->write<float>(hidden.render + vis::smokeRenderStart, hidden.render_start);
    };

    if (cfg::visuals::no_smoke && vis::smokeVolume) {
        // The cloud fades out with its age, from 17s to 22s: started long ago, it is gone. The ESP keeps the smoke
        for (auto entity : this->smokes) {
            if (this->hidden_smokes.contains(entity) || !IsLive(entity) || !p->read<bool>(entity + vis::m_bSmokeEffectSpawned))
                continue;

            auto volume = entity + vis::smokeVolume;
            HiddenSmoke hidden;
            hidden.volume_start = p->read<float>(volume + vis::smokeVolumeStart);
            hidden.render = p->read<uintptr_t>(volume + vis::smokeRenderObject);
            if (hidden.render)
                hidden.render_start = p->read<float>(hidden.render + vis::smokeRenderStart);

            p->write<float>(volume + vis::smokeVolumeStart, hidden.volume_start - SMOKE_AGE);
            if (hidden.render)
                p->write<float>(hidden.render + vis::smokeRenderStart, hidden.render_start - SMOKE_AGE);

            this->hidden_smokes.emplace(entity, hidden);
        }
    }
    else {
        // Back, but only on smokes still there
        for (const auto& [entity, hidden] : this->hidden_smokes)
            if (this->known.contains(entity))
                restore(entity, hidden);
        this->hidden_smokes.clear();
    }

    std::erase_if(this->hidden_smokes, [&](const auto& entry) { return !this->known.contains(entry.first); });
}

void Visuals::UpdateGlow() {
    auto p = Engine::GetProcess();

    std::unordered_map<uintptr_t, uint32_t> wanted;
    for (auto [entity, color] : this->glow_targets) {
        if (!IsLive(entity))
            continue;

        auto glow = entity + vis::m_Glow;
        if (p->read<int>(glow + vis::m_iGlowType) != GLOW_THROUGH_WALLS)
            p->write<int>(glow + vis::m_iGlowType, GLOW_THROUGH_WALLS);
        if (p->read<uint32_t>(glow + vis::m_glowColorOverride) != color)
            p->write<uint32_t>(glow + vis::m_glowColorOverride, color);
        if (!p->read<bool>(glow + vis::m_bGlowing))
            p->write<bool>(glow + vis::m_bGlowing, true);

        wanted.emplace(entity, color);
    }

    // No longer wanted: off again, only on entities the cache still has, the others may be freed memory
    for (auto [entity, color] : this->glowing) {
        if (wanted.contains(entity) || !this->known.contains(entity) || !IsLive(entity))
            continue;

        p->write<bool>(entity + vis::m_Glow + vis::m_bGlowing, false);
        p->write<uint32_t>(entity + vis::m_Glow + vis::m_glowColorOverride, 0);
    }

    this->glowing = std::move(wanted);
}

bool Visuals::HasModelGlow() {
    return vis::m_pSceneObjectUpdater && vis::m_pSceneNode && vis::sceneNodeCount && vis::sceneNodeList && vis::sceneHandleObject
        && vis::sceneObjectAttributes && vis::dwSceneSystem && vis::sceneSystemAllocateAttributes && vis::fnSetAttributeFloat4;
}

bool Visuals::SetRenderColors(const std::vector<std::pair<uintptr_t, uint32_t>>& colors) {
    auto p = Engine::GetProcess();

    // CEntityInstance::m_pEntity, its CEntityIdentity is the slot of the entity list: the entity first, its handle at 0x10
    constexpr std::ptrdiff_t ENTITY_IDENTITY = 0x10;
    constexpr std::ptrdiff_t IDENTITY_HANDLE = 0x10;
    constexpr size_t CODE_SIZE = 0x1000;
    constexpr size_t COLORS = 0xC00;    // The colors the calls point at, after the code
    constexpr size_t MAX_CALLS = 32;

    if (!vis::fnSetRenderColor || colors.empty() || !GameThread::Ensure())
        return false;

    if (!this->chams_code && !(this->chams_code = p->allocate_remote(CODE_SIZE, PAGE_EXECUTE_READWRITE)))
        return false;

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    auto function = Engine::GetClient().base + vis::fnSetRenderColor;
    std::vector<uint32_t> data;

    emit({ 0x48, 0x83, 0xEC, 0x28 });                   // sub rsp, 0x28

    for (auto [pawn, color] : colors) {
        if (data.size() >= MAX_CALLS)
            break;

        auto identity = p->read<uintptr_t>(pawn + ENTITY_IDENTITY);
        if (!identity || p->read<uintptr_t>(identity) != pawn)
            continue;
        auto handle = p->read<uint32_t>(identity + IDENTITY_HANDLE);

        // The slot still holds this pawn with the same handle, or its call is skipped
        emit({ 0x48, 0xB8 }); emit64(identity);         // mov rax, identity
        emit({ 0x48, 0xB9 }); emit64(pawn);             // mov rcx, pawn
        emit({ 0x48, 0x39, 0x08 });                     // cmp [rax], rcx
        emit({ 0x0F, 0x85 }); emit32(0);                // jne next
        size_t skip_entity = code.size() - 4;
        emit({ 0x81, 0x78, IDENTITY_HANDLE }); emit32(handle); // cmp dword ptr [rax + 0x10], handle
        emit({ 0x0F, 0x85 }); emit32(0);                // jne next
        size_t skip_handle = code.size() - 4;

        emit({ 0x48, 0xBA }); emit64(this->chams_code + COLORS + data.size() * 4); // mov rdx, &color
        emit({ 0x49, 0xBB }); emit64(function);         // mov r11, SetRenderColor
        emit({ 0x41, 0xFF, 0xD3 });                     // call r11

        auto patch = [&](size_t at) {
            auto offset = static_cast<uint32_t>(code.size() - (at + 4));
            for (int i = 0; i < 4; i++)
                code[at + i] = static_cast<uint8_t>(offset >> (i * 8));
        };
        patch(skip_entity);
        patch(skip_handle);

        data.push_back(color);
    }

    emit({ 0x48, 0x83, 0xC4, 0x28 });                   // add rsp, 0x28
    emit({ 0x33, 0xC0 });                               // xor eax, eax
    emit({ 0xC3 });                                     // ret

    if (data.empty() || code.size() > COLORS)
        return false;

    p->write_bytes(this->chams_code + COLORS, std::vector<uint8_t>(reinterpret_cast<uint8_t*>(data.data()),
        reinterpret_cast<uint8_t*>(data.data()) + data.size() * 4));
    p->write_bytes(this->chams_code, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->chams_code), code.size());

    if (!GameThread::Call(this->chams_code)) {
        // Might still run later: this code is left alone, the next calls get new memory
        LOGF(WARNING, "Chams: the call into the game did not finish in time");
        this->chams_code = 0;
        return false;
    }

    return true;
}

bool Visuals::EnsureGlowUpdater() {
    if (this->glow_page)
        return true;
    if (!HasModelGlow())
        return false;

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient().base;

    auto page = p->allocate_remote(GLOW_PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!page)
        return false;

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    // A jump with a rel32 filled in once its target is known
    auto jump = [&](std::initializer_list<uint8_t> opcode) { emit(opcode); emit32(0); return code.size() - 4; };
    auto bind = [&](size_t at, size_t target) {
        auto offset = static_cast<uint32_t>(static_cast<int32_t>(target) - static_cast<int32_t>(at + 4));
        for (int i = 0; i < 4; i++)
            code[at + i] = static_cast<uint8_t>(offset >> (i * 8));
    };

    auto set_attribute = client + vis::fnSetAttributeFloat4;

    // Called by the game with (pawn, ?, bool): the original first, then the spawn protection on the scene objects
    // of the pawn when it is in the table
    emit({ 0x53 });                                         // push rbx
    emit({ 0x56 });                                         // push rsi
    emit({ 0x57 });                                         // push rdi
    emit({ 0x41, 0x54 });                                   // push r12
    emit({ 0x41, 0x55 });                                   // push r13
    emit({ 0x48, 0x83, 0xEC, 0x30 });                       // sub rsp, 0x30
    emit({ 0x48, 0x8B, 0xD9 });                             // mov rbx, rcx
    emit({ 0x48, 0xB8 }); emit64(page + GLOW_ORIGINAL);     // mov rax, &original
    emit({ 0xFF, 0x10 });                                   // call [rax]
    emit({ 0x48, 0x89, 0x44, 0x24, 0x28 });                 // mov [rsp + 0x28], rax

    // Our entry of the pawn
    emit({ 0x48, 0xBE }); emit64(page + GLOW_TABLE);        // mov rsi, table
    size_t find = code.size();
    emit({ 0x48, 0x8B, 0x06 });                             // mov rax, [rsi]
    emit({ 0x48, 0x85, 0xC0 });                             // test rax, rax
    auto to_done_1 = jump({ 0x0F, 0x84 });                  // jz done
    emit({ 0x48, 0x3B, 0xC3 });                             // cmp rax, rbx
    auto to_found = jump({ 0x0F, 0x84 });                   // je found
    emit({ 0x48, 0x83, 0xC6, static_cast<uint8_t>(GLOW_ENTRY) }); // add rsi, entry
    auto to_find = jump({ 0xE9 });                          // jmp find
    bind(to_find, find);

    // Each scene object of its model
    bind(to_found, code.size());
    emit({ 0x4C, 0x8B, 0xA3 }); emit32(static_cast<uint32_t>(vis::m_pSceneNode)); // mov r12, [rbx + node]
    emit({ 0x4D, 0x85, 0xE4 });                             // test r12, r12
    auto to_done_2 = jump({ 0x0F, 0x84 });                  // jz done
    emit({ 0x33, 0xFF });                                   // xor edi, edi
    size_t object = code.size();
    emit({ 0x41, 0x3B, 0xBC, 0x24 }); emit32(static_cast<uint32_t>(vis::sceneNodeCount)); // cmp edi, [r12 + count]
    auto to_done_3 = jump({ 0x0F, 0x8D });                  // jge done
    emit({ 0x49, 0x8B, 0x84, 0x24 }); emit32(static_cast<uint32_t>(vis::sceneNodeList)); // mov rax, [r12 + list]
    emit({ 0x48, 0x63, 0xD7 });                             // movsxd rdx, edi
    emit({ 0x48, 0x8B, 0x04, 0xD0 });                       // mov rax, [rax + rdx * 8]
    emit({ 0x48, 0x85, 0xC0 });                             // test rax, rax
    auto to_next_1 = jump({ 0x0F, 0x84 });                  // jz next
    emit({ 0x4C, 0x8B, 0xA8 }); emit32(static_cast<uint32_t>(vis::sceneHandleObject)); // mov r13, [rax + object]
    emit({ 0x4D, 0x85, 0xED });                             // test r13, r13
    auto to_next_2 = jump({ 0x0F, 0x84 });                  // jz next

    // Its attribute list, made by the scene system like the game does when there is none yet
    emit({ 0x49, 0x8B, 0x8D }); emit32(static_cast<uint32_t>(vis::sceneObjectAttributes)); // mov rcx, [r13 + attributes]
    emit({ 0x48, 0x85, 0xC9 });                             // test rcx, rcx
    auto to_have = jump({ 0x0F, 0x85 });                    // jnz have
    emit({ 0x48, 0xB8 }); emit64(client + vis::dwSceneSystem); // mov rax, &scene system
    emit({ 0x48, 0x8B, 0x08 });                             // mov rcx, [rax]
    emit({ 0x48, 0x85, 0xC9 });                             // test rcx, rcx
    auto to_next_3 = jump({ 0x0F, 0x84 });                  // jz next
    emit({ 0x49, 0x8B, 0xD5 });                             // mov rdx, r13
    emit({ 0x48, 0x8B, 0x01 });                             // mov rax, [rcx]
    emit({ 0xFF, 0x90 }); emit32(static_cast<uint32_t>(vis::sceneSystemAllocateAttributes)); // call [rax + allocate]
    emit({ 0x49, 0x8B, 0x8D }); emit32(static_cast<uint32_t>(vis::sceneObjectAttributes)); // mov rcx, [r13 + attributes]
    emit({ 0x48, 0x85, 0xC9 });                             // test rcx, rcx
    auto to_next_4 = jump({ 0x0F, 0x84 });                  // jz next

    bind(to_have, code.size());
    emit({ 0xBA }); emit32(SPAWN_INVULNERABILITY);          // mov edx, "SpawnInvulnerability"
    emit({ 0x4C, 0x8D, 0x46, 0x20 });                       // lea r8, [rsi + 0x20] (strength)
    emit({ 0x48, 0xB8 }); emit64(set_attribute);            // mov rax, SetAttributeFloat4
    emit({ 0xFF, 0xD0 });                                   // call rax
    emit({ 0x49, 0x8B, 0x8D }); emit32(static_cast<uint32_t>(vis::sceneObjectAttributes)); // mov rcx, [r13 + attributes]
    emit({ 0xBA }); emit32(INVULNERABILITY_COLOR);          // mov edx, "InvulnerabilityColor"
    emit({ 0x4C, 0x8D, 0x46, 0x10 });                       // lea r8, [rsi + 0x10] (color)
    emit({ 0x48, 0xB8 }); emit64(set_attribute);            // mov rax, SetAttributeFloat4
    emit({ 0xFF, 0xD0 });                                   // call rax

    size_t next = code.size();
    bind(to_next_1, next);
    bind(to_next_2, next);
    bind(to_next_3, next);
    bind(to_next_4, next);
    emit({ 0xFF, 0xC7 });                                   // inc edi
    auto to_object = jump({ 0xE9 });                        // jmp object
    bind(to_object, object);

    size_t done = code.size();
    bind(to_done_1, done);
    bind(to_done_2, done);
    bind(to_done_3, done);
    emit({ 0x48, 0x8B, 0x44, 0x24, 0x28 });                 // mov rax, [rsp + 0x28]
    emit({ 0x48, 0x83, 0xC4, 0x30 });                       // add rsp, 0x30
    emit({ 0x41, 0x5D });                                   // pop r13
    emit({ 0x41, 0x5C });                                   // pop r12
    emit({ 0x5F });                                         // pop rdi
    emit({ 0x5E });                                         // pop rsi
    emit({ 0x5B });                                         // pop rbx
    emit({ 0xC3 });                                         // ret

    if (code.size() > GLOW_PAGE_SIZE - GLOW_CODE)
        return false;

    // No original yet & an empty table: nothing calls it before a pawn is switched over
    p->write<uintptr_t>(page + GLOW_ORIGINAL, 0);
    p->write<uint64_t>(page + GLOW_MARK, GLOW_MARK_VALUE);
    p->write<uintptr_t>(page + GLOW_TABLE, 0);
    p->write_bytes(page + GLOW_CODE, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(page + GLOW_CODE), code.size());

    this->glow_page = page;
    LOGF(VERBOSE, "Glow chams updater at 0x{:X}", page + GLOW_CODE);
    return true;
}

void Visuals::UpdateModelGlow() {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();
    auto in_client = [&](uintptr_t address) { return address >= client.base && address < client.base + client.size; };

    // The glow color with its strength as alpha
    struct Wanted {
        uintptr_t pawn;
        color_t glow{ 0.f, 0.f, 0.f, 0.f };
    };
    std::vector<Wanted> wanted;
    for (const auto& target : this->chams_targets)
        if (target.glow && wanted.size() < GLOW_MAX_PAWNS && IsLive(target.pawn))
            wanted.push_back({ target.pawn, color_t(target.color.r, target.color.g, target.color.b, target.color.a * target.glow_strength) });

    if (wanted.empty() && this->glow_hooked.empty())
        return;
    if (!EnsureGlowUpdater())
        return;

    auto stub = this->glow_page + GLOW_CODE;
    auto original = p->read<uintptr_t>(this->glow_page + GLOW_ORIGINAL);

    // The table before the updaters are switched over: { pawn, 0, color, strength }, then pawn 0
    std::vector<uint8_t> table((wanted.size() + 1) * GLOW_ENTRY, 0);
    for (size_t i = 0; i < wanted.size(); i++) {
        auto entry = table.data() + i * GLOW_ENTRY;
        const auto& w = wanted[i];
        float values[8] = { w.glow.r, w.glow.g, w.glow.b, 0.f, w.glow.a, w.glow.a, w.glow.a, w.glow.a };

        std::memcpy(entry, &w.pawn, sizeof(uintptr_t));
        std::memcpy(entry + 0x10, values, sizeof(values));
    }

    if (table != this->glow_table) {
        p->write_bytes(this->glow_page + GLOW_TABLE, table);
        this->glow_table = table;
    }

    std::unordered_map<uintptr_t, uintptr_t> hooked;
    for (const auto& w : wanted) {
        auto pawn = w.pawn;
        auto handle = p->read<uintptr_t>(pawn + vis::m_pSceneObjectUpdater);
        if (!handle)
            continue;

        auto function = p->read<uintptr_t>(handle + UPDATER_FUNCTION);
        if (function != stub) {
            // Still pointing to a page of ours from a run before, which keeps the original at its start. Of any
            // build: the code might be elsewhere in it, the page starts on the allocation granularity
            if (function && !in_client(function)) {
                auto page = function & ~(ALLOCATION_GRANULARITY - 1);
                if (p->read<uint64_t>(page + GLOW_MARK) == GLOW_MARK_VALUE)
                    function = p->read<uintptr_t>(page + GLOW_ORIGINAL);
            }

            // Only the updater of the game we know is replaced
            if (!in_client(function)) {
                static bool told = false;
                if (!told) {
                    told = true;
                    LOGF(WARNING, "Glow chams: a player has an updater we do not know at 0x{:X}, skipped", function);
                }
                continue;
            }

            if (!original) {
                original = function;
                p->write<uintptr_t>(this->glow_page + GLOW_ORIGINAL, original);
            }
            else if (function != original)
                continue;

            p->write<uintptr_t>(handle + UPDATER_FUNCTION, stub);
        }

        hooked.emplace(pawn, handle);
    }

    // No longer wanted: their own updater back, it takes the glow off the next frame. Only on pawns still there, the
    // cache has dead players as their spectator pawn
    for (const auto& [pawn, handle] : this->glow_hooked) {
        if (hooked.contains(pawn) || !IsLive(pawn))
            continue;
        if (p->read<uintptr_t>(pawn + vis::m_pSceneObjectUpdater) == handle && p->read<uintptr_t>(handle + UPDATER_FUNCTION) == stub)
            p->write<uintptr_t>(handle + UPDATER_FUNCTION, original);
    }

    this->glow_hooked = std::move(hooked);
}

void Visuals::RestoreModelGlow() {
    if (!this->glow_page)
        return;

    auto p = Engine::GetProcess();
    auto stub = this->glow_page + GLOW_CODE;
    auto original = p->read<uintptr_t>(this->glow_page + GLOW_ORIGINAL);

    for (const auto& [pawn, handle] : this->glow_hooked) {
        if (!original || !IsLive(pawn))
            continue;
        if (p->read<uintptr_t>(pawn + vis::m_pSceneObjectUpdater) == handle && p->read<uintptr_t>(handle + UPDATER_FUNCTION) == stub)
            p->write<uintptr_t>(handle + UPDATER_FUNCTION, original);
    }

    // An updater we missed only calls the original now. The page stays: the game might be running it right now
    p->write<uintptr_t>(this->glow_page + GLOW_TABLE, 0);
    this->glow_hooked.clear();
    this->glow_table.clear();
}

void Visuals::UpdateChams() {
    auto p = Engine::GetProcess();

    // Glow: written into the game, no call needed
    UpdateModelGlow();

    // A call that failed is tried again a bit later, it can block for a while
    auto now = std::chrono::steady_clock::now();
    if (now < this->chams_retry)
        return;

    std::unordered_map<uintptr_t, Tint> wanted;
    std::vector<std::pair<uintptr_t, uint32_t>> calls;

    for (const auto& target : this->chams_targets) {
        if (!target.tint || !IsLive(target.pawn))
            continue;

        // Textured is always opaque
        auto color = Pack(target.tint_color) | 0xFF000000u;
        auto current = p->read<uint32_t>(target.pawn + vis::m_clrRender);
        auto it = this->tinted.find(target.pawn);

        // The color of the game before ours, kept from the first time
        Tint tint;
        tint.original = it != this->tinted.end() ? it->second.original : current;
        tint.applied = color;
        tint.done = true;

        // Through the game, it gives the color to the model. Again when the game changed it
        if (it == this->tinted.end() || !it->second.done || it->second.applied != color || current != color)
            calls.push_back({ target.pawn, color });

        wanted.emplace(target.pawn, tint);
    }

    // No longer wanted: the color of the game back, on pawns that still exist. Not only the ones the cache has: a dead
    // player is their spectator pawn there, their own kept our color & came back with it the next round
    for (const auto& [pawn, tint] : this->tinted) {
        if (wanted.contains(pawn) || !IsLive(pawn))
            continue;
        calls.push_back({ pawn, tint.original });
    }

    // Not done this time: tried again later
    if (!calls.empty() && !SetRenderColors(calls)) {
        this->chams_retry = now + CHAMS_RETRY;
        for (auto& [pawn, tint] : wanted)
            if (std::any_of(calls.begin(), calls.end(), [&](const auto& call) { return call.first == pawn; }))
                tint.done = false;

        // Ones to turn back stay known
        for (const auto& [pawn, tint] : this->tinted)
            if (!wanted.contains(pawn) && IsLive(pawn))
                wanted.emplace(pawn, Tint{ tint.original, tint.original, true });

        // Without the call into the game it cannot work at all, said once
        static bool told = false;
        if (!told && !vis::fnSetRenderColor) {
            told = true;
            LOGF(WARNING, "Chams need 'SetRenderColor' of the game, it was not found");
        }
    }

    this->tinted = std::move(wanted);
}

void Visuals::Restore() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    auto pawn = Engine::GetLocalPawn();

    if (this->flash_written && pawn)
        p->write<float>(pawn + vis::m_flFlashMaxAlpha, FLASH_FULL);

    for (const auto& [entity, hidden] : this->hidden_smokes) {
        if (!IsLive(entity))
            continue;

        auto volume = entity + vis::smokeVolume;
        p->write<float>(volume + vis::smokeVolumeStart, hidden.volume_start);
        if (hidden.render && p->read<uintptr_t>(volume + vis::smokeRenderObject) == hidden.render)
            p->write<float>(hidden.render + vis::smokeRenderStart, hidden.render_start);
    }

    for (auto [entity, color] : this->glowing) {
        if (!IsLive(entity))
            continue;
        p->write<bool>(entity + vis::m_Glow + vis::m_bGlowing, false);
        p->write<uint32_t>(entity + vis::m_Glow + vis::m_glowColorOverride, 0);
    }

    this->flash_written = false;

    // The colors of the game back on the models, through the game like they were set
    std::vector<std::pair<uintptr_t, uint32_t>> colors;
    for (const auto& [pawn, tint] : this->tinted)
        if (tint.done && IsLive(pawn))
            colors.push_back({ pawn, tint.original });
    if (!colors.empty())
        SetRenderColors(colors);

    RestoreModelGlow();

    this->hidden_smokes.clear();
    this->glowing.clear();
    this->tinted.clear();
}

void Visuals::Shutdown() {
    auto& visuals = GetInstance();
    visuals.stopping = true;

    // The thread puts everything back on its way out
    std::this_thread::sleep_for(100ms);
}
