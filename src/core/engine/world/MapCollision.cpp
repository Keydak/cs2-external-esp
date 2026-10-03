#include "MapCollision.hpp"
#include "MapExport.hpp"

#include <algorithm>

namespace {
    constexpr char TRI_MAGIC[4] = { 'C', 'S', '2', 'T' };
    constexpr uint32_t TRI_VERSION = 1;
    constexpr uint32_t LEAF_SIZE = 4;

    Vec3_t Min(const Vec3_t& a, const Vec3_t& b) {
        return Vec3_t(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
    }

    Vec3_t Max(const Vec3_t& a, const Vec3_t& b) {
        return Vec3_t(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
    }

    Vec3_t Cross(const Vec3_t& a, const Vec3_t& b) {
        return Vec3_t(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    float Dot(const Vec3_t& a, const Vec3_t& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    // Looks next to the executable first, then the working directory
    std::filesystem::path FindMapFile(const std::string& name) {
        auto file = std::filesystem::path("maps") / (name + ".tri");

        char exe_path[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
            auto candidate = std::filesystem::path(exe_path).parent_path() / file;
            if (std::filesystem::exists(candidate))
                return candidate;
        }

        return file;
    }

    // Where a built map goes, next to the executable
    std::filesystem::path BuildPath(const std::string& name) {
        auto file = std::filesystem::path("maps") / (name + ".tri");

        char exe_path[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH))
            return std::filesystem::path(exe_path).parent_path() / file;

        return file;
    }

    // Missing, or older than the map of the game after an update
    bool NeedsBuild(const std::string& name) {
        std::error_code error;
        auto file = FindMapFile(name);

        if (!std::filesystem::exists(file, error))
            return true;

        auto maps = MapExport::FindMapsDir();
        if (maps.empty())
            return false;

        auto vpk_time = std::filesystem::last_write_time(maps / (name + ".vpk"), error);
        if (error)
            return false;

        auto file_time = std::filesystem::last_write_time(file, error);
        return !error && vpk_time > file_time;
    }
}

void MapCollision::SetMap(const std::string& raw_name) {
    // "maps/de_dust2.vpk" -> "de_dust2"
    auto name = std::filesystem::path(raw_name).stem().string();

    auto& i = GetInstance();
    {
        std::lock_guard<std::mutex> lock(i.mtx);
        if (name == i.requested)
            return;

        i.requested = name;
        i.world.reset();
        i.boxes.reset();
        i.status = name.empty() ? "No map" : "Loading " + name + "...";
    }

    if (name.empty())
        return;

    std::thread([name]() {
        auto& i = GetInstance();

        // Built from the map files of the game the first time the map is played
        if (NeedsBuild(name)) {
            {
                std::lock_guard<std::mutex> lock(i.mtx);
                if (i.requested != name)
                    return;
                i.status = "Building " + name + " from the game files...";
                i.progress = { true, false, 0.05f, name, {} };
            }

            MapExport::Export(name, BuildPath(name), [](float value) { SetProgress(value); });
        }

        auto world = Load(name);

        {
            std::lock_guard<std::mutex> lock(i.mtx);
            if (i.progress.active && i.progress.map == name) {
                i.progress.active = false;
                i.progress.failed = world == nullptr;
                i.progress.value = 1.f;
                i.progress.finished = std::chrono::steady_clock::now();
            }
        }

        std::lock_guard<std::mutex> lock(i.mtx);

        // The map changed again while loading
        if (i.requested != name)
            return;

        if (world) {
            i.status = std::format("{} ({} triangles)", name, world->triangles.size());
            i.world = std::move(world);
        }
        else
            i.status = std::format("No collision for {}, it could not be built from the game files", name);
    }).detach();
}

MapCollision::Progress MapCollision::GetProgress() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);
    return i.progress;
}

void MapCollision::SetProgress(float value) {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);

    if (i.progress.active)
        i.progress.value = std::max(i.progress.value, value);
}

bool MapCollision::IsLoaded() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);
    return i.world != nullptr;
}

std::string MapCollision::GetStatus() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);
    return i.status;
}

