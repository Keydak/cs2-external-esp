#pragma once

#include "core/engine/cache/Cache.hpp"

class Overlays {
public:
    ~Overlays() = default;
    Overlays(const Overlays&) = delete;
    Overlays(Overlays&&) = delete;
    Overlays& operator=(const Overlays&) = delete;
    Overlays& operator=(Overlays&&) = delete;

    static bool Init();
    static void Render();

    // Site & time left of the bomb, measured only when d is null. Also drawn by the menu preview
    static ImVec2 DrawBombCard(ImDrawList* d, ImVec2 pos, const Bomb& bomb);

private:
    ImFont* font;
    ImFont* font_alt;
    ImFont* font_icons;

    size_t vel_index = 0;
    std::vector<int> vel_buffer;
    float vel_accumulator = 0.0f;
private:
    Overlays() {};

    static Overlays& GetInstance()
    {
        static Overlays i{};
        return i;
    }

    bool InitImpl();
    void RenderImpl();

    void RenderBomb();
    void RenderMapProgress();
    ImVec2 DrawBombCardImpl(ImDrawList* d, ImVec2 pos, const Bomb& bomb);
    void RenderRadar();
    void RenderNotice();
    void RenderEspStatus();
    void RenderWatermark();
    void RenderSpeedChart();
    void RenderDebugWindow();
    void RenderSpectatorList();
    void RenderKeybinds();
    void RenderVotes();
    void RenderHitmarkers();
};
