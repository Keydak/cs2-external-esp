#include "GrenadePrediction.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/engine/world/MapCollision.hpp"
#include "core/engine/types/Weapons.hpp"

#include <numbers>

// Mirrors the grenade physics of the game (CBaseCSGrenadeProjectile), see the constants below
namespace {
    constexpr float TICK_INTERVAL = 1.f / 64.f;
    constexpr int MAX_TICKS = 1024;

    constexpr float THROW_SPEED = 750.f * 0.9f;
    constexpr float PLAYER_VELOCITY_SCALE = 1.25f;

    constexpr float GRAVITY = 800.f * 0.4f; // sv_gravity * grenade gravity scale
    constexpr float ELASTICITY = 0.45f;
    constexpr float MIN_BOUNCE_SPEED = 20.f;
    constexpr float FLOOR_NORMAL_Z = 0.7f;
    constexpr float STOP_EPSILON = 0.1f;
    constexpr float SURFACE_OFFSET = 0.1f; // Keeps the next trace from starting inside the surface
    constexpr float MARK_BOUNCE_SPEED = 50.f; // Slower contacts are the grenade rolling or resting

    constexpr float FUSE_TIME = 1.5f;           // HE & flash
    constexpr float MOLOTOV_FUSE_TIME = 2.f;    // Molotov & incendiary explode in the air after this
    constexpr float DETONATE_CHECK = 0.2f;      // The game only checks for detonation this often

    constexpr float EYE_HEIGHT = 64.06f;
    constexpr float EYE_HEIGHT_DUCKED = 46.04f;
    constexpr uint32_t FL_DUCKING = (1 << 1);

    constexpr int DETONATE_CHECK_TICKS = static_cast<int>(DETONATE_CHECK / TICK_INTERVAL);

    std::optional<GrenadeType> GrenadeFromItem(int index) {
        switch (index) {
        case weapon_flashbang:      return GrenadeType::Flash;
        case weapon_hegrenade:      return GrenadeType::HE;
        case weapon_smokegrenade:   return GrenadeType::Smoke;
        case weapon_molotov:        return GrenadeType::Molotov;
        case weapon_incgrenade:     return GrenadeType::Molotov;
        case weapon_decoy:          return GrenadeType::Decoy;
        default:                    return std::nullopt;
        }
    }

    float Dot(const Vec3_t& a, const Vec3_t& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3_t Forward(float pitch, float yaw) {
        constexpr float to_radians = std::numbers::pi_v<float> / 180.f;
        float sp = sinf(pitch * to_radians), cp = cosf(pitch * to_radians);
        float sy = sinf(yaw * to_radians), cy = cosf(yaw * to_radians);
        return Vec3_t(cp * cy, cp * sy, -sp);
    }

    struct Move {
        Vec3_t end;
        bool hit = false;
        MapCollision::Hit info;
    };

    // Moves as far as the world allows
    Move PushEntity(const Vec3_t& start, const Vec3_t& delta) {
        Move move;
        move.hit = MapCollision::Trace(start, start + delta, move.info);
        move.end = move.hit
            ? start + delta * move.info.fraction + move.info.normal * SURFACE_OFFSET
            : start + delta;
        return move;
    }

    Vec3_t ClipVelocity(const Vec3_t& velocity, const Vec3_t& normal, float overbounce) {
        float backoff = Dot(velocity, normal) * overbounce;
        Vec3_t out = velocity - normal * backoff;

        for (int axis = 0; axis < 3; axis++)
            if (out[axis] > -STOP_EPSILON && out[axis] < STOP_EPSILON)
                out[axis] = 0.f;

        return out;
    }

    bool ShouldDetonate(GrenadeType type, const Vec3_t& velocity, const Move& move, int tick) {
        float time = tick * TICK_INTERVAL;
        bool check_tick = tick % DETONATE_CHECK_TICKS == 0;

        switch (type) {
        case GrenadeType::Smoke:
        case GrenadeType::Decoy:
            return velocity.length_2d() < 0.1f && check_tick;
        case GrenadeType::Molotov:
            if (move.hit && move.info.normal.z > FLOOR_NORMAL_Z)
                return true;
            return time > MOLOTOV_FUSE_TIME && check_tick;
        default:
            return time > FUSE_TIME && check_tick;
        }
    }
}

bool GrenadePrediction::Predict(uintptr_t pawn, GrenadePath& path) {
    auto p = Engine::GetProcess();
    path = {};

    if (!p || !pawn)
        return false;

    // Grenade in hand
    auto weapon_services = p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices);
    if (!weapon_services)
        return false;

