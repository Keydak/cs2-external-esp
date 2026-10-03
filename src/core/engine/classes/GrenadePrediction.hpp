#pragma once
#include "core/engine/classes/Grenades.hpp"

// Simulates the throw of the grenade the local player is holding against the map collision
class GrenadePrediction {
public:
    // Throw of the grenade the local player is holding
    static bool Predict(uintptr_t pawn, GrenadePath& path);

    // Flight of a grenade from its spawn position & velocity, ticks count from the throw
    static bool Simulate(GrenadeType type, Vec3_t position, Vec3_t velocity, GrenadePath& path);

    // TEMP: last simulated throw, compared with real throws for calibration
    static inline Vec3_t last_start{};
    static inline Vec3_t last_velocity{};
};