void MapCollision::SetBoxes(std::vector<Box> boxes) {
    auto oriented = std::make_shared<std::vector<OrientedBox>>();
    oriented->reserve(boxes.size());

    for (const auto& box : boxes) {
        // Same as AngleMatrix of the game
        constexpr float RAD = 3.14159265f / 180.f;
        float sp = sinf(box.angles.x * RAD), cp = cosf(box.angles.x * RAD);
        float sy = sinf(box.angles.y * RAD), cy = cosf(box.angles.y * RAD);
        float sr = sinf(box.angles.z * RAD), cr = cosf(box.angles.z * RAD);

        OrientedBox o;
        o.origin = box.origin;
        o.mins = box.mins;
        o.maxs = box.maxs;
        o.axes[0] = Vec3_t(cp * cy, cp * sy, -sp);
        o.axes[1] = Vec3_t(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp);
        o.axes[2] = Vec3_t(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);

        o.bounds_min = Vec3_t(FLT_MAX, FLT_MAX, FLT_MAX);
        o.bounds_max = Vec3_t(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int corner = 0; corner < 8; corner++) {
            Vec3_t local(corner & 1 ? box.maxs.x : box.mins.x, corner & 2 ? box.maxs.y : box.mins.y, corner & 4 ? box.maxs.z : box.mins.z);
            auto world = box.origin + o.axes[0] * local.x + o.axes[1] * local.y + o.axes[2] * local.z;
            o.bounds_min = Min(o.bounds_min, world);
            o.bounds_max = Max(o.bounds_max, world);
        }

        oriented->push_back(o);
    }

    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);
    i.boxes = std::move(oriented);
}

bool MapCollision::TraceBox(const OrientedBox& box, const Vec3_t& start, const Vec3_t& end, Hit& hit) {
    // Into the space of the box, it is axis aligned there
    auto offset = start - box.origin;
    auto delta = end - start;

    float p[3] = { Dot(offset, box.axes[0]), Dot(offset, box.axes[1]), Dot(offset, box.axes[2]) };
    float d[3] = { Dot(delta, box.axes[0]), Dot(delta, box.axes[1]), Dot(delta, box.axes[2]) };
    float lo[3] = { box.mins.x, box.mins.y, box.mins.z };
    float hi[3] = { box.maxs.x, box.maxs.y, box.maxs.z };

    float enter = -FLT_MAX, leave = FLT_MAX;
    int axis = -1;

    for (int a = 0; a < 3; a++) {
        if (fabsf(d[a]) < 1e-6f) {
            if (p[a] < lo[a] || p[a] > hi[a])
                return false;
            continue;
        }

        float t0 = (lo[a] - p[a]) / d[a], t1 = (hi[a] - p[a]) / d[a];
        if (t0 > t1)
            std::swap(t0, t1);

        if (t0 > enter) {
            enter = t0;
            axis = a;
        }
        leave = std::min(leave, t1);

        if (enter > leave)
            return false;
    }

    // Starting inside does not count, nothing could leave a box it is stuck in
    if (axis < 0 || enter < 0.f || enter > 1.f || enter >= hit.fraction)
        return false;

    hit.fraction = enter;
    hit.normal = box.axes[axis] * (d[axis] > 0.f ? -1.f : 1.f);
    return true;
}

bool MapCollision::Blocked(const Vec3_t& start, const Vec3_t& end) {
    std::shared_ptr<World> world;
    std::shared_ptr<const std::vector<OrientedBox>> boxes;
    {
        auto& i = GetInstance();
        std::lock_guard<std::mutex> lock(i.mtx);
        world = i.world;
        boxes = i.boxes;
    }

    if (!world)
        return false;

    Hit hit;

    // The few boxes first, they are cheap
    if (boxes) {
        auto low = Min(start, end), high = Max(start, end);

        for (const auto& box : *boxes) {
            if (high.x < box.bounds_min.x || low.x > box.bounds_max.x ||
                high.y < box.bounds_min.y || low.y > box.bounds_max.y ||
                high.z < box.bounds_min.z || low.z > box.bounds_max.z)
                continue;

            if (TraceBox(box, start, end, hit))
                return true;
        }
    }

    return TraceWorld(*world, start, end, hit, true);
}

