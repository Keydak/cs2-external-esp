#pragma once
#include "core/memory/Memory.hpp"

#include <deque>
#include <unordered_map>

// One of our hits, or a kill
struct HitEvent {
    int victim = -1;        // Index of the player whose health went down with it, -1 when none was seen
    int damage = 0;         // Health they lost, 0 when unknown
    bool kill = false;
    int hits = 1;           // Hits that came at once (the pellets of a shotgun), a sound for each
    std::chrono::steady_clock::time_point time;
};

// Our hits & kills, for the hitmarker & the hit & kill sounds. Seen from what the game keeps of us: the hit counter the
// server sends in the bullet services of our pawn & our kills of the round in the controller, the player hit from their
// health going down at the same time. Memory reads only, no -insecure needed
class HitEffects {
public:
    ~HitEffects()                            = default;
    HitEffects(const HitEffects&)            = delete;
    HitEffects(HitEffects&&)                 = delete;
    HitEffects& operator=(const HitEffects&) = delete;
    HitEffects& operator=(HitEffects&&)      = delete;

    static bool Init();
    static void Shutdown();

    // The events since the last call, oldest first
    static std::vector<HitEvent> Drain();
    // Only the kills since the last call, a list of their own (KillEffect)
    static std::vector<HitEvent> DrainKills();
private:
    HitEffects() {};

    static HitEffects& GetInstance()
    {
        static HitEffects i{};
        return i;
    }

    void Thread();
    void Reset();
    void Emit(const HitEvent& event);

    struct Health {
        uintptr_t pawn = 0;
        int value = 0;
    };

    struct Drop {
        int victim;
        int damage;
        int left;           // Health after it
        std::chrono::steady_clock::time_point time;
        bool used = false;
    };

    std::unordered_map<int, Health> health;
    std::deque<Drop> drops;
    struct PendingHit {
        std::chrono::steady_clock::time_point time;
        int count = 1;
    };
    std::deque<PendingHit> pending_hits;
    std::deque<std::chrono::steady_clock::time_point> pending_kills;

    uintptr_t last_pawn = 0;
    uintptr_t last_controller = 0;
    uintptr_t last_bullets = 0;
    int last_hits = -1;
    int last_kills = -1;

    std::mutex mutex;
    std::vector<HitEvent> events;
    std::vector<HitEvent> kills;

    std::atomic<bool> stopping = false;
    std::atomic<bool> running = false;
};
