#pragma once
#include "assets/fonts/Icons.h"

enum Tab {
    PLAYERS,
    BOMB,
    PROJECTILES,
    ITEMS,
    SKINS,
    MOVEMENT,
    MISC,
    CONFIGS,
    SETTINGS
};

class Menu {
public:
    ~Menu() = default;
    Menu(const Menu&) = delete;
    Menu(Menu&&) = delete;
    Menu& operator=(const Menu&) = delete;
    Menu& operator=(Menu&&) = delete;

    static bool Init();
    static void Render();

    static void RenderStartupHelp();

    static ImVec2 GetPos();
    static ImVec2 GetSize();
private:
    Menu() {};

    static Menu& GetInstance()
    {
        static Menu i{};
        return i;
    }

    bool InitImpl();
    void RenderImpl();
    void RenderStartupHelpImpl();

    void RenderBackground();
    void RenderSidebar();
    void RenderHeader();

    void RenderPreviewPanel(float alpha);
    void RenderPlayersPreview();
    void RenderBombPreview();
    void RenderProjectilesPreview();
    void RenderItemsPreview();

    void RenderPlayersTab();
    void RenderBombTab();
    void RenderProjectilesTab();
    void RenderItemsTab();
    void RenderSkinsTab();
    void RenderMovementTab();
    void RenderMiscTab();
    void RenderConfigsTab();
    void RenderSettingsTab();

    void SetupStyles();
private:
    bool isSetup = true;

    ImVec2 pos;
    ImVec2 size;

    Tab active_tab = Tab::PLAYERS;

    // Players tab
    int esp_group = 1; // Team, enemy
    float model_yaw = -0.52f; // Preview model, radians, a bit turned with the rifle towards us
    float bomb_yaw = 0.6f;     // Cameras of the bomb & projectile previews
    float projectile_yaw = 1.1f;
    float items_yaw = 0.8f;

    // Skins tab
    enum class SkinPage { ITEMS, GLOVES, KNIVES, AGENTS, MUSIC_KITS, PRESETS, SKINS };

    int skin_team = 0;                  // cfg::skins::team_t
    SkinPage skin_page = SkinPage::ITEMS;
    int selected_item = 0;              // Definition index of the item on the skins page
    char skin_search[64]{};
    float skin_page_progress = 1.f;     // Pages slide in like tabs

    void OpenSkinPage(SkinPage page, int item = 0);

    float applied_scale = 0.f; // cfg::settings::ui_scale, once the mouse is let go

    // Animations
    bool was_open = false;
    float open_progress = 0.f;  // Fade in & out
    float tab_progress = 1.f;   // Content sliding in after a tab switch
    float indicator_y = -1.f;   // Sidebar highlight, relative to the window
};
