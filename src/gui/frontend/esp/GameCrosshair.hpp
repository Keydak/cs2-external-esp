#pragma once

// The crosshair set in the game, read from cs2_user_convars_0_slot0.vcfg in the Steam userdata.
// The file of the account that played last, read again when the game saves it
struct GameCrosshair {
    float size = 5.f;               // cl_crosshairsize
    float thickness = 0.5f;         // cl_crosshairthickness
    float gap = 1.f;                // cl_crosshairgap
    float outline_thickness = 1.f;  // cl_crosshair_outlinethickness
    bool dot = false;               // cl_crosshairdot
    bool t_style = false;           // cl_crosshair_t, no top line
    bool outline = false;           // cl_crosshair_drawoutline

    // Newer settings of the game (cl_crosshair_length, _gap & _thickness): pixels on a screen this tall,
    // instead of the sizes above
    bool pixels = false;
    float pixel_length = 8.f, pixel_gap = 4.f, pixel_thickness = 2.f, pixel_outline = 1.f;
    float pixel_screen_height = 1080.f;

    ImU32 color = IM_COL32(50, 250, 50, 200);
    ImU32 outline_color = IM_COL32(0, 0, 0, 200);

    bool found = false;             // False when no file was found, the values are the defaults of the game

    // Current settings, looked up again every few seconds
    static const GameCrosshair& Get();

    // Draws it like the game does, around the center
    void Draw(ImDrawList* d, ImVec2 center, float screen_height) const;
};
