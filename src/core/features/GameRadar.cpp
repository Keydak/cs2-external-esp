#include "GameRadar.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Dumper.hpp"

#include <unordered_set>

namespace {
    // The game clears the flag again once the server says nobody sees the player, so it is written often
    constexpr auto UPDATE_INTERVAL = 30ms;
}

bool GameRadar::Init() {
    return GetInstance().InitImpl();
}

bool GameRadar::IsAvailable() {
    return Engine::IsInsecure();
}

bool GameRadar::InitImpl() {
    if (!IsAvailable())
        return false;

    std::thread(&GameRadar::Thread, this).detach();

    LOGF(INFO, "Successfully initialized game radar...");
    return true;
}

void GameRadar::Thread() {
    // Pawns we marked as spotted, cleared again when the internal radar is turned off
    std::unordered_set<uintptr_t> marked;

    while (true) {
        std::this_thread::sleep_for(UPDATE_INTERVAL);

        auto p = Engine::GetProcess();
        if (!p)
            continue;

        bool active = cfg::world::radar::enabled && cfg::world::radar::mode == cfg::world::radar::MODE_GAME;

        if (!active) {
            if (!marked.empty()) {
                Restore(marked);
                marked.clear();
            }
            continue;
        }

        auto snapshot = Cache::CopySnapshot();

        // Our bit in the spotted by masks, the player slot is the controller entity index minus one
        int slot = snapshot.local.index;
        bool has_slot = slot >= 0 && slot < 64;

        for (const auto& player : snapshot.players) {
            if (player.localplayer || !player.alive || !snapshot.game.IsEnemy(snapshot.local.team, player.team))
                continue;

            auto pawn = player.GetPawnAddress();
            if (!pawn)
                continue;

            marked.insert(pawn);
            auto state = pawn + offsets::pawn::m_entitySpottedState;

            auto spotted = state + offsets::pawn::m_bSpotted;
            if (!p->read<bool>(spotted))
                p->write<bool>(spotted, true);

            // With mp_teammates_are_enemies (deathmatch) the radar ignores m_bSpotted & only shows players spotted by us
            if (!has_slot)
                continue;

            auto mask = state + offsets::pawn::m_bSpottedByMask + (slot / 32) * 4;
            auto bit = 1u << (slot % 32);
            auto value = p->read<uint32_t>(mask);

            if (!(value & bit))
                p->write<uint32_t>(mask, value | bit);
        }
    }
}

void GameRadar::Restore(const std::unordered_set<uintptr_t>& marked) {
    auto p = Engine::GetProcess();
    auto snapshot = Cache::CopySnapshot();

    int slot = snapshot.local.index;
    bool has_slot = slot >= 0 && slot < 64;

    // Only pawns that are still there, a pawn that left may be freed memory by now
    for (const auto& player : snapshot.players) {
        auto pawn = player.GetPawnAddress();
        if (!pawn || !marked.contains(pawn))
            continue;

        // The game puts them back on the radar on its own while someone really sees them
        auto state = pawn + offsets::pawn::m_entitySpottedState;
        p->write<bool>(state + offsets::pawn::m_bSpotted, false);

        if (has_slot) {
            auto mask = state + offsets::pawn::m_bSpottedByMask + (slot / 32) * 4;
            auto value = p->read<uint32_t>(mask);
            p->write<uint32_t>(mask, value & ~(1u << (slot % 32)));
        }
    }
}
