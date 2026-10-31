// PNG/JPG files -> D3D11 textures for ImGui (AddImage), decoded with Windows Imaging Component.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
struct ID3D11Device;
struct ID3D11ShaderResourceView;
namespace emotes::ui {
// The device textures are made on: Hooks.cpp after ImGui init, null before it is released.
void SetTextureDevice(ID3D11Device* device);
// Longer side scaled down to at most maxSide px (high quality). Render thread; null on any failure. Caller Release()s.
ID3D11ShaderResourceView* LoadImageTexture(const std::filesystem::path& file, int maxSide);
// Encoded PNG/JPG/BMP bytes (a cover thumbnail) -> RGBA8 pixels, longer side scaled down to maxSide. Any thread
// (a worker, so the decode never costs the render thread a frame); false on failure.
bool DecodeImage(const void* data, size_t size, int maxSide, std::vector<uint8_t>* rgba, int* w, int* h);
// RGBA8 pixels (from DecodeImage) -> texture. Render thread; null on failure. Caller Release()s.
ID3D11ShaderResourceView* CreateImageTexture(const uint8_t* rgba, int w, int h);
}  // namespace emotes::ui
