// The wheel's two faces: Fortnite's Burbank Big Condensed Black for the titles (if installed or in the pack's fonts
// folder), Segoe UI Bold for the rest. Either falls back to ImGui's built-in font.
#pragma once
#include <filesystem>
struct ImFont;

namespace emotes::ui {

void LoadFonts(const std::filesystem::path& packDir);  // after ImGui::CreateContext, before the first frame
ImFont* HeavyFont();
ImFont* BodyFont();

}  // namespace emotes::ui
