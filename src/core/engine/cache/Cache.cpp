#include "Cache.hpp"

#include "core/engine/Engine.hpp" // Circular dep
#include "core/offsets/Dumper.hpp"
#include "core/engine/world/MapCollision.hpp"

namespace {
    constexpr float SMOKE_BLOCK_RADIUS = 130.f;	// A bit inside the smoke, its edge can be seen through
    constexpr float SMOKE_BLOCK_HEIGHT = 60.f;	// The cloud sits above where the grenade lies

    // Does the segment pass through the sphere
    bool SegmentHitsSphere(const Vec3_t& start, const Vec3_t& end, const Vec3_t& center, float radius) {
        auto direction = end - start;
        auto to_center = center - start;

        float length_sq = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
        if (length_sq <= 0.f)
            return false;

        float t = std::clamp((to_center.x * direction.x + to_center.y * direction.y + to_center.z * direction.z) / length_sq, 0.f, 1.f);
        Vec3_t closest(start.x + direction.x * t, start.y + direction.y * t, start.z + direction.z * t);
        auto offset = closest - center;

        return offset.x * offset.x + offset.y * offset.y + offset.z * offset.z < radius * radius;
    }

    // Visible when any part of the body can be seen from our eyes, through neither the map nor a smoke
    bool IsVisible(const Player& local, const Player& player, const std::vector<Grenade>& grenades) {
        // Nothing to trace against, everything counts as visible
        if (!local.alive || !MapCollision::IsLoaded())
            return true;

        if (player.bone_list.size() <= bone_index::foot_heel_R)
            return true;

        // Any of these seen is enough, so a player showing only a leg or an arm counts as visible
        const DWORD points[] = {
            bone_index::head, bone_index::spine_2, bone_index::pelvis,
            bone_index::hand_L, bone_index::hand_R, bone_index::elbow_L, bone_index::elbow_R,
            bone_index::knee_L, bone_index::knee_R, bone_index::foot_heel_L, bone_index::foot_heel_R,
        };

        for (auto bone : points) {
            const auto& target = player.bone_list[bone].pos;

            if (MapCollision::Blocked(local.eye, target))
                continue;

            bool smoked = false;
            for (const auto& grenade : grenades) {
                if (grenade.type != GrenadeType::Smoke || !grenade.detonated)
                    continue;

                auto center = grenade.pos + Vec3_t(0.f, 0.f, SMOKE_BLOCK_HEIGHT);
                if (SegmentHitsSphere(local.eye, target, center, SMOKE_BLOCK_RADIUS)) {
                    smoked = true;
                    break;
                }
            }

            if (!smoked)
                return true;
        }

        return false;
    }
}

bool Cache::Refresh() {
    return Get().RefreshImpl();
}

Snapshot Cache::CopySnapshot() {
    std::lock_guard<std::mutex> lock(Get().mtx);
    return {
        Get().game,
        Get().bomb,
        Get().local,
        Get().globals,
        Get().players,
        Get().grenades,
        Get().items,
        Get().grenade_path
    };
}

