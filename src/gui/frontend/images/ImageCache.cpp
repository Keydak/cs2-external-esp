#include "ImageCache.hpp"

#include "gui/renderer/window/Window.hpp"
#include "updater/http/HttpHelper.hpp"

#include <d3d11.h>
#include <wincodec.h>
#include <shlwapi.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {
    constexpr int WORKERS = 4;
    const std::filesystem::path cache_dir = "cache/images";

    // File name from the url
    std::string HashName(const std::string& url) {
        uint64_t hash = 14695981039346656037ull; // FNV-1a
        for (unsigned char c : url) {
            hash ^= c;
            hash *= 1099511628211ull;
        }

        return std::format("{:016x}.img", hash);
    }

    bool ReadFile(const std::filesystem::path& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
            return false;

        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    // PNG / JPG -> RGBA through Windows Imaging Component
    bool Decode(const std::string& bytes, std::vector<uint8_t>& pixels, UINT& width, UINT& height) {
        IWICImagingFactory* factory = nullptr;
        IStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool success = false;

        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                break;

            stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
            if (!stream)
                break;

            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
                break;

            if (FAILED(decoder->GetFrame(0, &frame)))
                break;

            if (FAILED(factory->CreateFormatConverter(&converter)))
                break;

            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                break;

            if (FAILED(converter->GetSize(&width, &height)) || !width || !height)
                break;

            pixels.resize(static_cast<size_t>(width) * height * 4);
            success = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
        } while (false);

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();

        return success;
    }

    ImTextureID CreateTexture(const std::vector<uint8_t>& pixels, UINT width, UINT height) {
        if (!Window::device)
            return 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = width * 4;

        // The device is free threaded, no need to be on the render thread
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(Window::device->CreateTexture2D(&desc, &data, &texture)))
            return 0;

        ID3D11ShaderResourceView* view = nullptr;
        HRESULT result = Window::device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();

        return SUCCEEDED(result) ? reinterpret_cast<ImTextureID>(view) : 0;
    }
}

ImTextureID ImageCache::FromMemory(const void* data, size_t size, const void* alpha, size_t alpha_size, bool silhouette) {
    // Decoding needs COM on this thread, fine when it is already there in another mode
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<uint8_t> pixels, mask;
    UINT width = 0, height = 0, mask_width = 0, mask_height = 0;
    ImTextureID texture = 0;

    if (Decode(std::string(static_cast<const char*>(data), size), pixels, width, height)) {
        bool masked = alpha && Decode(std::string(static_cast<const char*>(alpha), alpha_size), mask, mask_width, mask_height)
            && mask_width == width && mask_height == height;

        if (masked)
            for (size_t i = 0; i < pixels.size(); i += 4)
                pixels[i + 3] = mask[i];

        if (silhouette)
            for (size_t i = 0; i < pixels.size(); i += 4)
                pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;

        if (masked || !alpha)
            texture = CreateTexture(pixels, width, height);
    }

    if (SUCCEEDED(com))
        CoUninitialize();

    return texture;
}

ImTextureID ImageCache::Get(const std::string& url) {
    return GetInstance().GetImpl(url);
}

ImTextureID ImageCache::GetImpl(const std::string& url) {
    if (url.empty())
        return 0;

    std::lock_guard<std::mutex> lock(this->mutex);

    if (!this->started) {
        this->started = true;

        for (int i = 0; i < WORKERS; i++)
            std::thread(&ImageCache::Worker, this).detach();
    }

    auto [it, inserted] = this->entries.try_emplace(url);
    if (inserted) {
        this->queue.push_back(url);
        this->wake.notify_one();
    }

    return it->second.state == State::Loaded ? it->second.texture : 0;
}

void ImageCache::Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (true) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->wake.wait(lock, [this] { return !this->queue.empty(); });

            url = this->queue.back();
            this->queue.pop_back();
        }

        auto texture = Load(url);

        std::lock_guard<std::mutex> lock(this->mutex);
        auto& entry = this->entries[url];
        entry.texture = texture;
        entry.state = texture ? State::Loaded : State::Failed;
    }
}

ImTextureID ImageCache::Load(const std::string& url) {
    auto path = cache_dir / HashName(url);

    std::string bytes;
    if (!ReadFile(path, bytes)) {
        if (HttpHelper::GetRaw(url, bytes) != 200 || bytes.empty())
            return 0;

        std::error_code error;
        std::filesystem::create_directories(cache_dir, error);

        std::ofstream f(path, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<uint8_t> pixels;
    UINT width = 0, height = 0;

    if (!Decode(bytes, pixels, width, height)) {
        // Broken file in the cache, it is downloaded again next start
        std::error_code error;
        std::filesystem::remove(path, error);
        return 0;
    }

    return CreateTexture(pixels, width, height);
}
