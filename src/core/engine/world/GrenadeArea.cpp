#include "GrenadeArea.hpp"
#include "MapCollision.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace {
    // Smoke, roughly the size of the in game one on open ground
    constexpr float SMOKE_RADIUS = 144.f;
    constexpr float SMOKE_HEIGHT = 128.f;       // Above where it popped
    constexpr float SMOKE_DEPTH = 64.f;         // Below, spilling down ledges & stairs
    constexpr float SMOKE_MAX_STRETCH = 2.5f;   // How far it flows when walls take away part of its volume

    // Molotov
    constexpr float FIRE_CELL = 16.f;
    constexpr float FIRE_RADIUS = 120.f;
    constexpr float FIRE_MAX_STRETCH = 1.5f;
    constexpr float FIRE_STEP = 24.f;           // Highest step the fire climbs
    constexpr float FIRE_DROP = 64.f;           // Deepest drop it follows
    constexpr float FIRE_GRID = 12.f;           // Burning fires
    constexpr float FIRE_MIN_COVER = 40.f;      // Around each burning fire, at least

    constexpr float FLOOR_NORMAL_Z = 0.7f;

    int64_t Key(int x, int y, int z = 0) {
        constexpr int64_t bias = 1 << 20;
        return ((x + bias) << 42) | ((y + bias) << 21) | (z + bias);
    }

    // Grid cells are filled closest first, "stretch" being the distance scaled to the open ground size
    struct Entry {
        float stretch;
        int x, y, z;
        float floor;

        bool operator>(const Entry& other) const { return stretch > other.stretch; }
    };

    using Frontier = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;
}

AreaShape GrenadeArea::Smoke(const Vec3_t& origin, float voxel_size) {
    if (!MapCollision::IsLoaded())
        return Disc(origin, SMOKE_RADIUS, SMOKE_HEIGHT, voxel_size);

    auto stretch = [&](int x, int y, int z) {
        float horizontal = (x * x + y * y) * voxel_size * voxel_size / (SMOKE_RADIUS * SMOKE_RADIUS);
        float vertical = z * voxel_size / (z >= 0 ? SMOKE_HEIGHT : SMOKE_DEPTH);
        return std::sqrt(horizontal + vertical * vertical);
    };

    // Voxels the smoke fills on open ground, it keeps that volume when walls are in the way
    size_t budget = 0;
    {
        int reach = static_cast<int>(std::ceil(SMOKE_RADIUS / voxel_size));
        int up = static_cast<int>(std::ceil(SMOKE_HEIGHT / voxel_size));
        int down = static_cast<int>(std::ceil(SMOKE_DEPTH / voxel_size));

        for (int z = -down; z <= up; z++)
            for (int y = -reach; y <= reach; y++)
                for (int x = -reach; x <= reach; x++)
                    budget += stretch(x, y, z) <= 1.f;
    }

    MapCollision::Hit hit;

    auto base = origin + Vec3_t(0.f, 0.f, voxel_size * 0.5f);
    if (MapCollision::Trace(origin + Vec3_t(0.f, 0.f, 1.f), base, hit))
        base = origin + Vec3_t(0.f, 0.f, 1.f);

    auto center = [&](int x, int y, int z) {
        return base + Vec3_t(x * voxel_size, y * voxel_size, z * voxel_size);
    };

    constexpr int directions[6][3] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };

    Frontier frontier;
    std::unordered_set<int64_t> queued;
    std::vector<Entry> filled;

    frontier.push({ 0.f, 0, 0, 0, 0.f });
    queued.insert(Key(0, 0, 0));

    // Flood fill, only through openings the world leaves
    while (!frontier.empty() && filled.size() < budget) {
        auto voxel = frontier.top();
        frontier.pop();

        if (voxel.stretch > SMOKE_MAX_STRETCH)
            break;

        filled.push_back(voxel);
        auto from = center(voxel.x, voxel.y, voxel.z);

        for (const auto& direction : directions) {
            int x = voxel.x + direction[0], y = voxel.y + direction[1], z = voxel.z + direction[2];
            auto key = Key(x, y, z);

            if (queued.contains(key) || MapCollision::Trace(from, center(x, y, z), hit))
                continue;

            queued.insert(key);
            frontier.push({ stretch(x, y, z), x, y, z, 0.f });
        }
    }

    // Columns, from the lowest to the highest voxel
    std::unordered_map<int64_t, size_t> columns;
    std::vector<std::pair<int, int>> heights;

    AreaShape shape;
    shape.cell_size = voxel_size;
    shape.origin_x = base.x;
    shape.origin_y = base.y;

    for (const auto& voxel : filled) {
        auto [it, inserted] = columns.try_emplace(Key(voxel.x, voxel.y), shape.cells.size());

        if (inserted) {
            shape.cells.push_back({ voxel.x, voxel.y, 0.f, 0.f });
            heights.emplace_back(voxel.z, voxel.z);
        }
        else {
            auto& [low, high] = heights[it->second];
            low = std::min(low, voxel.z);
            high = std::max(high, voxel.z);
        }
    }

    for (size_t i = 0; i < shape.cells.size(); i++) {
        auto& cell = shape.cells[i];
        auto bottom = center(cell.x, cell.y, heights[i].first);

        // Down onto the ground under the column
        cell.floor = bottom.z - voxel_size * 0.5f;
        if (MapCollision::Trace(bottom, bottom - Vec3_t(0.f, 0.f, voxel_size), hit))
            cell.floor = bottom.z - voxel_size * hit.fraction;

        cell.top = center(cell.x, cell.y, heights[i].second).z + voxel_size * 0.5f;
    }

    return shape;
}