bool Cache::RefreshImpl() {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (!p)
        return false;

    auto now = steady_clock::now();

    // Without this, we are pointless :c
    // Its just calling game.UpdateMatrix() which has to bee updated as fast as possible
    if (!game.Update())
        return false;

#ifdef _DEBUG
    // Testing performance
    if (now - last < (cfg::dev::cache_refresh_rate * 1ms)) 
        return true;
#else
    // Just refresh every 5ms good for most people
    if (now - last < 5ms) 
        return true; // All good
#endif

    game.UpdateEntityList();
    game.UpdateMode();
    globals.Update();
    bomb.Update();

    std::vector<Player> scan;
    scan.reserve(globals.max_clients);
    for (int i = 0; i < globals.max_clients; i++) {
        auto player = Player(i, game.entity_list, game.list_entry);

        if (!player.Update())
            continue;

        if (player.localplayer)
            this->local = player;

        player.has_c4 = (bomb.carrier != 0 && (uintptr_t)player.pawn_controller_addr == bomb.carrier);

        // TODO: Handle or at least alert, in case of multiple lp
        //if (player.localplayer && (this->local.index == -1 || this->local.index == player.index))
        //    this->local = player;
        //else if (player.localplayer)
        //    LOGF(FATAL, "Offset missmatch, initial({}) current({}) there are more than one local players, update needed", this->local.index, player.index);
    
        scan.push_back(player);
    }

    // Line of sight to everyone, traced here so the overlay does not wait on it. Not every refresh, the result
    // is kept for the ones in between
    if (now - last_visibility >= visibility_rate) {
        visibility.clear();
        for (const auto& player : scan)
            if (player.alive && !player.localplayer)
                visibility[player.index] = IsVisible(this->local, player, grenades);
        last_visibility = now;
    }

    for (auto& player : scan)
        if (auto it = visibility.find(player.index); it != visibility.end())
            player.visible = it->second;

    // Game time, the newest pawn update
    float game_time = 0.f;
    for (const auto& player : scan)
        game_time = std::max(game_time, player.simulation_time);
    bomb.SetGameTime(game_time);

    // Collision follows the current map, loaded in the background. Always, players are traced against it
    MapCollision::SetMap(globals.in_match ? std::string(globals.map_name, strnlen(globals.map_name, sizeof(globals.map_name))) : "");

    GrenadePath path;
    if (cfg::esp::grenades::enabled && cfg::esp::grenades::prediction && this->local.alive)
        GrenadePrediction::Predict(Engine::GetLocalPawn(), path);

    ShapePreviewArea(path);

    bool grenades_scanned = false;
    if (cfg::esp::grenades::enabled && now - last_grenade_scan >= grenade_rate) {
        grenade_tracker.Update(game.entity_list);
        last_grenade_scan = now;
        grenades_scanned = true;
    }

    // New & picked up items now & then, where they are every refresh so dropped ones fall smoothly
    bool want_items = cfg::esp::items::enabled;
    if (want_items) {
        if (now - last_item_scan >= item_rate) {
            item_tracker.Update(game.entity_list);
            last_item_scan = now;
        }
        else
            item_tracker.UpdatePositions();
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        players = std::move(scan);

        if (want_items)
            items = item_tracker.list;
        else if (!want_items)
            items.clear();
        grenade_path = std::move(path);

        if (grenades_scanned)
            grenades = grenade_tracker.list;
        else if (!cfg::esp::grenades::enabled)
            grenades.clear();

        duration = duration_cast<std::chrono::milliseconds>(last - now);
        last = now;
    }

    return true;
}

void Cache::ShapePreviewArea(GrenadePath& path) {
    if (!path.valid || !path.on_ground || (path.type != GrenadeType::Smoke && path.type != GrenadeType::Molotov)) {
        preview_area = {};
        preview_valid = false;
        return;
    }

    constexpr float min_move = 4.f;

    // Take what the shaping thread finished
    {
        std::lock_guard<std::mutex> lock(shape_mtx);
        if (shaped_ready) {
            shaped_ready = false;
            preview_area = std::move(shaped_area);
            preview_end = shaped_end;
            preview_type = shaped_type;
            preview_valid = true;
        }
    }

    bool outdated = !preview_valid || path.type != preview_type || path.end.dist_to_3d(preview_end) > min_move;

    // One shape at a time, the next one starts from where the landing spot is by then
    if (outdated && !shaping.exchange(true)) {
        std::thread([this, end = path.end, type = path.type]() {
            auto area = type == GrenadeType::Smoke ? GrenadeArea::Smoke(end) : GrenadeArea::FireSpread(end);

            {
                std::lock_guard<std::mutex> lock(shape_mtx);
                shaped_area = std::move(area);
                shaped_end = end;
                shaped_type = type;
                shaped_ready = true;
            }
            shaping = false;
        }).detach();
    }

    // Only a shape of this grenade, a smoke is not drawn as a fire while the new one is made
    if (preview_valid && preview_type == path.type)
        path.area = preview_area;
}
