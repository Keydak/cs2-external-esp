#include "AgentPreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {
    namespace pano = offsets::panorama;

    constexpr auto POLL = std::chrono::milliseconds(16);
    constexpr auto ASKED_FOR = std::chrono::milliseconds(400);    // Not asked for this long (the card is gone), it stops.
                                                                    // Frames of the overlay can be slow (free CPU)
    constexpr auto RETRY_AFTER = std::chrono::seconds(2);         // A team that failed
    constexpr int MAX_ATTEMPTS = 3;                                 // Of a team, then the menu opens without its picture
    constexpr auto GIVE_UP = std::chrono::minutes(3);             // The game not in a menu or a match all that time
    constexpr auto LOAD_TIME = std::chrono::seconds(15);          // The map & the agent of the panel, at most
    constexpr auto SETTLE = std::chrono::milliseconds(100);       // A background set, then on the screen
    constexpr int EDGE = 2;                                         // Pixels of the spot left out, the panel is whole
    constexpr int MAX_FAILURES = 3;

    // Page of a call: the code, the origin of the script, the script
    constexpr size_t PAGE_SIZE = 0x4000;
    constexpr uintptr_t PAGE_ORIGIN = 0x100;
    constexpr uintptr_t PAGE_SCRIPT = 0x200;
    // A name of our own. Line 0 & column 0 would take the compiled script cached for the origin (of the game for one
    // of its files, the first of ours after that): line 1 compiles it every time
    constexpr const char* ORIGIN = "cs2ext/agent_preview.js";

    // The agents, their camera of the loadout of the game
    constexpr const char* MODEL_T = "agents/models/tm_professional/tm_professional_vari.vmdl";   // Number K
    constexpr const char* MODEL_CT = "agents/models/ctm_sas/ctm_sas.vmdl";                      // Default CT
    constexpr const char* ID_T = "cs2ext_preview_t";
    constexpr const char* ID_CT = "cs2ext_preview_ct";

    // Every panel of ours under r (also copies & the ones of earlier versions) as p, made into a JavaScript loop
    std::string ForAll(const char* body) {
        return std::format("var ids=['{}','{}','cs2ext_agent_t','cs2ext_agent_ct'];"
            "var each=function(t){{for(var i=0;i<t.GetChildCount();i++){{var p=t.GetChild(i);"
            "if(ids.indexOf(p.id)>=0){{{}}}else each(p);}}}};each(r);", ID_T, ID_CT, body);
    }

    // Page of the RunFrame hook: the job, a stub & the vtable copy
    enum JobState : uint32_t { JOB_IDLE = 0, JOB_QUEUED = 1, JOB_RUNNING = 2, JOB_DONE = 3 };
    constexpr size_t FRAME_PAGE_SIZE = 0x2000;
    constexpr uintptr_t JOB_STATE = 0x00;       // uint32
    constexpr uintptr_t JOB_FUNCTION = 0x08;    // uint64
    constexpr uintptr_t FRAME_REAL = 0x10;      // uint64, the real vtable, for a later run when this one did not put it back
    constexpr uintptr_t FRAME_MAGIC = 0x18;     // uint64
    constexpr uint64_t MAGIC = 0x5657455250474741;  // "AGGPREWV"
    constexpr uintptr_t JOB_NOTHING = 0x30;     // xor eax, eax; ret, what a cancelled job runs instead
    constexpr uintptr_t FRAME_STUB = 0x40;
    constexpr uintptr_t FRAME_TABLE = 0x200;    // The RTTI pointer, then the copy
    constexpr size_t MAX_ENTRIES = 0x300;

    // c the root panel of the context, r the panel ours go in: the highest one above it that has a size (the root of
    // the context can be a panel the layout gives none). Errors go to the console of the game
    std::string Wrap(const std::string& body) {
        return "(function(){var c=$.GetContextPanel();var r=c;"
            "for(var t=c;t;t=t.GetParent())if(t.actuallayoutwidth>0&&t.actuallayoutheight>0)r=t;"
            "try{" + body + "}catch(e){$.Msg('[cs2ext] agent preview error: '+e);}})();";
    }

    template <typename T>
    void SafeRelease(T*& object) {
        if (object) {
            object->Release();
            object = nullptr;
        }
    }

    // The screen of the monitor a point is on, through DXGI desktop duplication: what is shown, without the windows
    // left out of captures (the overlay while a picture is taken)
    class ScreenCapture {
    public:
        ~ScreenCapture() {
            SafeRelease(this->duplication);
            SafeRelease(this->context);
            SafeRelease(this->device);
        }

        std::string Open(POINT at) {
            IDXGIFactory1* factory = nullptr;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return "DXGI could not be used";

            std::string error = "the monitor of the game was not found";
            IDXGIAdapter1* adapter = nullptr;
            for (UINT a = 0; !this->duplication && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
                IDXGIOutput* output = nullptr;
                for (UINT o = 0; !this->duplication && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
                    DXGI_OUTPUT_DESC desc{};
                    if (SUCCEEDED(output->GetDesc(&desc)) && PtInRect(&desc.DesktopCoordinates, at)) {
                        this->desktop = desc.DesktopCoordinates;
                        IDXGIOutput1* output1 = nullptr;
                        if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &this->device, nullptr, &this->context)) &&
                            SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
                            HRESULT result = output1->DuplicateOutput(this->device, &this->duplication);
                            if (FAILED(result))
                                error = std::format("the screen could not be captured (0x{:X})", static_cast<uint32_t>(result));
                        }
                        SafeRelease(output1);
                    }
                    SafeRelease(output);
                }
                SafeRelease(adapter);
            }
            SafeRelease(factory);
            return this->duplication ? "" : error;
        }

        // BGRA pixels of a rect of the screen, as it is now (frames since the last one are dropped)
        bool Grab(RECT rect, std::vector<uint8_t>& pixels) {
            if (!this->duplication)
                return false;

            DXGI_OUTDUPL_FRAME_INFO info{};
            IDXGIResource* resource = nullptr;
            bool got = false;
            for (int attempt = 0; attempt < 8 && !got; attempt++) {
                HRESULT result = this->duplication->AcquireNextFrame(250, &info, &resource);
                if (result == DXGI_ERROR_WAIT_TIMEOUT)
                    continue;
                if (FAILED(result))
                    return false;

                // Only the mouse moved: the image is not new
                got = info.LastPresentTime.QuadPart != 0;
                if (!got) {
                    SafeRelease(resource);
                    this->duplication->ReleaseFrame();
                }
            }
            if (!got)
                return false;

            ID3D11Texture2D* frame = nullptr;
            bool done = false;
            if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&frame)))) {
                D3D11_TEXTURE2D_DESC desc{};
                frame->GetDesc(&desc);

                LONG left = rect.left - this->desktop.left, top = rect.top - this->desktop.top;
                LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
                bool inside = left >= 0 && top >= 0 && width > 0 && height > 0 &&
                    left + width <= static_cast<LONG>(desc.Width) && top + height <= static_cast<LONG>(desc.Height);

                if (inside && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    D3D11_TEXTURE2D_DESC staging_desc{};
                    staging_desc.Width = width;
                    staging_desc.Height = height;
                    staging_desc.MipLevels = 1;
                    staging_desc.ArraySize = 1;
                    staging_desc.Format = desc.Format;
                    staging_desc.SampleDesc.Count = 1;
                    staging_desc.Usage = D3D11_USAGE_STAGING;
                    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                    ID3D11Texture2D* staging = nullptr;
                    if (SUCCEEDED(this->device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
                        D3D11_BOX box{ static_cast<UINT>(left), static_cast<UINT>(top), 0,
                            static_cast<UINT>(left + width), static_cast<UINT>(top + height), 1 };
                        this->context->CopySubresourceRegion(staging, 0, 0, 0, 0, frame, 0, &box);

                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(this->context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                            pixels.resize(static_cast<size_t>(width) * height * 4);
                            for (LONG y = 0; y < height; y++)
                                std::memcpy(&pixels[static_cast<size_t>(y) * width * 4], static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 4);
                            this->context->Unmap(staging, 0);
                            done = true;
                        }
                        SafeRelease(staging);
                    }
                }
                SafeRelease(frame);
            }

            SafeRelease(resource);
            this->duplication->ReleaseFrame();
            return done;
        }

    private:
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGIOutputDuplication* duplication = nullptr;
        RECT desktop{};
    };

    // BGRA with straight alpha into a PNG file, through WIC
    bool SavePng(const std::filesystem::path& path, const std::vector<uint8_t>& pixels, UINT width, UINT height) {
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        bool saved = false;

        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;

        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height))) {
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            saved = SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
                SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()))) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }

        SafeRelease(frame);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        if (com)
            CoUninitialize();
        return saved;
    }

    // Pixels that are not the black background
    size_t Content(const std::vector<uint8_t>& pixels) {
        size_t count = 0;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            count += std::max({ pixels[i], pixels[i + 1], pixels[i + 2] }) > 24;
        return count;
    }
}