bool MapCollision::Trace(const Vec3_t& start, const Vec3_t& end, Hit& hit) {
    std::shared_ptr<World> world;
    std::shared_ptr<const std::vector<OrientedBox>> boxes;
    {
        auto& i = GetInstance();
        std::lock_guard<std::mutex> lock(i.mtx);
        world = i.world;
        boxes = i.boxes;
    }

    if (!world)
        return false;

    bool hit_any = TraceWorld(*world, start, end, hit);
    if (!hit_any)
        hit.fraction = 1.f;

    // The closest of the world & the boxes
    if (boxes) {
        auto low = Min(start, end), high = Max(start, end);

        for (const auto& box : *boxes) {
            if (high.x < box.bounds_min.x || low.x > box.bounds_max.x ||
                high.y < box.bounds_min.y || low.y > box.bounds_max.y ||
                high.z < box.bounds_min.z || low.z > box.bounds_max.z)
                continue;

            hit_any |= TraceBox(box, start, end, hit);
        }
    }

    return hit_any;
}

std::shared_ptr<MapCollision::World> MapCollision::Load(const std::string& name) {
    auto path = FindMapFile(name);

    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        LOGF(WARNING, "Grenade prediction has no collision for '{}', expected {}", name, path.string());
        return nullptr;
    }

    char magic[4]{};
    uint32_t version = 0, count = 0;
    file.read(magic, sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (memcmp(magic, TRI_MAGIC, sizeof(magic)) != 0 || version != TRI_VERSION) {
        LOGF(WARNING, "{} is not a valid collision file, export it again", path.string());
        return nullptr;
    }

    std::vector<float> raw(static_cast<size_t>(count) * 9);
    file.read(reinterpret_cast<char*>(raw.data()), raw.size() * sizeof(float));

    if (!file) {
        LOGF(WARNING, "{} is truncated, export it again", path.string());
        return nullptr;
    }

    auto world = std::make_shared<World>();
    world->name = name;
    world->triangles.reserve(count);

    for (uint32_t t = 0; t < count; t++) {
        const float* v = &raw[t * 9];
        Vec3_t a(v[0], v[1], v[2]), b(v[3], v[4], v[5]), c(v[6], v[7], v[8]);
        world->triangles.push_back({ a, b - a, c - a });
    }

    SetProgress(0.9f);

    auto start = std::chrono::steady_clock::now();
    Build(*world);

    LOGF(INFO, "Loaded collision for {} ({} triangles, bvh built in {}ms)",
        name, count, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());

    return world;
}

void MapCollision::Build(World& world) {
    auto& triangles = world.triangles;
    auto& nodes = world.nodes;

    nodes.clear();
    nodes.reserve(triangles.size() * 2 / LEAF_SIZE + 1);

    std::vector<Vec3_t> centers(triangles.size());
    for (size_t i = 0; i < triangles.size(); i++) {
        const auto& t = triangles[i];
        centers[i] = t.v0 + (t.e1 + t.e2) / 3.f;
    }

    std::vector<uint32_t> order(triangles.size());
    for (uint32_t i = 0; i < order.size(); i++)
        order[i] = i;

    struct Task {
        uint32_t node, first, count;
    };

    std::vector<Task> stack;
    nodes.push_back({});
    stack.push_back({ 0, 0, static_cast<uint32_t>(triangles.size()) });

    while (!stack.empty()) {
        auto task = stack.back();
        stack.pop_back();

        Vec3_t min(FLT_MAX, FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        Vec3_t center_min = min, center_max = max;

        for (uint32_t i = task.first; i < task.first + task.count; i++) {
            const auto& t = triangles[order[i]];
            auto b = t.v0 + t.e1, c = t.v0 + t.e2;

            min = Min(min, Min(t.v0, Min(b, c)));
            max = Max(max, Max(t.v0, Max(b, c)));

            center_min = Min(center_min, centers[order[i]]);
            center_max = Max(center_max, centers[order[i]]);
        }

        nodes[task.node].min = min;
        nodes[task.node].max = max;

        if (task.count <= LEAF_SIZE) {
            nodes[task.node].first = task.first;
            nodes[task.node].count = task.count;
            continue;
        }

        // Median split along the longest axis of the centers
        auto extent = center_max - center_min;
        int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);

        auto begin = order.begin() + task.first;
        auto middle = begin + task.count / 2;
        std::nth_element(begin, middle, begin + task.count, [&](uint32_t a, uint32_t b) {
            return centers[a][axis] < centers[b][axis];
        });

        uint32_t left_count = task.count / 2;

        // Children are allocated as a pair, the right one always follows the left one
        auto left = static_cast<uint32_t>(nodes.size());
        nodes.push_back({});
        nodes.push_back({});

        nodes[task.node].first = left;
        nodes[task.node].count = 0;

        stack.push_back({ left, task.first, left_count });
        stack.push_back({ left + 1, task.first + left_count, task.count - left_count });
    }

    // Leaves index into a reordered triangle list
    std::vector<Triangle> sorted(triangles.size());
    for (size_t i = 0; i < order.size(); i++)
        sorted[i] = triangles[order[i]];
    triangles = std::move(sorted);
}

