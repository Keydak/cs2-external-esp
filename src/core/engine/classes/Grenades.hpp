#pragma once
#include <optional>
#include <unordered_map>

#include "core/engine/world/GrenadeArea.hpp"
#include "core/engine/world/MapCollision.hpp"

enum class GrenadeType {
    Smoke,
    Molotov,
    Flash,
    HE,
    Decoy,
    Fire // Burning molotov/incendiary (inferno)
};

struct GrenadePath {
    bool valid = false;
    GrenadeType type = GrenadeType::HE;
    std::vector<Vec3_t> points;   // Position on every simulated tick
    std::vector<Vec3_t> bounces;  // Where the grenade hit the world
    Vec3_t end{};                 // Detonation / resting point
    bool on_ground = false;       // Ended on a floor rather than mid air
    float time = 0.f;             // Seconds until detonation
    AreaShape area;               // Smoke / fire it leaves where it ends
};

struct Grenade {
    GrenadeType type = GrenadeType::HE;
    uintptr_t entity = 0;
    Vec3_t pos{};
    int team = 0;
    bool detonated = false;     // Smoke popped or fire burning, no more flight
    float time_left = -1.f;     // Seconds, -1 when it does not apply
    float duration = 0.f;
    std::vector<Vec3_t> trail;  // Flight path
    std::vector<Vec3_t> fires;  // Burning fire positions (Fire only)
    AreaShape area;             // Popped smoke or burning ground

    GrenadePath landing;            // Predicted flight, simulated from where it was thrown
    float landing_time_left = -1.f; // Seconds until it lands / detonates
    size_t landing_from = 0;        // Point of the prediction the grenade is at
};

// Scans the entity list for grenades, keeping some state across scans (trails & timers)
class Grenades {
public:
    bool Update(uintptr_t entity_list);

public:
    std::vector<Grenade> list;

private:
    struct Tracked {
        std::vector<Vec3_t> trail;
        std::chrono::steady_clock::time_point first_seen{};
        std::chrono::steady_clock::time_point detonated_at{};
        bool detonated = false;
        bool seen = false;

        Vec3_t last_pos{};

        AreaShape area;
        bool area_final = false;    // Shaped against the map collision
        int fire_count = -1;        // Fires the area of an inferno was shaped with

        GrenadePath landing;
        bool landing_simulated = false;
        bool landing_over = false;  // Landed, detonated or went off the predicted path
        size_t landing_from = 0;
        float landing_offset = -1.f; // Seconds the grenade had already flown when first seen

        // A decoy lies where it landed until it goes off: its flight is over once it stays still
        std::chrono::steady_clock::time_point still_since{};
        bool rested = false;
    };

    // What an entity is to us: a grenade, or a solid thing that moves & is not in the map files (doors, cars,
    // crates, breakables...) that smokes & traces stop at
    struct Kind {
        std::optional<GrenadeType> grenade;
        bool obstacle = false;
    };

    // From the class of the entity, some have no designer name (C_Inferno). The designer name without a class
    const Kind& GetKind(uintptr_t class_info, uintptr_t name_address);

    void ReadGrenade(uintptr_t entity, GrenadeType type, std::chrono::steady_clock::time_point now);
    static bool ReadObstacle(uintptr_t entity, MapCollision::Box& box);
    void UpdateLanding(Grenade& grenade, Tracked& state, std::chrono::steady_clock::time_point now);

private:
    std::unordered_map<uintptr_t, Tracked> tracked;

    // By the address of the class info, or of the designer name (interned). Both can be reused by something
    // else after a map change, so looked up again now & then
    std::unordered_map<uintptr_t, Kind> kinds;
    std::chrono::steady_clock::time_point kinds_cleared{};

    std::vector<uint8_t> buffer;
};