bool AgentPreview::Init() {
    return GetInstance().InitImpl();
}

bool AgentPreview::IsAvailable() {
    return Engine::IsInsecure() && pano::fnRunScript && pano::contextMap && pano::fnRunFrame;
}

std::string AgentPreview::PicturePath(bool terrorist) {
    return (std::filesystem::path("cache") / "agents" / (terrorist ? "agent_t_v2.png" : "agent_ct_v3.png")).string();
}

int AgentPreview::GetGeneration() {
    return GetInstance().generation;
}

bool AgentPreview::IsReady() {
    return GetInstance().ready;
}

std::string AgentPreview::GetProgress() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.progress;
}

float AgentPreview::GetPercent() {
    auto& i = GetInstance();
    return i.ready ? 1.f : i.percent.load();
}

void AgentPreview::SetStage(float stage) {
    // Only forward, also when a picture is taken again
    float now = std::clamp(this->stage_from + this->stage_span * stage, 0.f, 1.f);
    if (now > this->percent)
        this->percent = now;
}

void AgentPreview::Request(float x, float y, float width, float height, float screen_x, float screen_y) {
    auto& i = GetInstance();
    std::lock_guard lock(i.wanted_mutex);
    i.wanted = { floorf(x), floorf(y), floorf(width), floorf(height), floorf(screen_x), floorf(screen_y) };
    i.asked_at = std::chrono::steady_clock::now();
    // Counted from the first time it was asked: in a match it is not, until the main menu
    if (i.started == std::chrono::steady_clock::time_point{})
        i.started = i.asked_at;
}

