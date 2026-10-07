#include "HitEffects.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Offsets.hpp"
#include "core/features/Sounds.hpp"

namespace {
    constexpr auto POLL = std::chrono::milliseconds(4);
    constexpr auto MATCH_WINDOW = std::chrono::milliseconds(120);  // A hit & the health going down, apart at most this
    constexpr auto DROP_LIFETIME = std::chrono::milliseconds(500);
    constexpr auto KILL_WAIT = std::chrono::milliseconds(150);     // For our kill counter, after a hit someone died with
    constexpr size_t MAX_EVENTS = 32;
    constexpr int MAX_HITS_AT_ONCE = 12;   // More at once is the counter of another pawn or map, not hits
}

bool HitEffects::Init() {
    auto& effects = GetInstance();
    if (effects.running.exchange(true))
        return true;

    std::thread(&HitEffects::Thread, &effects).detach();
    return true;
}

void HitEffects::Shutdown() {
    GetInstance().stopping = true;
}

std::vector<HitEvent> HitEffects::Drain() {
    auto& effects = GetInstance();
    std::lock_guard lock(effects.mutex);

    std::vector<HitEvent> out;
    out.swap(effects.events);
    return out;
}

void HitEffects::Reset() {
    this->health.clear();
    this->drops.clear();
    this->pending_hits.clear();
    this->pending_kills.clear();
    this->last_pawn = 0;
    this->last_controller = 0;
    this->last_bullets = 0;
    this->last_hits = -1;
    this->last_kills = -1;
}

void HitEffects::Emit(const HitEvent& event) {
    // A sound per hit, all at once so they stack up (the pellets of a shotgun): the killing one gets the kill sound,
    // the others the hit sound
    int hit_sounds = event.hits;
    if (event.kill && cfg::misc::killsound) {
        Sounds::Play(cfg::misc::killsound_file, cfg::misc::killsound_volume);
        hit_sounds--;
    }

    if (cfg::misc::hitsound && hit_sounds > 0)
        Sounds::Play(cfg::misc::hitsound_file, cfg::misc::hitsound_volume, hit_sounds);

    std::lock_guard lock(this->mutex);
    if (this->events.size() >= MAX_EVENTS)
        this->events.erase(this->events.begin());

    this->events.push_back(event);
}

