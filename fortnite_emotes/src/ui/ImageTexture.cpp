#include "ui/ImageTexture.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace emotes::ui {

namespace {
std::atomic<ID3D11Device*> g_device{nullptr};

bool DecodePixels(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, int maxSide, std::vector<uint8_t>* pixels,
                  int* outW, int* outH) {
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096)
        return false;

    ComPtr<IWICBitmapSource> source = frame;
    if (static_cast<int>(std::max(w, h)) > maxSide) {
        const float k = static_cast<float>(maxSide) / static_cast<float>(std::max(w, h));
        const UINT nw = std::max(1u, static_cast<UINT>(w * k + 0.5f)), nh = std::max(1u, static_cast<UINT>(h * k + 0.5f));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), nw, nh, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler, w = nw, h = nh;
    }
    ComPtr<IWICFormatConverter> rgba;  // straight alpha, as the ImGui DX11 backend blends
    if (FAILED(wic->CreateFormatConverter(&rgba)) ||
        FAILED(rgba->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    pixels->assign(static_cast<size_t>(w) * h * 4, 0);
    if (FAILED(rgba->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels->size()), pixels->data()))) {
        pixels->clear();
        return false;
    }
    *outW = static_cast<int>(w);
    *outH = static_cast<int>(h);
    return true;
}

ID3D11ShaderResourceView* MakeTexture(ID3D11Device* device, const uint8_t* pixels, int w, int h) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels, static_cast<UINT>(w) * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &tex)) || FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;
    return srv;
}

// makeDecoder(factory, &decoder) opens the source (a file or a memory buffer); the rest is shared.
template <class MakeDecoder>
bool DecodeWith(int maxSide, const MakeDecoder& makeDecoder, std::vector<uint8_t>* pixels, int* w, int* h) {
    if (maxSide <= 0) return false;
    // Join whatever apartment the thread has (RPC_E_CHANGED_MODE = already an STA, e.g. the game's render thread).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            makeDecoder(wic.Get(), decoder.GetAddressOf()))
            ok = DecodePixels(wic.Get(), decoder.Get(), maxSide, pixels, w, h);
    }  // every COM object is released before CoUninitialize
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}
}  // namespace

void SetTextureDevice(ID3D11Device* device) { g_device = device; }

ID3D11ShaderResourceView* LoadImageTexture(const std::filesystem::path& file, int maxSide) {
    ID3D11Device* device = g_device.load();
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!device || !DecodeWith(maxSide, [&](IWICImagingFactory* wic, IWICBitmapDecoder** decoder) {
            return SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, decoder));
        }, &pixels, &w, &h))
        return nullptr;
    return MakeTexture(device, pixels.data(), w, h);
}

bool DecodeImage(const void* data, size_t size, int maxSide, std::vector<uint8_t>* rgba, int* w, int* h) {
    if (!data || !size || size > 0xFFFFFFFFu || !rgba || !w || !h) return false;
    return DecodeWith(maxSide, [&](IWICImagingFactory* wic, IWICBitmapDecoder** decoder) {
        ComPtr<IWICStream> stream;  // the decoder keeps its own reference; the buffer only has to live for this call
        return SUCCEEDED(wic->CreateStream(&stream)) &&
               SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)),
                                                      static_cast<DWORD>(size))) &&
               SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder));
    }, rgba, w, h);
}

ID3D11ShaderResourceView* CreateImageTexture(const uint8_t* rgba, int w, int h) {
    ID3D11Device* device = g_device.load();
    if (!device || !rgba || w <= 0 || h <= 0) return nullptr;
    return MakeTexture(device, rgba, w, h);
}

}  // namespace emotes::ui