bool AgentPreview::IsCapturing() {
    return GetInstance().capturing;
}

std::string AgentPreview::GetStatus() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.status;
}

bool AgentPreview::InitImpl() {
    std::error_code error;
    bool both = std::filesystem::exists(PicturePath(true), error) && std::filesystem::exists(PicturePath(false), error);
    if (both) {
        this->ready = true;
        return true;
    }

    // Without -insecure nothing can be taken: the menu opens, the preview without the agents
    if (!IsAvailable()) {
        this->ready = true;
        LOGF(WARNING, "The pictures of the agents of the ESP preview are taken once with the game started with -insecure");
        return false;
    }

    SetProgress("Waiting for the game");
    std::thread(&AgentPreview::Thread, this).detach();
    LOGF(INFO, "Taking the pictures of the agents of the ESP preview, the menu opens once they are there...");
    return true;
}

void AgentPreview::SetStatus(const std::string& text) {
    std::lock_guard lock(this->status_mutex);
    if (this->status != text && !text.empty())
        LOGF(WARNING, "Agent picture: {}", text);
    this->status = text;
}

void AgentPreview::SetProgress(const std::string& text) {
    std::lock_guard lock(this->status_mutex);
    if (this->progress != text && !text.empty())
        LOGF(INFO, "Agent pictures: {}", text);
    this->progress = text;
}

void AgentPreview::Thread() {
    while (!this->stopping && !this->ready) {
        {
            std::unique_lock lock(this->wanted_mutex);
            this->wake.wait_for(lock, POLL);
        }
        Update();
    }
}

bool AgentPreview::Asked(Wanted* want) {
    std::lock_guard lock(this->wanted_mutex);
    if (want)
        *want = this->wanted;
    return !this->stopping && std::chrono::steady_clock::now() - this->asked_at < ASKED_FOR;
}

std::vector<uintptr_t> AgentPreview::Roots() {
    std::vector<uintptr_t> roots;
    for (auto id : { "CSGOMainMenu", "CSGOHud" })
        if (auto root = FindRoot(id))
            roots.push_back(root);
    return roots;
}

void AgentPreview::Update() {
    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();

    // Done: both there, or each team out of attempts, or the game never got to a menu
    std::error_code error;
    bool missing[2] = { !std::filesystem::exists(PicturePath(true), error), !std::filesystem::exists(PicturePath(false), error) };
    bool left = (missing[0] && this->attempts[0] < MAX_ATTEMPTS) || (missing[1] && this->attempts[1] < MAX_ATTEMPTS);
    std::chrono::steady_clock::time_point first_asked;
    {
        std::lock_guard lock(this->wanted_mutex);
        first_asked = this->started;
    }
    bool given_up = first_asked != std::chrono::steady_clock::time_point{} && now - first_asked > GIVE_UP;
    if (!left || this->failures >= MAX_FAILURES || given_up || !p || !Engine::IsInsecure()) {
        if (missing[0] || missing[1])
            LOGF(WARNING, "Agent pictures: not all could be taken ({}), the preview shows none for that team", GetStatus());
        else
            LOGF(INFO, "Agent pictures: ready");
        SetProgress("");
        this->ready = true;
        RemoveFrameHook();
        return;
    }

    Wanted want;
    if (!Asked(&want))
        return;

    if (!this->engine) {
        // PanoramaUIEngine001, its AccessUIEngine: mov rax, [rcx + engine]; ret
        auto iface = p->FindInterface("panorama.dll", "PanoramaUIEngine001");
        auto access = iface ? p->read<uintptr_t>(p->read<uintptr_t>(iface) + pano::accessUIEngine * 8) : 0;
        uint8_t bytes[5]{};
        if (!access || !p->read_raw(access, bytes, sizeof(bytes)) || bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x41 || bytes[4] != 0xC3) {
            this->failures = MAX_FAILURES;
            SetStatus("the engine of Panorama was not found");
            return;
        }
        this->engine = p->read<uintptr_t>(iface + bytes[3]);
        if (!this->engine)
            return;     // Not made yet
    }

    // The enemies first, then the team
    bool terrorist = missing[0] && this->attempts[0] < MAX_ATTEMPTS;
    int team = terrorist ? 0 : 1;
    if (now - this->failed_at[team] < RETRY_AFTER)
        return;

    // The game in a menu or a match: the panels go in their roots
    if (Roots().empty()) {
        SetProgress("Waiting for the main menu");
        return;
    }

    int number = (missing[0] && missing[1]) ? (terrorist ? 1 : 2) : 1;
    int count = (missing[0] && missing[1]) ? 2 : 1;
    SetProgress(std::format("Loading {}/{}", number, count));

    this->stage_from = 0.05f + 0.95f * (number - 1) / count;
    this->stage_span = 0.95f / count;
    SetStage(0.f);

    this->capturing = true;
    bool taken = Capture(want, terrorist);

    // Ours go, also when it stopped half way
    auto roots = Roots();
    if (!roots.empty())
        Run(roots, Wrap(ForAll("p.DeleteAsync(0.0);")));
    this->capturing = false;

    if (taken) {
        this->generation++;
        SetStatus("");
        LOGF(INFO, "Agent picture of the {} taken: {}", terrorist ? "T" : "CT", PicturePath(terrorist));
    }
    else if (Asked()) {
        this->attempts[team]++;
        this->failed_at[team] = std::chrono::steady_clock::now();
    }
}