    auto weapon = Engine::GetEntityFromHandle(p->read<uint32_t>(weapon_services + offsets::pawn::m_hActiveWeapon));
    if (!weapon)
        return false;

    auto index = p->read<uint16_t>(weapon + offsets::pawn::m_AttributeManager + offsets::pawn::m_Item + offsets::pawn::m_iItemDefinitionIndex);
    auto type = GrenadeFromItem(index);
    if (!type)
        return false;

    // 1 for a left click throw, 0.5 for both buttons and 0 for a right click lob
    float strength = 1.f;
    if (p->read<bool>(weapon + offsets::grenade::m_bPinPulled))
        strength = std::clamp(p->read<float>(weapon + offsets::grenade::m_flThrowStrength), 0.f, 1.f);

    // Eye position & view
    auto node = p->read<uintptr_t>(pawn + offsets::pawn::m_pGameSceneNode);
    if (!node)
        return false;

    auto origin = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin);
    auto flags = p->read<uint32_t>(pawn + offsets::pawn::m_fFlags);
    auto eye = origin + Vec3_t(0.f, 0.f, (flags & FL_DUCKING) ? EYE_HEIGHT_DUCKED : EYE_HEIGHT);

    auto angles = p->read<Vec3_t>(pawn + offsets::grenade::m_angEyeAngles);
    auto player_velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);

    // Throws are aimed a bit higher than the crosshair
    float pitch = angles.x;
    if (pitch > 90.f)
        pitch -= 360.f;
    else if (pitch < -90.f)
        pitch += 360.f;
    pitch -= (90.f - std::fabs(pitch)) * 10.f / 90.f;

    auto forward = Forward(pitch, angles.y);

    // Start in front of the eyes, pulled back when that is inside a wall
    auto start = eye + Vec3_t(0.f, 0.f, strength * 12.f - 12.f);
    auto spawn = PushEntity(start, forward * 22.f);
    auto position = spawn.end - forward * 6.f;

    auto velocity = forward * (THROW_SPEED * (strength * 0.7f + 0.3f)) + player_velocity * PLAYER_VELOCITY_SCALE;

    return Simulate(*type, position, velocity, path);
}

bool GrenadePrediction::Simulate(GrenadeType type, Vec3_t position, Vec3_t velocity, GrenadePath& path) {
    path = {};

    if (!MapCollision::IsLoaded())
        return false;

    path.type = type;
    path.points.push_back(position);

    for (int tick = 0; tick < MAX_TICKS; tick++) {
        // Gravity, integrated over the tick
        float new_z = velocity.z - GRAVITY * TICK_INTERVAL;
        Vec3_t delta(velocity.x * TICK_INTERVAL, velocity.y * TICK_INTERVAL, (velocity.z + new_z) * 0.5f * TICK_INTERVAL);
        velocity.z = new_z;

        auto move = PushEntity(position, delta);
        bool detonate = ShouldDetonate(type, velocity, move, tick);

        path.on_ground = move.hit && move.info.normal.z > FLOOR_NORMAL_Z;

        if (move.hit) {
            // Resting on the floor also "hits" it every tick, only real bounces are marked
            if (velocity.length() > MARK_BOUNCE_SPEED)
                path.bounces.push_back(move.end);

            auto bounced = ClipVelocity(velocity, move.info.normal, 2.f) * ELASTICITY;
            if (bounced.length_sqr() < MIN_BOUNCE_SPEED * MIN_BOUNCE_SPEED)
                bounced = Vec3_t(0.f, 0.f, 0.f);

            velocity = bounced;

            // On floors the rest of the tick is spent sliding along it
            if (move.info.normal.z > FLOOR_NORMAL_Z)
                move = PushEntity(move.end, bounced * ((1.f - move.info.fraction) * TICK_INTERVAL));
        }

        position = move.end;
        path.points.push_back(position);

        if (detonate) {
            path.time = (tick + 1) * TICK_INTERVAL;
            break;
        }
    }

    path.end = position;
    path.valid = true;

    return true;
}
