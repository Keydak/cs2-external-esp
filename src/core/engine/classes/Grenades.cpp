#include "Grenades.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/engine/classes/GrenadePrediction.hpp"
#include "core/engine/world/MapCollision.hpp"

using namespace std::chrono;

namespace {
    // The entity list is split in chunks of 512 identities, grenades live well within the first ones
    constexpr int CHUNK_COUNT = 8;
    constexpr int CHUNK_SIZE = 512;
    constexpr size_t IDENTITY_SIZE = 0x70;

    constexpr int MAX_FIRES = 64;

    constexpr auto NAME_CACHE_LIFETIME = seconds(5);

    constexpr float SMOKE_DURATION = 18.f;
    constexpr float FIRE_DURATION = 7.f; // Used when the lifetime read from the game looks wrong
    constexpr float FIRE_HALF_WIDTH = 30.f; // Same, for the size of a single fire

    constexpr float LANDING_MAX_ERROR = 64.f; // Further from the prediction than this, something deflected it
    constexpr float LANDING_REST_DISTANCE = 32.f;

    constexpr float TRAIL_MIN_DISTANCE = 4.f;
    constexpr size_t TRAIL_MAX_POINTS = 512;

    // Class info of an identity -> its binding -> the name of the class
    constexpr size_t IDENTITY_CLASS_INFO = 0x08;
    constexpr size_t CLASS_INFO_BINDING = 0x08;
    constexpr size_t BINDING_NAME = 0x08;

    std::optional<GrenadeType> TypeFromClass(std::string_view name) {
        if (name == "C_SmokeGrenadeProjectile")  return GrenadeType::Smoke;
        if (name == "C_MolotovProjectile")       return GrenadeType::Molotov;
        if (name == "C_FlashbangProjectile")     return GrenadeType::Flash;
        if (name == "C_HEGrenadeProjectile")     return GrenadeType::HE;
        if (name == "C_DecoyProjectile")         return GrenadeType::Decoy;
        if (name == "C_Inferno")                 return GrenadeType::Fire;
        return std::nullopt;
    }

    std::optional<GrenadeType> TypeFromName(std::string_view name) {
        if (name == "smokegrenade_projectile")  return GrenadeType::Smoke;
        if (name == "molotov_projectile")       return GrenadeType::Molotov;
        if (name == "flashbang_projectile")     return GrenadeType::Flash;
        if (name == "hegrenade_projectile")     return GrenadeType::HE;
        if (name == "decoy_projectile")         return GrenadeType::Decoy;
        if (name == "inferno")                  return GrenadeType::Fire;
        return std::nullopt;
    }

    // Props & moving brushes, the static ones are in the map files already
    bool ObstacleClass(std::string_view name) {
        constexpr std::string_view classes[] = {
            "C_DynamicProp", "C_PhysicsProp", "C_PhysicsPropMultiplayer", "C_BreakableProp", "C_PropDoorRotating",
            "C_BaseDoor", "C_FuncMoveLinear", "C_FuncRotating", "C_FuncTrackTrain", "C_PhysBox",
        };
        return std::find(std::begin(classes), std::end(classes), name) != std::end(classes);
    }

    bool ObstacleName(std::string_view name) {
        constexpr std::string_view kinds[] = {
            "prop_", "func_door", "func_breakable", "func_physbox", "func_movelinear", "func_rotating", "func_tracktrain",
        };
        return std::any_of(std::begin(kinds), std::end(kinds), [&](std::string_view kind) { return name.starts_with(kind); })
            && !name.starts_with("prop_ragdoll");
    }

    float SecondsSince(steady_clock::time_point point, steady_clock::time_point now) {
        return duration<float>(now - point).count();
    }
}