bool AgentPreview::Capture(const Wanted& want, bool terrorist) {
    const char* id = terrorist ? ID_T : ID_CT;
    const char* other = terrorist ? ID_CT : ID_T;
    const char* model = terrorist ? MODEL_T : MODEL_CT;
    const char* camera = terrorist ? "cam_loadoutmenu" : "cam_loadoutmenu_ct";

    // Where the panel is (pixels of the window): over the spot first, then bigger so the agent fills the spot
    float panel_x = want.x, panel_y = want.y, panel_width = want.width, panel_height = want.height;

    // The panel of the team: a plain panel over the spot only, on a background of that color, cutting what goes past
    // it (the loading box covers just the spot). The agent in it where it goes, bigger than it once zoomed. Pixels of
    // the window are pixels of the panel times the scale of the UI
    auto place = [&](const char* background) {
        auto roots = Roots();
        if (roots.empty())
            return false;
        return Run(roots, Wrap(std::format(
            "['{1}','cs2ext_agent_t','cs2ext_agent_ct'].forEach(function(n){{var o=r.FindChildTraverse(n);if(o)o.DeleteAsync(0.0);}});"
            "var p=r.FindChildTraverse('{0}');"
            "if(!p){{p=$.CreatePanel('Panel',r,'{0}',{{hittest:'false',hittestchildren:'false'}});"
            "$.CreatePanel('MapPlayerPreviewPanel',p,'{0}_model',{{map:'ui/buy_menu',camera:'{3}',"
            "'require-composition-layer':'true',playermodel:'{2}',playername:'vanity_character',"
            "animgraphcharactermode:'buy-menu',player:'true',mouse_rotate:'false',sync_spawn_addons:'true',"
            "'transparent-background':'true','pin-fov':'vertical',csm_split_plane0_distance_override:'250.0',"
            "hide_while_waiting_for_composite_materials:'false',hittest:'false'}});}}"
            "var a=p.FindChildTraverse('{0}_model');"
            "var sx=r.actualuiscale_x||1,sy=r.actualuiscale_y||1;"
            "p.style.position=({4}/sx)+'px '+({5}/sy)+'px 0px';"
            "p.style.width=({6}/sx)+'px';p.style.height=({7}/sy)+'px';p.style.overflow='clip clip';"
            "p.style.backgroundColor='{8}';p.style.zIndex='10000';p.visible=true;"
            "if(a){{a.style.position=({9}/sx)+'px '+({10}/sy)+'px 0px';"
            "a.style.width=({11}/sx)+'px';a.style.height=({12}/sy)+'px';}}",
            id, other, model, camera, floorf(want.x), floorf(want.y), floorf(want.width), floorf(want.height), background,
            floorf(panel_x - want.x), floorf(panel_y - want.y), floorf(panel_width), floorf(panel_height))));
    };

    // The spot on the screen, its edges left out
    const RECT rect{
        static_cast<LONG>(want.screen_x + want.x) + EDGE, static_cast<LONG>(want.screen_y + want.y) + EDGE,
        static_cast<LONG>(want.screen_x + want.x + want.width) - EDGE, static_cast<LONG>(want.screen_y + want.y + want.height) - EDGE };
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;

    ScreenCapture screen;
    auto opened = screen.Open(POINT{ rect.left, rect.top });
    if (!opened.empty()) {
        SetStatus(opened);
        this->failures++;
        return false;
    }

    // Still asked for, at the same spot (the card moves with the game window: started again then)
    auto still = [&]() {
        Wanted now;
        return Asked(&now) && now.x == want.x && now.y == want.y && now.width == want.width && now.height == want.height &&
            now.screen_x == want.screen_x && now.screen_y == want.screen_y;
    };

    // Loaded once the agent shows & stays about the same (the idle moves it a little). black: the last capture
    std::vector<uint8_t> black, white, black_after;
    auto settle = [&]() {
        size_t last = 0;
        int steady = 0;
        auto deadline = std::chrono::steady_clock::now() + LOAD_TIME;
        while (steady < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (!still())
                return false;
            if (std::chrono::steady_clock::now() > deadline) {
                SetStatus("the agent did not show in the panel of the game");
                return false;
            }
            if (!screen.Grab(rect, black))
                continue;

            size_t content = Content(black);
            size_t area = black.size() / 4;
            bool shows = content > area / 200;
            bool same = last && (content > last ? content - last : last - content) < area / 100;
            steady = shows && same ? steady + 1 : 0;
            last = content;
        }
        return true;
    };

    if (!place("#000000")) {
        SetStatus("no panel of the game to put the agent in yet");
        return false;
    }
    SetStage(0.1f);
    if (!settle())
        return false;
    SetStage(0.4f);

    // The camera of the loadout leaves a lot of room around the agent: the panel grows (beyond the spot, cut by it) until
    // the agent fills the spot, its feet a little above the bottom. pin-fov vertical: the agent grows with the panel
    int top = height, bottom = -1, left = width, right = -1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t* pixel = &black[(static_cast<size_t>(y) * width + x) * 4];
            if (std::max({ pixel[0], pixel[1], pixel[2] }) > 24) {
                top = std::min(top, y); bottom = std::max(bottom, y);
                left = std::min(left, x); right = std::max(right, x);
            }
        }
    }
    if (bottom > top) {
        float scale = std::min(4.f, height * 0.92f / static_cast<float>(bottom - top + 1));
        if (scale > 1.1f) {
            // Where the agent is in the panel, then where that goes
            float center = EDGE + (left + right) * 0.5f, head = EDGE + static_cast<float>(top);
            panel_width = want.width * scale;
            panel_height = want.height * scale;
            panel_x = want.x + want.width * 0.5f - center * scale;
            panel_y = want.y + EDGE + height * 0.04f - head * scale;
            if (!place("#000000") || !settle())
                return false;
            SetStage(0.7f);
        }
    }

    // Black, white, black again: the two blacks together are about when the white was taken, the idle moves it a little
    if (!screen.Grab(rect, black))
        return false;
    if (!place("#FFFFFF"))
        return false;
    std::this_thread::sleep_for(SETTLE);
    if (!still() || !screen.Grab(rect, white))
        return false;
    if (!place("#000000"))
        return false;
    std::this_thread::sleep_for(SETTLE);
    if (!still() || !screen.Grab(rect, black_after) || black.size() != white.size() || black_after.size() != white.size())
        return false;
    for (size_t i = 0; i < black.size(); i++)
        black[i] = static_cast<uint8_t>((black[i] + black_after[i] + 1) / 2);

    SetStage(0.9f);

    // Over black a pixel is color * alpha, over white color * alpha + (1 - alpha): the difference is 1 - alpha
    std::vector<uint8_t> picture(black.size());
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    size_t solid = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t i = (static_cast<size_t>(y) * width + x) * 4;
            int difference = (white[i] - black[i]) + (white[i + 1] - black[i + 1]) + (white[i + 2] - black[i + 2]);
            float alpha = std::clamp(1.f - difference / (3.f * 255.f), 0.f, 1.f);
            if (alpha < 2.f / 255.f)
                alpha = 0.f;

            for (int c = 0; c < 3; c++)
                picture[i + c] = alpha > 0.f ? static_cast<uint8_t>(std::min(255.f, black[i + c] / alpha + 0.5f)) : 0;
            picture[i + 3] = static_cast<uint8_t>(alpha * 255.f + 0.5f);

            if (picture[i + 3] > 8) {
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                solid += picture[i + 3] > 250;
            }
        }
    }

    // Nothing, or all of it solid: not the panel that was captured
    size_t area = static_cast<size_t>(width) * height;
    if (max_x < 0 || solid > area * 9 / 10) {
        SetStatus("the capture did not show the agent on its backgrounds");
        return false;
    }

    // Only the agent: the pieces that show are found, the biggest is the agent. Other pieces stay when they are big
    // enough & do not touch a side (a strip of the scene of the game at a side of the zoomed panel goes)
    {
        std::vector<int> piece(area, -1);
        struct Piece { size_t pixels = 0; bool side = false; };
        std::vector<Piece> pieces;
        std::vector<int> stack;
        for (int start = 0; start < static_cast<int>(area); start++) {
            if (piece[start] >= 0 || picture[static_cast<size_t>(start) * 4 + 3] <= 8)
                continue;
            int id = static_cast<int>(pieces.size());
            pieces.push_back({});
            piece[start] = id;
            stack.push_back(start);
            while (!stack.empty()) {
                int at = stack.back();
                stack.pop_back();
                int x = at % width, y = at / width;
                pieces[id].pixels++;
                if (x == 0 || x == width - 1)
                    pieces[id].side = true;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                            continue;
                        int next = ny * width + nx;
                        if (piece[next] < 0 && picture[static_cast<size_t>(next) * 4 + 3] > 8) {
                            piece[next] = id;
                            stack.push_back(next);
                        }
                    }
                }
            }
        }

        size_t biggest = 0;
        for (auto& p : pieces)
            biggest = std::max(biggest, p.pixels);

        min_x = width; min_y = height; max_x = -1; max_y = -1;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t at = static_cast<size_t>(y) * width + x;
                int id = piece[at];
                bool keep = id >= 0 && (pieces[id].pixels == biggest || (!pieces[id].side && pieces[id].pixels * 50 >= biggest));
                if (!keep) {
                    std::memset(&picture[at * 4], 0, 4);
                    continue;
                }
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
            }
        }
    }

    // Cut to the agent, a little room around
    min_x = std::max(0, min_x - 2); min_y = std::max(0, min_y - 2);
    max_x = std::min(width - 1, max_x + 2); max_y = std::min(height - 1, max_y + 2);
    int cut_width = max_x - min_x + 1, cut_height = max_y - min_y + 1;
    std::vector<uint8_t> cut(static_cast<size_t>(cut_width) * cut_height * 4);
    for (int y = 0; y < cut_height; y++)
        std::memcpy(&cut[static_cast<size_t>(y) * cut_width * 4], &picture[(static_cast<size_t>(min_y + y) * width + min_x) * 4], cut_width * 4);

    std::filesystem::path path = PicturePath(terrorist);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!SavePng(path, cut, cut_width, cut_height)) {
        SetStatus("the picture could not be saved to " + path.string());
        return false;
    }
    SetStage(1.f);
    return true;
}