void HitEffects::Thread() {
    using clock = std::chrono::steady_clock;

    while (!this->stopping) {
        std::this_thread::sleep_for(POLL);

        namespace hm = cfg::world::hitmarker;
        if (!hm::crosshair && !hm::world && !cfg::misc::hitsound && !cfg::misc::killsound) {
            Reset();
            continue;
        }

        auto p = Engine::GetProcess();
        auto snapshot = Cache::Current();
        if (!p || !snapshot) {
            Reset();
            continue;
        }

        const auto& local = snapshot->local;
        uintptr_t pawn = local.GetPawnAddress();
        uintptr_t controller = local.GetControllerAddress();

        if (local.index < 0 || !pawn || !controller) {
            Reset();
            continue;
        }

        auto now = clock::now();

        // Health of the others, a drop is someone hurt
        for (const auto& player : snapshot->players) {
            if (player.localplayer || player.index == local.index || player.index < 0)
                continue;

            uintptr_t their_pawn = player.GetPawnAddress();
            if (!their_pawn)
                continue;

            int value = p->read<int32_t>(their_pawn + offsets::pawn::m_iHealth);
            auto& known = this->health[player.index];

            if (known.pawn == their_pawn && value < known.value && known.value > 0)
                this->drops.push_back({ player.index, known.value - value, value, now });

            known = { their_pawn, value };
        }

        while (!this->drops.empty() && now - this->drops.front().time > DROP_LIFETIME)
            this->drops.pop_front();

        // Our counters. A new pawn, controller or bullet services (respawn, another match), a number going down (new
        // round, reconnect) or jumping up by more than hits can, only sets where we are. Dead, nothing is ours
        int hits = -1, kills = -1;
        auto bullets = p->read<uintptr_t>(pawn + offsets::hits::m_pBulletServices);
        if (bullets)
            hits = p->read<int32_t>(bullets + offsets::hits::m_totalHitsOnServer);
        if (auto tracking = p->read<uintptr_t>(controller + offsets::hits::m_pActionTrackingServices))
            kills = p->read<int32_t>(tracking + offsets::hits::m_iNumRoundKills);

        bool alive = p->read<int32_t>(pawn + offsets::pawn::m_iHealth) > 0;

        if (pawn != this->last_pawn || bullets != this->last_bullets) {
            this->last_pawn = pawn;
            this->last_bullets = bullets;
            this->last_hits = -1;
        }

        if (controller != this->last_controller) {
            this->last_controller = controller;
            this->last_kills = -1;
            this->pending_kills.clear();
        }

        if (hits >= 0) {
            int added = this->last_hits >= 0 ? hits - this->last_hits : 0;
            if (alive && added > 0 && added <= MAX_HITS_AT_ONCE)
                this->pending_hits.push_back({ now, added });
            this->last_hits = hits;
        }

        // A kill is only ever our kill counter going up: someone dying near a hit of ours may have been killed by
        // another player (a deathmatch is full of them)
        if (kills >= 0) {
            if (this->last_kills >= 0 && kills > this->last_kills && kills - this->last_kills <= MAX_HITS_AT_ONCE)
                for (int i = this->last_kills; i < kills; i++)
                    this->pending_kills.push_back(now);
            this->last_kills = kills;
        }

        // Hits: given out once the health of someone went down with them, or after a short wait without
        while (!this->pending_hits.empty()) {
            auto at = this->pending_hits.front().time;

            HitEvent event{};
            event.time = at;
            event.hits = std::clamp(this->pending_hits.front().count, 1, 16);
            int left = 1;

            for (auto& drop : this->drops) {
                if (drop.used || drop.time + MATCH_WINDOW < at || drop.time > at + MATCH_WINDOW)
                    continue;

                // The one hurt most, a shotgun spreads over several
                if (drop.damage > event.damage) {
                    event.victim = drop.victim;
                    event.damage = drop.damage;
                    left = drop.left;
                }
            }

            if (event.victim < 0 && now - at < MATCH_WINDOW)
                break;

            // Nobody's health went down with it: not a hit of ours on a player (a counter catching up after
            // loading), nothing to show or play
            if (event.victim < 0) {
                LOGF(VERBOSE, "Hit counter +{} without anyone's health going down, no sound", event.hits);
                this->pending_hits.pop_front();
                continue;
            }

            // Someone died with it: wait a little for our kill counter, it may come a moment after the health
            if (left <= 0 && this->pending_kills.empty() && now - at < KILL_WAIT)
                break;

            for (auto& drop : this->drops)
                if (!drop.used && drop.victim == event.victim && drop.time + MATCH_WINDOW >= at && drop.time <= at + MATCH_WINDOW)
                    drop.used = true;

            if (!this->pending_kills.empty()) {
                event.kill = true;
                this->pending_kills.pop_front();
            }

            this->pending_hits.pop_front();
            Emit(event);
        }

        // Kills without a hit (a knife, a grenade), once no hit came for them
        while (!this->pending_kills.empty() && now - this->pending_kills.front() >= MATCH_WINDOW) {
            HitEvent event{};
            event.time = this->pending_kills.front();
            event.kill = true;

            for (auto& drop : this->drops) {
                if (!drop.used && drop.left <= 0 && drop.time + MATCH_WINDOW >= event.time && drop.time <= event.time + MATCH_WINDOW) {
                    event.victim = drop.victim;
                    event.damage = drop.damage;
                    drop.used = true;
                    break;
                }
            }

            this->pending_kills.pop_front();
            Emit(event);
        }
    }
}