bool MapCollision::TraceWorld(const World& world, const Vec3_t& start, const Vec3_t& end, Hit& hit, bool any_hit) {
    if (world.nodes.empty())
        return false;

    auto direction = end - start;
    Vec3_t inverse(
        1.f / (direction.x != 0.f ? direction.x : 1e-12f),
        1.f / (direction.y != 0.f ? direction.y : 1e-12f),
        1.f / (direction.z != 0.f ? direction.z : 1e-12f)
    );

    float closest = 1.f;
    const Triangle* closest_triangle = nullptr;

    uint32_t stack[64];
    int stack_size = 0;
    stack[stack_size++] = 0;

    while (stack_size > 0) {
        const auto& node = world.nodes[stack[--stack_size]];

        // Slab test against the segment [0, closest]
        float t_min = 0.f, t_max = closest;
        bool miss = false;

        for (int axis = 0; axis < 3 && !miss; axis++) {
            float t0 = (node.min[axis] - start[axis]) * inverse[axis];
            float t1 = (node.max[axis] - start[axis]) * inverse[axis];
            if (t0 > t1) std::swap(t0, t1);

            t_min = std::max(t_min, t0);
            t_max = std::min(t_max, t1);
            miss = t_min > t_max;
        }

        if (miss)
            continue;

        if (node.count == 0) {
            if (stack_size + 2 > IM_ARRAYSIZE(stack))
                continue;

            stack[stack_size++] = node.first;      // left
            stack[stack_size++] = node.first + 1;  // right
            continue;
        }

        // Möller–Trumbore, two sided
        for (uint32_t i = node.first; i < node.first + node.count; i++) {
            const auto& t = world.triangles[i];

            auto p = Cross(direction, t.e2);
            float det = Dot(t.e1, p);
            if (std::fabs(det) < 1e-9f)
                continue;

            float inv_det = 1.f / det;
            auto s = start - t.v0;

            float u = Dot(s, p) * inv_det;
            if (u < 0.f || u > 1.f)
                continue;

            auto q = Cross(s, t.e1);
            float v = Dot(direction, q) * inv_det;
            if (v < 0.f || u + v > 1.f)
                continue;

            float distance = Dot(t.e2, q) * inv_det;
            if (distance > 0.f && distance < closest) {
                closest = distance;
                closest_triangle = &t;

                // Anything in the way is enough, no need for the closest
                if (any_hit) {
                    hit.fraction = closest;
                    return true;
                }
            }
        }
    }

    if (!closest_triangle)
        return false;

    auto normal = Cross(closest_triangle->e1, closest_triangle->e2);
    normal /= (normal.length() + 1e-9f);

    if (Dot(normal, direction) > 0.f)
        normal = -normal;

    hit.fraction = closest;
    hit.normal = normal;
    return true;
}