AreaShape GrenadeArea::FireSpread(const Vec3_t& origin) {
    if (!MapCollision::IsLoaded())
        return Disc(origin, FIRE_RADIUS, 0.f, FIRE_CELL);

    MapCollision::Hit hit;

    // Fire only starts on a floor
    auto probe = origin + Vec3_t(0.f, 0.f, 16.f);
    if (!MapCollision::Trace(probe, origin - Vec3_t(0.f, 0.f, FIRE_DROP), hit) || hit.normal.z < FLOOR_NORMAL_Z)
        return {};

    float start_floor = probe.z - (16.f + FIRE_DROP) * hit.fraction;

    auto stretch = [&](int x, int y) {
        return std::sqrt(static_cast<float>(x * x + y * y)) * FIRE_CELL / FIRE_RADIUS;
    };

    size_t budget = 0;
    {
        int reach = static_cast<int>(std::ceil(FIRE_RADIUS / FIRE_CELL));
        for (int y = -reach; y <= reach; y++)
            for (int x = -reach; x <= reach; x++)
                budget += stretch(x, y) <= 1.f;
    }

    constexpr int directions[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    constexpr float climb = FIRE_STEP + 1.f;

    AreaShape shape;
    shape.cell_size = FIRE_CELL;
    shape.origin_x = origin.x;
    shape.origin_y = origin.y;

    Frontier frontier;
    std::unordered_set<int64_t> queued;

    frontier.push({ 0.f, 0, 0, 0, start_floor });
    queued.insert(Key(0, 0));

    // Spreads over the floor, stopped by walls & drops
    while (!frontier.empty() && shape.cells.size() < budget) {
        auto cell = frontier.top();
        frontier.pop();

        if (cell.stretch > FIRE_MAX_STRETCH)
            break;

        shape.cells.push_back({ cell.x, cell.y, cell.floor, cell.floor });

        Vec3_t from(origin.x + cell.x * FIRE_CELL, origin.y + cell.y * FIRE_CELL, cell.floor + climb);

        for (const auto& direction : directions) {
            int x = cell.x + direction[0], y = cell.y + direction[1];
            auto key = Key(x, y);

            if (queued.contains(key))
                continue;

            Vec3_t to(origin.x + x * FIRE_CELL, origin.y + y * FIRE_CELL, from.z);

            if (MapCollision::Trace(from, to, hit))
                continue;

            if (!MapCollision::Trace(to, to - Vec3_t(0.f, 0.f, climb + FIRE_DROP), hit) || hit.normal.z < FLOOR_NORMAL_Z)
                continue;

            queued.insert(key);
            frontier.push({ stretch(x, y), x, y, 0, to.z - (climb + FIRE_DROP) * hit.fraction });
        }
    }

    return shape;
}

AreaShape GrenadeArea::Fires(const std::vector<Vec3_t>& fires, float half_width) {
    AreaShape shape;
    shape.cell_size = FIRE_GRID;

    std::unordered_map<int64_t, size_t> cells;
    std::vector<float> closest; // Squared distance to the fire the cell takes its height from

    // Fires spread up to ~70 apart, wide enough that next ones touch & the area is one piece
    half_width = std::max(half_width, FIRE_MIN_COVER);
    int reach = static_cast<int>(std::ceil(half_width / FIRE_GRID));

    for (const auto& fire : fires) {
        int fire_x = static_cast<int>(std::round(fire.x / FIRE_GRID));
        int fire_y = static_cast<int>(std::round(fire.y / FIRE_GRID));

        for (int y = fire_y - reach; y <= fire_y + reach; y++) {
            for (int x = fire_x - reach; x <= fire_x + reach; x++) {
                float dx = x * FIRE_GRID - fire.x, dy = y * FIRE_GRID - fire.y;
                float distance = dx * dx + dy * dy;

                if (distance > half_width * half_width)
                    continue;

                auto [it, inserted] = cells.try_emplace(Key(x, y), shape.cells.size());

                if (inserted) {
                    shape.cells.push_back({ x, y, fire.z, fire.z });
                    closest.push_back(distance);
                }
                else if (distance < closest[it->second]) {
                    shape.cells[it->second].floor = shape.cells[it->second].top = fire.z;
                    closest[it->second] = distance;
                }
            }
        }
    }

    if (shape.cells.empty())
        return shape;

    // Holes inside are filled: empty cells the outside of the bounds cannot reach
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (const auto& cell : shape.cells) {
        min_x = std::min(min_x, cell.x - 1); max_x = std::max(max_x, cell.x + 1);
        min_y = std::min(min_y, cell.y - 1); max_y = std::max(max_y, cell.y + 1);
    }

    std::unordered_set<int64_t> outside;
    std::vector<std::pair<int, int>> open = { { min_x, min_y } };
    outside.insert(Key(min_x, min_y));

    while (!open.empty()) {
        auto [x, y] = open.back();
        open.pop_back();

        constexpr int directions[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
        for (const auto& direction : directions) {
            int nx = x + direction[0], ny = y + direction[1];
            if (nx < min_x || nx > max_x || ny < min_y || ny > max_y)
                continue;

            auto key = Key(nx, ny);
            if (cells.contains(key) || !outside.insert(key).second)
                continue;

            open.emplace_back(nx, ny);
        }
    }

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            auto key = Key(x, y);
            if (cells.contains(key) || outside.contains(key))
                continue;

            // Height of the closest fire
            float best = FLT_MAX, z = 0.f;
            for (const auto& fire : fires) {
                float dx = x * FIRE_GRID - fire.x, dy = y * FIRE_GRID - fire.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    z = fire.z;
                }
            }

            cells.emplace(key, shape.cells.size());
            shape.cells.push_back({ x, y, z, z });
        }
    }

    return shape;
}

AreaShape GrenadeArea::Disc(const Vec3_t& center, float radius, float height, float cell_size) {
    AreaShape shape;
    shape.cell_size = cell_size;
    shape.origin_x = center.x;
    shape.origin_y = center.y;

    int reach = static_cast<int>(std::ceil(radius / cell_size));

    for (int y = -reach; y <= reach; y++)
        for (int x = -reach; x <= reach; x++)
            if ((x * x + y * y) * cell_size * cell_size <= radius * radius)
                shape.cells.push_back({ x, y, center.z, center.z + height });

    return shape;
}