void AgentPreview::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;
    i.wake.notify_one();
    std::this_thread::sleep_for(POLL * 2);

    if (Engine::GetProcess() && Engine::IsInsecure() && i.engine && i.frame_page) {
        auto roots = i.Roots();
        if (!roots.empty())
            i.Run(roots, Wrap(ForAll("p.DeleteAsync(0.0);")));
    }
    i.RemoveFrameHook();
}

std::vector<uintptr_t> AgentPreview::ContextPanels() {
    auto p = Engine::GetProcess();
    std::vector<uintptr_t> panels;

    // The JavaScript contexts by panel: a CUtlRBTree, nodes of 32 bytes { left, right, parent, tag, panel, context }
    auto map = this->engine + pano::contextMap;
    auto count = p->read<uint32_t>(map + 0xC) & 0x7FFFFFFF;
    auto nodes = p->read<uintptr_t>(map + 0x10);
    if (!nodes || count == 0 || count > 0x4000)
        return panels;

    std::vector<uint8_t> buffer(count * 0x20);
    if (!p->read_raw(nodes, buffer.data(), buffer.size()))
        return panels;

    for (uint32_t n = 0; n < count; n++) {
        auto node = &buffer[n * 0x20];
        int32_t left;
        uintptr_t panel;
        std::memcpy(&left, node, sizeof(left));
        std::memcpy(&panel, node + 0x10, sizeof(panel));
        if (left == static_cast<int32_t>(n) || panel < 0x10000)
            continue;   // Free
        panels.push_back(panel);
    }
    return panels;
}