bool Grenades::Update(uintptr_t entity_list) {
    auto p = Engine::GetProcess();
    list.clear();

    if (!p || !entity_list)
        return false;

    auto now = steady_clock::now();

    for (auto& [address, state] : tracked)
        state.seen = false;

    // Kinds are cached by addresses the game can reuse after a map change. Looked up again now & then, a few
    // hundred reads
    if (now - kinds_cleared > NAME_CACHE_LIFETIME) {
        kinds.clear();
        kinds_cleared = now;
    }

    buffer.resize(IDENTITY_SIZE * CHUNK_SIZE);

    std::vector<std::pair<uintptr_t, GrenadeType>> found;
    std::vector<MapCollision::Box> boxes;

    for (int chunk = 0; chunk < CHUNK_COUNT; chunk++) {
        auto chunk_address = p->read<uintptr_t>(entity_list + 0x10 + 0x8 * chunk);

        // A whole chunk in one read, way cheaper than one read per entity
        if (!chunk_address || !p->read_raw(chunk_address, buffer.data(), buffer.size()))
            continue;

        for (int i = 0; i < CHUNK_SIZE; i++) {
            auto identity = buffer.data() + IDENTITY_SIZE * i;

            auto entity = *reinterpret_cast<uintptr_t*>(identity);
            if (!entity)
                continue;

            auto class_info = *reinterpret_cast<uintptr_t*>(identity + IDENTITY_CLASS_INFO);
            auto name_address = *reinterpret_cast<uintptr_t*>(identity + offsets::grenade::m_designerName);
            if (!class_info && !name_address)
                continue;

            const auto& kind = GetKind(class_info, name_address);

            if (kind.grenade)
                found.emplace_back(entity, *kind.grenade);
            else if (kind.obstacle) {
                MapCollision::Box box;
                if (ReadObstacle(entity, box))
                    boxes.push_back(box);
            }
        }
    }

    // Before the grenades, a smoke is shaped around what is in its way right now
    MapCollision::SetBoxes(std::move(boxes));

    for (const auto& [entity, type] : found)
        ReadGrenade(entity, type, now);

    // Grenades that exploded or expired are gone from the entity list
    std::erase_if(tracked, [](const auto& entry) { return !entry.second.seen; });

    return true;
}

const Grenades::Kind& Grenades::GetKind(uintptr_t class_info, uintptr_t name_address) {
    auto key = class_info ? class_info : name_address;
    if (auto it = kinds.find(key); it != kinds.end())
        return it->second;

    auto p = Engine::GetProcess();
    Kind kind;

    char name[64]{};
    if (class_info) {
        if (auto binding = p->read<uintptr_t>(class_info + CLASS_INFO_BINDING))
            if (auto name_text = p->read<uintptr_t>(binding + BINDING_NAME))
                p->read_raw(name_text, name, sizeof(name) - 1);

        kind.grenade = TypeFromClass(name);
        kind.obstacle = !kind.grenade && ObstacleClass(name);
    }
    else {
        p->read_raw(name_address, name, sizeof(name) - 1);

        kind.grenade = TypeFromName(name);
        kind.obstacle = !kind.grenade && ObstacleName(name);
    }

    return kinds[key] = kind;
}

bool Grenades::ReadObstacle(uintptr_t entity, MapCollision::Box& box) {
    constexpr uint8_t SOLID_NONE = 0;
    constexpr uint8_t FSOLID_NOT_SOLID = 0x4, FSOLID_TRIGGER = 0x8;
    constexpr float MIN_SIZE = 12.f;    // Bottles & cans, nothing a smoke stops at
    constexpr float MAX_SIZE = 1024.f;  // Bounds that cover half the map, not a thing in the way

    auto p = Engine::GetProcess();

    auto collision = p->read<uintptr_t>(entity + offsets::grenade::m_pCollision);
    auto node = p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode);
    if (!collision || !node)
        return false;

    if (p->read<uint8_t>(collision + offsets::grenade::m_nSolidType) == SOLID_NONE)
        return false;
    if (p->read<uint8_t>(collision + offsets::grenade::m_usSolidFlags) & (FSOLID_NOT_SOLID | FSOLID_TRIGGER))
        return false;

    box.mins = p->read<Vec3_t>(collision + offsets::grenade::m_vecMins);
    box.maxs = p->read<Vec3_t>(collision + offsets::grenade::m_vecMaxs);

    auto size = box.maxs - box.mins;
    float largest = std::max({ size.x, size.y, size.z });
    if (size.x <= 0.f || size.y <= 0.f || size.z <= 0.f || largest < MIN_SIZE || largest > MAX_SIZE)
        return false;

    box.origin = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin);
    box.angles = p->read<Vec3_t>(node + offsets::grenade::m_angAbsRotation);
    return !box.origin.zero();
}

