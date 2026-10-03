#pragma once

// Made by scripts/render_player_previews.py, the pre-rendered player of the ESP preview

namespace player_preview {
    constexpr int FRAMES = 12;          // Angles around, frame k is turned k * 360 / FRAMES degrees
    constexpr int COLUMNS = 4;         // Of the grid
    constexpr int FRAME_WIDTH = 256;
    constexpr int FRAME_HEIGHT = 320;
    constexpr int FEET = 312;          // Row of the feet in a frame
}

extern const unsigned char player_preview_t[];        // JPEG
extern const size_t player_preview_t_size;
extern const unsigned char player_preview_t_alpha[];  // PNG, gray
extern const size_t player_preview_t_alpha_size;
constexpr int player_preview_t_height = 291;  // Pixels from the feet to the top of the head
extern const unsigned char player_preview_ct[];        // JPEG
extern const size_t player_preview_ct_size;
extern const unsigned char player_preview_ct_alpha[];  // PNG, gray
extern const size_t player_preview_ct_alpha_size;
constexpr int player_preview_ct_height = 292;  // Pixels from the feet to the top of the head
