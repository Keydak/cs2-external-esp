#pragma once

#include "core/engine/cache/Cache.hpp"

#include <imgui_internal.h>

class Esp {
public:
    ~Esp() = default;
    Esp(const Esp&) = delete;
    Esp(Esp&&) = delete;
    Esp& operator=(const Esp&) = delete;
    Esp& operator=(Esp&&) = delete;

    static bool Init();
    static void Render();

    // Where a flag was drawn, for the menu preview to drag it
    struct FlagArea {
        int flag;
        int side;
        int index;      // In the layout of the side
        ImRect rect;
    };

    // Drawn exactly like on players, the menu preview uses these too
    static void DrawBox(ImDrawList* d, Vec2_t min, Vec2_t max, const color_t& color);
    static void DrawTracker(ImDrawList* d, Vec2_t head, float box_width, const color_t& color);
    // Head, chest or pelvis can be seen from our eyes, through neither the map nor a smoke

    // Bomb & grenades seen through a made up camera, for the menu previews. Nothing is drawn for null ones
    static void RenderPreview(ImDrawList* d, const view_matrix_t& matrix, const Bomb* bomb,
        const std::vector<Grenade>& grenades, const GrenadePath* path,
        const std::vector<Item>* items = nullptr, Vec3_t viewer = {});

    // Font of the weapon icons, for the stand-ins of the menu previews
    static ImFont* GetIconFont();

    // force: every flag, even the ones the player does not have right now
    static void DrawFlags(ImDrawList* d, const Player& local, const Player& player, Vec2_t min, Vec2_t max,
        const cfg::esp::group_t& group, bool force = false, std::vector<FlagArea>* areas = nullptr);

private:
    ImGuiIO io;
    ImFont* font;
    ImFont* font_merged_icons;
    ImDrawList* d;

    // Temporary storage for ease
    view_matrix_t matrix;
private:
    Esp() {};

    static Esp& GetInstance()
    {
        static Esp i{};
        return i;
    }

    bool InitImpl();
    void RenderImpl();


    void RenderPlayer(const Player& local, const Player& player, const cfg::esp::group_t& group, bool visible);
    void RenderPlayerBones(const Player& player, bool visible, const color_t& visible_color, const color_t& invisible_color);
    void DrawFlagsImpl(ImDrawList* d, const Player& local, const Player& player, Vec2_t min, Vec2_t max,
        const cfg::esp::group_t& group, bool force, std::vector<FlagArea>* areas);
    void RenderPlayerTracker(const Player& player, std::pair<Vec2_t, Vec2_t> bounds, const color_t& color);
    void RenderPlayerTracers(const Player& source, const Player& player, const cfg::esp::group_t& group, bool visible);
    
    void RenderBombBox(Bomb bomb);
    void RenderItems(const std::vector<Item>& items, const Vec3_t& viewer);
    void RenderGrenades(const std::vector<Grenade>& grenades);
    void RenderGrenadePrediction(const GrenadePath& path);
    void RenderGrenadeLanding(const Grenade& grenade, ImU32 color);
    static bool ShowsArea(GrenadeType type);   // The area of a smoke or fire with its switch on
    void RenderPathEnd(const GrenadePath& path, ImU32 color, const char* icon, float time);
    void RenderGroundCircle(const Vec3_t& center, float radius, ImU32 color, int segments, bool filled, bool glow = false);
    void RenderArea(const AreaShape& area, ImU32 color, bool glow = false);
	void RenderCrosshair(Player local);
};