void Grenades::ReadGrenade(uintptr_t entity, GrenadeType type, steady_clock::time_point now) {
    auto p = Engine::GetProcess();

    auto [it, inserted] = tracked.try_emplace(entity);
    auto& state = it->second;

    if (inserted)
        state.first_seen = now;
    state.seen = true;

    // TEMP: compare real throws with the last prediction, for calibration
    if (inserted && type != GrenadeType::Fire) {
        auto start = p->read<Vec3_t>(entity + offsets::grenade::m_vInitialPosition);
        auto velocity = p->read<Vec3_t>(entity + offsets::grenade::m_vInitialVelocity);
        auto& ps = GrenadePrediction::last_start;
        auto& pv = GrenadePrediction::last_velocity;

        LOGF(INFO, "[nade] actual start=({:.1f} {:.1f} {:.1f}) vel=({:.1f} {:.1f} {:.1f}) | predicted start=({:.1f} {:.1f} {:.1f}) vel=({:.1f} {:.1f} {:.1f})",
            start.x, start.y, start.z, velocity.x, velocity.y, velocity.z,
            ps.x, ps.y, ps.z, pv.x, pv.y, pv.z);
    }

    Grenade grenade;
    grenade.type = type;
    grenade.team = p->read<uint8_t>(entity + offsets::pawn::m_iTeamNum);

    if (auto node = p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode))
        grenade.pos = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin);

    switch (type) {
    case GrenadeType::Smoke: {
        if (!p->read<bool>(entity + offsets::grenade::m_bDidSmokeEffect))
            break;

        grenade.pos = p->read<Vec3_t>(entity + offsets::grenade::m_vSmokeDetonationPos);

        if (!state.detonated) {
            state.detonated = true;
            state.detonated_at = now;

            // TEMP: how far the landing prediction was from the real smoke, for calibration
            if (state.landing.valid) {
                LOGF(INFO, "[nade] smoke landed {:.1f}u from its prediction (predicted {:.2f}s, took {:.2f}s)",
                    state.landing.end.dist_to_3d(grenade.pos), state.landing.time, SecondsSince(state.first_seen, now));
            }
        }

        // Shaped once against the map, a plain disc until its collision is loaded
        if (!state.area_final) {
            state.area = GrenadeArea::Smoke(grenade.pos);
            state.area_final = MapCollision::IsLoaded();
        }

        grenade.area = state.area;
        grenade.detonated = true;
        grenade.duration = SMOKE_DURATION;
        grenade.time_left = std::max(0.f, SMOKE_DURATION - SecondsSince(state.detonated_at, now));
        break;
    }
    case GrenadeType::Fire: {
        float lifetime = p->read<float>(entity + offsets::grenade::m_nFireLifetime);
        if (!(lifetime > 1.f && lifetime < 30.f))
            lifetime = FIRE_DURATION;

        // The inferno stays in the entity list for a while after it went out, nothing to show then
        float time_left = lifetime - SecondsSince(state.first_seen, now);
        if (time_left <= 0.f)
            return;

        int count = std::clamp(p->read<int>(entity + offsets::grenade::m_fireCount), 0, MAX_FIRES);

        float positions[MAX_FIRES][3]{};

        if (count > 0)
            p->read_raw(entity + offsets::grenade::m_firePositions, positions, sizeof(positions[0]) * count);

        // Every fire counts: the burning flags are not reliable anymore (all false while it burns), the fires
        // go out together when the inferno is gone
        Vec3_t center{};
        for (int i = 0; i < count; i++) {
            Vec3_t fire(positions[i][0], positions[i][1], positions[i][2]);
            if (fire.zero())
                continue;

            grenade.fires.push_back(fire);
            center += fire;
        }

        if (!grenade.fires.empty())
            grenade.pos = center / static_cast<float>(grenade.fires.size());

        // Shaped again only when it spread to more fires
        if (static_cast<int>(grenade.fires.size()) != state.fire_count) {
            float half_width = p->read<float>(entity + offsets::grenade::m_maxFireHalfWidth);
            if (!(half_width > 10.f && half_width < 80.f))
                half_width = FIRE_HALF_WIDTH;

            state.area = GrenadeArea::Fires(grenade.fires, half_width);
            state.fire_count = static_cast<int>(grenade.fires.size());
        }

        grenade.area = state.area;
        grenade.detonated = true;
        grenade.duration = lifetime;
        grenade.time_left = time_left;
        break;
    }
    default:
        // HE & flash stay around for a moment after they went off, there is nothing left to show
        if (p->read<bool>(entity + offsets::grenade::m_bExplodeEffectBegan) ||
            p->read<int>(entity + offsets::grenade::m_nExplodeEffectTickBegin) > 0)
            return;
        break;
    }

    // Where it is going to land, simulated once from the real throw. Retried while the map collision loads
    if (cfg::esp::grenades::landing && type != GrenadeType::Fire && !grenade.detonated && !state.landing_simulated) {
        auto start = p->read<Vec3_t>(entity + offsets::grenade::m_vInitialPosition);
        auto velocity = p->read<Vec3_t>(entity + offsets::grenade::m_vInitialVelocity);

        if (!start.zero() && !velocity.zero())
            state.landing_simulated = GrenadePrediction::Simulate(type, start, velocity, state.landing);

        if (state.landing_simulated && state.landing.on_ground) {
            if (type == GrenadeType::Smoke)
                state.landing.area = GrenadeArea::Smoke(state.landing.end);
            else if (type == GrenadeType::Molotov)
                state.landing.area = GrenadeArea::FireSpread(state.landing.end);
        }
    }

    if (!grenade.detonated && state.landing.valid && !state.landing_over)
        UpdateLanding(grenade, state, now);

    // Flight path, recorded only while the grenade is still in the air
    if (!grenade.detonated && !grenade.pos.zero()) {
        bool moved = state.trail.empty() || state.trail.back().dist_to_3d(grenade.pos) > TRAIL_MIN_DISTANCE;

        if (moved && state.trail.size() < TRAIL_MAX_POINTS)
            state.trail.push_back(grenade.pos);
    }

    state.last_pos = grenade.pos;

    grenade.trail = state.trail;
    list.push_back(std::move(grenade));
}