bool AgentPreview::HasContext(uintptr_t panel) {
    auto panels = ContextPanels();
    return panel && std::find(panels.begin(), panels.end(), panel) != panels.end();
}

uintptr_t AgentPreview::FindRoot(const char* id) {
    auto p = Engine::GetProcess();
    size_t length = std::strlen(id);
    for (auto panel : ContextPanels()) {
        auto name = p->read<uintptr_t>(panel + pano::panelId);
        char text[32]{};
        if (!name || !p->read_raw(name, text, length + 1))
            continue;
        if (std::memcmp(text, id, length + 1) == 0)
            return panel;
    }
    return 0;
}

bool AgentPreview::InstallFrameHook() {
    auto p = Engine::GetProcess();
    auto panorama = p->GetModule("panorama.dll");
    auto run_frame = panorama.base + pano::fnRunFrame;
    auto in_module = [&](uintptr_t address) { return address >= panorama.base && address < panorama.base + panorama.size; };

    auto vtable = p->read<uintptr_t>(this->engine);

    // Still our copy from an earlier run that was closed without putting the real one back
    if (vtable && !in_module(vtable)) {
        auto old = vtable - FRAME_TABLE - sizeof(uintptr_t);
        auto real = p->read<uintptr_t>(old + FRAME_REAL);
        if (p->read<uint64_t>(old + FRAME_MAGIC) != MAGIC || !in_module(real)) {
            LOGF(WARNING, "Agent preview: the vtable of the Panorama engine is not the one of the game (0x{:X})", vtable);
            return false;
        }
        p->write<uintptr_t>(this->engine, real);
        vtable = real;
        LOGF(INFO, "The vtable of the Panorama engine was still ours from an earlier run");
    }

    // The entries, RunFrame among them
    std::vector<uintptr_t> entries;
    int frame_index = -1;
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        auto entry = p->read<uintptr_t>(vtable + i * sizeof(uintptr_t));
        if (!in_module(entry))
            break;
        if (entry == run_frame)
            frame_index = static_cast<int>(i);
        entries.push_back(entry);
    }
    if (frame_index < 0) {
        LOGF(WARNING, "Agent preview: RunFrame is not in the vtable of the Panorama engine ({} entries)", entries.size());
        return false;
    }

    auto page = p->allocate_remote(FRAME_PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!page)
        return false;

    std::vector<uint8_t> code(FRAME_PAGE_SIZE, 0xCC);
    auto put = [&](size_t& at, std::initializer_list<uint8_t> bytes) { for (auto b : bytes) code[at++] = b; };
    auto put32 = [&](size_t& at, uint32_t value) { for (int k = 0; k < 4; k++) code[at++] = static_cast<uint8_t>(value >> (k * 8)); };
    auto put64 = [&](size_t& at, uint64_t value) { for (int k = 0; k < 8; k++) code[at++] = static_cast<uint8_t>(value >> (k * 8)); };

    size_t at = JOB_STATE;
    put32(at, JOB_IDLE);
    at = FRAME_REAL;
    put64(at, vtable);
    at = FRAME_MAGIC;
    put64(at, MAGIC);
    at = JOB_NOTHING;
    put(at, { 0x31, 0xC0, 0xC3 });

    // RunFrame entry: a queued job first, then RunFrame of the game. Its arguments & xmm0 - xmm3 kept
    at = FRAME_STUB;
    put(at, { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51 });       // push rcx, rdx, r8, r9
    put(at, { 0x48, 0x83, 0xEC, 0x68 });                    // sub rsp, 0x68
    put(at, { 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20 });        // movdqu [rsp + 0x20], xmm0
    put(at, { 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30 });        // movdqu [rsp + 0x30], xmm1
    put(at, { 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40 });        // movdqu [rsp + 0x40], xmm2
    put(at, { 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 });        // movdqu [rsp + 0x50], xmm3
    put(at, { 0x49, 0xBB }); put64(at, page);               // mov r11, job
    put(at, { 0xB8 }); put32(at, JOB_QUEUED);               // mov eax, queued
    put(at, { 0xB9 }); put32(at, JOB_RUNNING);              // mov ecx, running
    put(at, { 0xF0, 0x41, 0x0F, 0xB1, 0x0B });              // lock cmpxchg [r11], ecx
    put(at, { 0x75 }); size_t skip_job = at++;              // jne done
    put(at, { 0x49, 0x8B, 0x43, static_cast<uint8_t>(JOB_FUNCTION) }); // mov rax, [r11 + function]
    put(at, { 0xFF, 0xD0 });                                // call rax
    put(at, { 0x49, 0xBB }); put64(at, page);               // mov r11, job
    put(at, { 0x41, 0xC7, 0x03 }); put32(at, JOB_DONE);     // mov dword ptr [r11], done
    code[skip_job] = static_cast<uint8_t>(at - (skip_job + 1));
    put(at, { 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20 });        // movdqu xmm0, [rsp + 0x20]
    put(at, { 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30 });        // movdqu xmm1, [rsp + 0x30]
    put(at, { 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40 });        // movdqu xmm2, [rsp + 0x40]
    put(at, { 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50 });        // movdqu xmm3, [rsp + 0x50]
    put(at, { 0x48, 0x83, 0xC4, 0x68 });                    // add rsp, 0x68
    put(at, { 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 });        // pop r9, r8, rdx, rcx
    put(at, { 0x48, 0xB8 }); put64(at, run_frame);          // mov rax, RunFrame
    put(at, { 0xFF, 0xE0 });                                // jmp rax
    if (at > FRAME_TABLE)
        return false;

    // The copy, with the RTTI pointer right before it like the real one
    at = FRAME_TABLE;
    put64(at, p->read<uintptr_t>(vtable - sizeof(uintptr_t)));
    size_t first = at;
    for (size_t i = 0; i < entries.size(); i++)
        put64(at, static_cast<int>(i) == frame_index ? page + FRAME_STUB : entries[i]);
    if (at > FRAME_PAGE_SIZE)
        return false;

    p->write_bytes(page, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(page), code.size());

    this->frame_page = page;
    this->real_vtable = vtable;
    this->copy_vtable = page + first;
    p->write<uintptr_t>(this->engine, this->copy_vtable);
    LOGF(INFO, "Agent preview calls run at the start of the frames of Panorama ({} entries, RunFrame {})", entries.size(), frame_index);
    return true;
}

