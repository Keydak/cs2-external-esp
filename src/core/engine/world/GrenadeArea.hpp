#pragma once

// Ground footprint of a smoke or fire: a grid of columns, each with its floor & top height
struct AreaShape {
    struct Cell {
        int x, y;
        float floor;    // Where the area touches the ground
        float top;      // Top of the smoke, the floor again for fires
    };

    float cell_size = 0.f;
    float origin_x = 0.f; // World position of the center of cell (0, 0)
    float origin_y = 0.f;
    std::vector<Cell> cells;

    bool Valid() const { return !cells.empty(); }
};

// Shapes smokes & fires against the map collision, so they follow walls, doors & ledges like in game
class GrenadeArea {
public:
    // Volume a smoke fills from where it popped, flowing around walls & through openings
    static AreaShape Smoke(const Vec3_t& origin, float voxel_size = 16.f);

    // Ground a molotov is going to set on fire when it lands here
    static AreaShape FireSpread(const Vec3_t& origin);

    // Ground covered by burning fires
    static AreaShape Fires(const std::vector<Vec3_t>& fires, float half_width);

    // Fallback while there is no collision for the map
    static AreaShape Disc(const Vec3_t& center, float radius, float height, float cell_size);
};