void Grenades::UpdateLanding(Grenade& grenade, Tracked& state, steady_clock::time_point now) {
    const auto& path = state.landing;
    const auto& points = path.points;

    if (points.size() < 2 || grenade.pos.zero())
        return;

    // Where the grenade is along the prediction, it only moves forward
    size_t closest = state.landing_from;
    float closest_distance = FLT_MAX;

    for (size_t i = state.landing_from; i < points.size(); i++) {
        float distance = points[i].dist_to_3d(grenade.pos);
        if (distance < closest_distance) {
            closest_distance = distance;
            closest = i;
        }
    }

    // Time left counted from when it was thrown, it may have been in the air for a while when first seen
    float tick_time = path.time / static_cast<float>(points.size() - 1);
    if (state.landing_offset < 0.f)
        state.landing_offset = closest * tick_time - SecondsSince(state.first_seen, now);

    float time_left = path.time - (state.landing_offset + SecondsSince(state.first_seen, now));

    // Gone as soon as it is over: resting where predicted, out of time or knocked off the path
    bool resting = grenade.pos.dist_to_3d(state.last_pos) < 0.5f && grenade.pos.dist_to_3d(path.end) < LANDING_REST_DISTANCE;
    bool out_of_time = path.time > 0.f && time_left <= 0.f;

    if (resting || out_of_time || closest_distance > LANDING_MAX_ERROR) {
        state.landing_over = true;
        return;
    }

    state.landing_from = closest;

    grenade.landing = path;
    grenade.landing_from = closest;
    grenade.landing_time_left = std::max(0.f, time_left);
}