void AgentPreview::RemoveFrameHook() {
    auto p = Engine::GetProcess();
    if (!p || !this->frame_page)
        return;

    // A queued call runs nothing, the page stays: the game might be inside the stub
    p->write<uintptr_t>(this->frame_page + JOB_FUNCTION, this->frame_page + JOB_NOTHING);
    if (p->read<uintptr_t>(this->engine) == this->copy_vtable)
        p->write<uintptr_t>(this->engine, this->real_vtable);
    this->frame_page = 0;
}

bool AgentPreview::CallInFrame(uintptr_t function, DWORD timeout_ms) {
    auto p = Engine::GetProcess();
    auto job = this->frame_page;

    // The game put its own vtable back
    if (p->read<uintptr_t>(this->engine) != this->copy_vtable) {
        LOGF(WARNING, "Agent preview: the vtable of the Panorama engine was replaced");
        this->frame_page = 0;
        return false;
    }

    auto wait = [&](bool idle_only) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        auto spin_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        while (true) {
            auto state = p->read<uint32_t>(job + JOB_STATE);
            if (state == JOB_DONE) {
                p->write<uint32_t>(job + JOB_STATE, JOB_IDLE);
                return true;
            }
            // A cancelled job that never ran: it runs nothing, ours takes its place
            if (state == JOB_IDLE || (idle_only && state == JOB_QUEUED))
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            // The menu waits on it while dragged: the first moments without a sleep (one of 1 ms is often 15 on Windows)
            if (std::chrono::steady_clock::now() < spin_until)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    if (!wait(true))
        return false;

    p->write<uintptr_t>(job + JOB_FUNCTION, function);
    p->write<uint32_t>(job + JOB_STATE, JOB_QUEUED);

    if (!wait(false)) {
        // Panorama does not run its frames (the game is loading): nothing runs later. The stub reads the function
        // only after taking the job, it either already runs with what is current or runs nothing
        p->write<uintptr_t>(job + JOB_FUNCTION, job + JOB_NOTHING);
        return false;
    }
    return true;
}

bool AgentPreview::Run(uintptr_t panel, const std::string& script) {
    return Run(std::vector<uintptr_t>{ panel }, script);
}

bool AgentPreview::Run(const std::vector<uintptr_t>& panels, const std::string& script) {
    static std::mutex running;     // The thread & Shutdown share the page
    std::lock_guard lock(running);

    auto p = Engine::GetProcess();
    if (script.size() + 1 > PAGE_SIZE - PAGE_SCRIPT)
        return false;
    if (!this->frame_page && !InstallFrameHook())
        return false;

    if (!this->page && !(this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE)))
        return false;

    auto run_script = p->GetModule("panorama.dll").base + pano::fnRunScript;

    // RunScript(engine, panel, script, origin, { line 1, column 0 }) for each panel, all in the same frame
    std::vector<uint8_t> code;
    auto put = [&](std::initializer_list<uint8_t> list) { code.insert(code.end(), list); };
    auto put64 = [&](uint64_t value) { for (int k = 0; k < 8; k++) code.push_back(static_cast<uint8_t>(value >> (k * 8))); };
    put({ 0x48, 0x83, 0xEC, 0x38 });                    // sub rsp, 0x38
    for (auto panel : panels) {
        put({ 0x48, 0xB9 }); put64(this->engine);       // mov rcx, engine
        put({ 0x48, 0xBA }); put64(panel);              // mov rdx, panel
        put({ 0x49, 0xB8 }); put64(this->page + PAGE_SCRIPT);   // mov r8, script
        put({ 0x49, 0xB9 }); put64(this->page + PAGE_ORIGIN);   // mov r9, origin
        put({ 0x48, 0xC7, 0x44, 0x24, 0x20, 1, 0, 0, 0 }); // mov qword [rsp + 0x20], 1
        put({ 0x48, 0xB8 }); put64(run_script);         // mov rax, RunScript
        put({ 0xFF, 0xD0 });                            // call rax
    }
    put({ 0x48, 0x83, 0xC4, 0x38 });                    // add rsp, 0x38
    put({ 0xC3 });                                      // ret
    if (panels.empty() || code.size() > PAGE_ORIGIN)
        return false;

    std::vector<uint8_t> data(PAGE_SCRIPT + script.size() + 1, 0);
    std::copy(code.begin(), code.end(), data.begin());
    std::memcpy(&data[PAGE_ORIGIN], ORIGIN, std::strlen(ORIGIN) + 1);
    std::memcpy(&data[PAGE_SCRIPT], script.c_str(), script.size() + 1);
    p->write_bytes(this->page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), data.size());

    if (!CallInFrame(this->page, 500)) {
        // The game might still be inside it, the page is left to it
        this->page = 0;
        return false;
    }
    return true;
}

