#include "Items.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/classes/Weapon.hpp"
#include "core/offsets/Dumper.hpp"
#include "assets/fonts/WeaponIcons.h"

namespace {
    // Same layout as the grenade scan, dropped items live well within the first chunks
    constexpr int CHUNK_COUNT = 8;
    constexpr int CHUNK_SIZE = 512;
    constexpr size_t IDENTITY_SIZE = 0x70;

    constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFF;

    bool IsUtility(int index) {
        switch (index) {
        case weapon_flashbang:
        case weapon_hegrenade:
        case weapon_frag_grenade:
        case weapon_smokegrenade:
        case weapon_molotov:
        case weapon_decoy:
        case weapon_incgrenade:
            return true;
        }
        return false;
    }
}

Items::NameKind Items::GetKind(uintptr_t name_address) {
    if (auto it = kinds.find(name_address); it != kinds.end())
        return it->second;

    char name[48]{};
    Engine::GetProcess()->read_raw(name_address, name, sizeof(name) - 1);

    std::string_view view(name);
    auto kind = NameKind::None;

    if (view.starts_with("weapon_"))
        kind = NameKind::Weapon;
    else if (view == "item_defuser" || view == "item_cutters")
        kind = NameKind::Kit;

    kinds[name_address] = kind;
    return kind;
}

bool Items::Update(uintptr_t entity_list) {
    auto p = Engine::GetProcess();
    list.clear();

    if (!p || !entity_list)
        return false;

    // Names are cached by where their text is, the game puts other names there after a map change
    auto now = std::chrono::steady_clock::now();
    if (now - kinds_cleared > std::chrono::seconds(5)) {
        kinds.clear();
        kinds_cleared = now;
    }

    buffer.resize(IDENTITY_SIZE * CHUNK_SIZE);

    for (int chunk = 0; chunk < CHUNK_COUNT; chunk++) {
        auto chunk_address = p->read<uintptr_t>(entity_list + 0x10 + 0x8 * chunk);

        if (!chunk_address || !p->read_raw(chunk_address, buffer.data(), buffer.size()))
            continue;

        for (int i = 0; i < CHUNK_SIZE; i++) {
            auto identity = buffer.data() + IDENTITY_SIZE * i;

            auto entity = *reinterpret_cast<uintptr_t*>(identity);
            auto name_address = *reinterpret_cast<uintptr_t*>(identity + offsets::grenade::m_designerName);
            if (!entity || !name_address)
                continue;

            auto kind = GetKind(name_address);
            if (kind == NameKind::None)
                continue;

            // Held or in someone's inventory
            if (p->read<uint32_t>(entity + offsets::econ::m_hOwnerEntity) != INVALID_HANDLE)
                continue;

            Item item;
            item.entity = entity;

            item.node = p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode);
            if (item.node)
                item.pos = p->read<Vec3_t>(item.node + offsets::bomb::m_vecAbsOrigin);

            if (item.pos.zero())
                continue;

            if (kind == NameKind::Kit) {
                item.kind = ItemKind::Kit;
                item.name = "Defuse Kit";
                item.icon = WeaponIcons::CUTTERS;
            }
            else {
                item.item_index = p->read<uint16_t>(entity + offsets::pawn::m_AttributeManager + offsets::pawn::m_Item + offsets::pawn::m_iItemDefinitionIndex);
                if (!item.item_index)
                    continue;

                item.kind = item.item_index == weapon_c4 ? ItemKind::Bomb : IsUtility(item.item_index) ? ItemKind::Utility : ItemKind::Weapon;
                item.name = Weapon::NameFor(item.item_index);
                item.icon = Weapon::IconFor(item.item_index);

                if (item.kind == ItemKind::Weapon)
                    item.ammo = p->read<int32_t>(entity + offsets::pawn::m_iClip1);
            }

            list.push_back(std::move(item));
        }
    }

    return true;
}

void Items::UpdatePositions() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    for (auto& item : list) {
        if (!item.node)
            continue;

        auto pos = p->read<Vec3_t>(item.node + offsets::bomb::m_vecAbsOrigin);
        if (!pos.zero())
            item.pos = pos;
    }
}
