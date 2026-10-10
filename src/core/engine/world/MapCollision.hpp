#pragma once

// Static map collision, built from the game files by MapExport the first time a map is played
class MapCollision {
public:
    ~MapCollision()                              = default;
    MapCollision(const MapCollision&)            = delete;
    MapCollision(MapCollision&&)                 = delete;
    MapCollision& operator=(const MapCollision&) = delete;
    MapCollision& operator=(MapCollision&&)      = delete;

    struct Hit {
        float fraction = 1.f;   // 0..1 along the traced segment
        Vec3_t normal{};        // Facing against the trace direction
    };

    // Loads maps/<name>.tri in the background when the map changes
    static void SetMap(const std::string& name);

    static bool IsLoaded();
    static std::string GetStatus();

    // Building a map from the game files, for the loading bar
    struct Progress {
        bool active = false;    // Building or loading right after
        bool failed = false;
        float value = 0.f;      // 0..1
        std::string map;
        std::chrono::steady_clock::time_point finished{}; // When it was done, the bar fades out after
    };

    static Progress GetProgress();

    // Things that move & are not in the map files, like doors: a box turned by angles
    struct Box {
        Vec3_t origin{};
        Vec3_t angles{};    // Pitch, yaw, roll
        Vec3_t mins{}, maxs{};
    };

    // Replaces the boxes traces hit besides the map
    static void SetBoxes(std::vector<Box> boxes);

    // Returns true when the segment hits the world or a box
    static bool Trace(const Vec3_t& start, const Vec3_t& end, Hit& hit);

    // Is anything in the way, without where: stops at the first thing hit, way cheaper for line of sight
    static bool Blocked(const Vec3_t& start, const Vec3_t& end);

    // Anything of the world or a box with its bounds in this box: the cheap check before tracing a thick thing
    static bool Near(const Vec3_t& min, const Vec3_t& max);

private:
    MapCollision() {};

    static MapCollision& GetInstance()
    {
        static MapCollision i{};
        return i;
    }

    struct Triangle {
        Vec3_t v0, e1, e2;
    };

    struct Node {
        Vec3_t min, max;
        uint32_t first;  // First triangle (leaf) or left child (inner, the right child is first + 1)
        uint32_t count;  // Triangles in a leaf, 0 for inner nodes
    };

    // Immutable once built, swapped in as a whole so traces never see a half loaded map
    struct World {
        std::string name;
        std::vector<Triangle> triangles;
        std::vector<Node> nodes;
    };

    static std::shared_ptr<World> Load(const std::string& name);
    static void SetProgress(float value);
    static void Build(World& world);
    static bool TraceWorld(const World& world, const Vec3_t& start, const Vec3_t& end, Hit& hit, bool any_hit = false);

    // A box with its rotation worked out, columns forward, left & up
    struct OrientedBox {
        Vec3_t origin, mins, maxs;
        Vec3_t axes[3];
        Vec3_t bounds_min, bounds_max;  // Around it in the world, far away traces skip it with these
    };

    static bool TraceBox(const OrientedBox& box, const Vec3_t& start, const Vec3_t& end, Hit& hit);

private:
    std::mutex mtx;
    std::shared_ptr<World> world;
    std::shared_ptr<const std::vector<OrientedBox>> boxes;
    std::string requested;
    std::string status = "No map";
    Progress progress;
};
