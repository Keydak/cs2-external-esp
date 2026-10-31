#include "hooks/Hooks.h"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <vector>

#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "core/Config.h"
#include "core/Journal.h"
#include "features/EmoteWheel.h"
#include "hooks/GameHooks.h"
#include "sdk/GameState.h"
#include "ui/Fonts.h"
#include "ui/ImageTexture.h"

namespace emotes::hooks {
namespace {

using PresentFn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn   = BOOL(WINAPI*)(const RECT*);

using SetRelativeMouseModeFn = void(__fastcall*)(void*, bool);
using MouseInputEnabledFn    = bool(__fastcall*)(void*);

PresentFn      g_originalPresent      = nullptr;
ResizeFn       g_originalResize       = nullptr;
SetCursorPosFn g_originalSetCursorPos = nullptr;
ClipCursorFn   g_originalClipCursor   = nullptr;
WNDPROC        g_originalWndProc      = nullptr;
HMODULE        g_selfModule           = nullptr;
HWND           g_gameWindow           = nullptr;

SetRelativeMouseModeFn g_origSetRelativeMouseMode = nullptr;
MouseInputEnabledFn    g_origMouseInputEnabled    = nullptr;
std::atomic<bool>      g_gameWantsLook{false};  // what the game last asked SetRelativeMouseMode for
std::atomic<bool>      g_lookKnown{false};      // whether it has asked since we hooked it

// Our own message to the game window: cursor changes run on the window thread, which owns the game's input system.
constexpr UINT WM_EMOTES_CURSOR = WM_APP + 0x6C8;

ID3D11Device*           g_device       = nullptr;
ID3D11DeviceContext*    g_context      = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;

// Backbuffer dimensions for stretched resolution support
UINT g_bbWidth  = 0;
UINT g_bbHeight = 0;

void* g_presentTarget      = nullptr;
void* g_resizeTarget       = nullptr;
void* g_setCursorPosTarget = nullptr;
void* g_clipCursorTarget   = nullptr;
void* g_srmTarget          = nullptr;
void* g_mieTarget          = nullptr;

constexpr int kPrologueSize = 16;
struct PrologueRecord { void* target; uint8_t bytes[kPrologueSize]; };
std::vector<PrologueRecord> g_prologues;
bool g_canUnload = true;

std::atomic<bool> g_unload{false};
std::atomic<bool> g_shuttingDown{false};
bool              g_initialized = false;
bool              g_gameHooksTried = false;
std::atomic<bool> g_wheelOpen{false};    // emote wheel: cursor free and no mouse look
std::atomic<float> g_scroll{0.0f};       // mouse wheel notches since the last frame, from the window procedure

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// While the wheel is open the game may not pin or move the cursor.
BOOL WINAPI HkSetCursorPos(int X, int Y) {
    if (g_wheelOpen) return TRUE;
    return g_originalSetCursorPos(X, Y);
}

BOOL WINAPI HkClipCursor(const RECT* lpRect) {
    if (g_wheelOpen) return g_originalClipCursor(nullptr);
    return g_originalClipCursor(lpRect);
}

// CInputSystem::SetRelativeMouseMode, slot 76 of InputSystemVersion001, is the game's one switch between mouse look
// (cursor held and hidden) and a free cursor. We remember what it asks for and hold look off while the wheel is open.
void __fastcall HkSetRelativeMouseMode(void* self, bool look) {
    g_gameWantsLook = look;
    g_lookKnown = true;
    g_origSetRelativeMouseMode(self, look && !g_wheelOpen);
}

// CCSGOInput::MouseInputEnabled: no mouse look or shooting while the wheel is open.
bool __fastcall HkMouseInputEnabled(void* rcx) {
    if (g_wheelOpen) return false;
    return g_origMouseInputEnabled(rcx);
}

// Window thread only. Frees the cursor while the wheel is open, else whatever the game last asked for (before it
// has asked, mouse look when in a match).
void ApplyCursor() {
    auto&      data = sdk::Data();
    const bool look = g_lookKnown ? g_gameWantsLook.load() : sdk::IsInGame() && sdk::GetLocalController();
    if (g_origSetRelativeMouseMode && data.inputSystem) g_origSetRelativeMouseMode(data.inputSystem, look && !g_wheelOpen);
    if (g_wheelOpen) {
        if (g_originalClipCursor) g_originalClipCursor(nullptr);
        SetCursor(nullptr);  // the wheel draws its own pointer
    } else if (look) {
        SetCursor(nullptr);
    }
}

void InstallCursorHooks() {
    auto& data = sdk::Data();
    if (data.inputSystem) {
        auto vtable = *reinterpret_cast<void***>(data.inputSystem);
        if (vtable && vtable[76] &&
            MH_CreateHook(vtable[76], &HkSetRelativeMouseMode, reinterpret_cast<void**>(&g_origSetRelativeMouseMode)) == MH_OK) {
            MH_EnableHook(vtable[76]);
            g_srmTarget = vtable[76];
            RecordPrologue(vtable[76]);
        }
    }
    if (data.fnMouseInputEnabled) {
        void* mieAddr = reinterpret_cast<void*>(data.fnMouseInputEnabled);
        if (MH_CreateHook(mieAddr, &HkMouseInputEnabled, reinterpret_cast<void**>(&g_origMouseInputEnabled)) == MH_OK) {
            MH_EnableHook(mieAddr);
            g_mieTarget = mieAddr;
            RecordPrologue(mieAddr);
        }
    }
    if (!g_srmTarget || !g_mieTarget) journal().warn("hooks", "some cursor hooks failed: the wheel may not free the mouse");
}

bool RemoveCursorHooks() {
    bool ok = true;
    if (g_srmTarget) ok &= SafeRemoveHook(g_srmTarget);
    if (g_mieTarget) ok &= SafeRemoveHook(g_mieTarget);
    return ok;
}

void CreateRenderTarget(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        D3D11_TEXTURE2D_DESC td;
        backBuffer->GetDesc(&td);
        g_bbWidth  = td.Width;
        g_bbHeight = td.Height;
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void ReleaseRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

// On the render thread's first frame: what needs the game's device and window.
void InitImGui(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_gameWindow = desc.OutputWindow;

    swapChain->GetDevice(IID_PPV_ARGS(&g_device));
    g_device->GetImmediateContext(&g_context);
    CreateRenderTarget(swapChain);

    ImGui_ImplWin32_Init(g_gameWindow);
    ImGui_ImplDX11_Init(g_device, g_context);
    ui::SetTextureDevice(g_device);

    sdk::Data().gameWindow = g_gameWindow;

    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    g_initialized = true;
}

// The game's window thread. The wheel takes clicks and the mouse wheel while it is open.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_shuttingDown.load()) return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);

    if (msg == WM_EMOTES_CURSOR) {
        ApplyCursor();
        return 0;
    }

    // The emote wheel keeps clicks and the mouse wheel (it pages with the wheel): no shots, no weapon switch.
    if (g_wheelOpen) {
        // Esc closes the emote wheel and never reaches the game (it would open the pause menu)
        if (wParam == VK_ESCAPE && (msg == WM_KEYDOWN || msg == WM_KEYUP)) {
            if (msg == WM_KEYDOWN) EmoteWheelCancel();
            return 0;
        }
        switch (msg) {
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {  // the wheel draws its own pointer
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_MOUSEWHEEL:
                g_scroll = g_scroll.load() + static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
                return TRUE;
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            case WM_MOUSEHWHEEL:
                return TRUE;
            // Releases still reach the game so a button held when the wheel opened is not left stuck down, but without
            // the other buttons in wParam: the game (SDL) takes each one set there as held.
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
                return CallWindowProcW(g_originalWndProc, hwnd, msg,
                                       wParam & ~static_cast<WPARAM>(MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2),
                                       lParam);
        }
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (g_shuttingDown.load()) return g_originalPresent(swapChain, syncInterval, flags);

    if (!g_initialized) InitImGui(swapChain);
    if (!g_gameHooksTried) {  // on the render thread, like the rest of the game hooks' first calls
        g_gameHooksTried = true;
        if (sdk::Data().initialized) {
            InstallGameHooks();
            InstallCursorHooks();
            journal().info("hooks", "ready");
        }
    }

    // If the game puts its own window procedure back, take the window again.
    if (g_originalWndProc &&
        GetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(g_originalWndProc))
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));

    // The unload key, on its first press.
    {
        static bool s_down = false;
        const int key = GetConfig().unloadKey;
        const bool down = key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !s_down && GetForegroundWindow() == g_gameWindow) g_unload = true;
        s_down = down;
    }

    ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDSV = nullptr;
    if (g_context) g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (g_bbWidth > 0 && g_bbHeight > 0) io.DisplaySize = ImVec2(static_cast<float>(g_bbWidth), static_cast<float>(g_bbHeight));
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
    ImGui::NewFrame();

    EmoteWheelDraw(ImGui::GetForegroundDrawList(), g_scroll.exchange(0.0f));
    if (const bool wheel = EmoteWheelWantsCursor(); wheel != g_wheelOpen.load()) {
        g_wheelOpen = wheel;
        if (g_gameWindow) PostMessageW(g_gameWindow, WM_EMOTES_CURSOR, 0, 0);
    }

    ImGui::Render();
    if (g_renderTarget) {
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    if (g_context) {
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
        for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
        if (savedDSV) savedDSV->Release();
    }
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                          DXGI_FORMAT format, UINT flags) {
    if (g_shuttingDown.load()) return g_originalResize(swapChain, bufferCount, width, height, format, flags);
    ReleaseRenderTarget();
    HRESULT hr = g_originalResize(swapChain, bufferCount, width, height, format, flags);
    CreateRenderTarget(swapChain);
    return hr;
}

}  // namespace

void Initialize(HMODULE selfModule) {
    g_selfModule = selfModule;
    // What needs neither the game's device nor its render thread, here on the loader thread so none of it holds a
    // frame up: signatures, schema and interfaces (the slow part), the config and the fonts.
    if (!sdk::Initialize()) journal().warn("hooks", "the game's modules did not load");
    LoadConfig(selfModule);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::LoadFonts(DataDir());

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr,
                   nullptr, nullptr, L"EmotesDummy", nullptr};
    RegisterClassExW(&wc);
    HWND dummyWnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                    wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = dummyWnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*      dummySwap    = nullptr;
    ID3D11Device*        dummyDevice  = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                  &dummySwap, &dummyDevice, nullptr, &dummyContext);
    if (!dummySwap) {
        journal().warn("hooks", "could not create a D3D11 device to find Present");
        if (dummyWnd) DestroyWindow(dummyWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_unload = true;
        return;
    }

    void** vtable = *reinterpret_cast<void***>(dummySwap);
    void*  presentAddr = vtable[8];
    void*  resizeAddr  = vtable[13];

    dummySwap->Release();
    dummyDevice->Release();
    dummyContext->Release();
    DestroyWindow(dummyWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    MH_Initialize();
    MH_CreateHook(presentAddr, &HkPresent, reinterpret_cast<void**>(&g_originalPresent));
    MH_CreateHook(resizeAddr, &HkResizeBuffers, reinterpret_cast<void**>(&g_originalResize));

    auto setCursorPosAddr = GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursorPos");
    auto clipCursorAddr   = GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor");
    if (setCursorPosAddr)
        MH_CreateHook(setCursorPosAddr, &HkSetCursorPos, reinterpret_cast<void**>(&g_originalSetCursorPos));
    if (clipCursorAddr)
        MH_CreateHook(clipCursorAddr, &HkClipCursor, reinterpret_cast<void**>(&g_originalClipCursor));

    g_presentTarget      = presentAddr;
    g_resizeTarget       = resizeAddr;
    g_setCursorPosTarget = setCursorPosAddr;
    g_clipCursorTarget   = clipCursorAddr;

    MH_EnableHook(MH_ALL_HOOKS);

    RecordPrologue(presentAddr);
    RecordPrologue(resizeAddr);
    RecordPrologue(setCursorPosAddr);
    RecordPrologue(clipCursorAddr);
}

void Shutdown() {
    g_shuttingDown.store(true);
    const bool wheelWasOpen = g_wheelOpen.exchange(false);
    Sleep(200);

    // Hand the game its cursor back, on the window thread while our hooks are still in.
    if (g_initialized && g_gameWindow && wheelWasOpen) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(g_gameWindow, WM_EMOTES_CURSOR, 0, 0, SMTO_ABORTIFHUNG, 1000, &ignored);
    }

    bool allRemoved = RemoveCursorHooks();
    allRemoved &= RemoveGameHooks();
    allRemoved &= SafeRemoveHook(g_presentTarget);
    allRemoved &= SafeRemoveHook(g_resizeTarget);
    allRemoved &= SafeRemoveHook(g_setCursorPosTarget);
    allRemoved &= SafeRemoveHook(g_clipCursorTarget);

    if (allRemoved) MH_Uninitialize();

    if (g_initialized && g_gameWindow && g_originalWndProc) {
        if (GetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(WndProc))
            SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        else
            allRemoved = false;
    }
    g_canUnload = allRemoved;

    sdk::Shutdown();
    EmoteWheelReleaseTextures();
    ui::SetTextureDevice(nullptr);
    if (g_initialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (g_initialized) {
        ReleaseRenderTarget();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device)  { g_device->Release();  g_device  = nullptr; }
    }
    journal().info("hooks", allRemoved ? "unloaded" : "hooks changed by something else; staying loaded");
}

bool ShouldUnload() { return g_unload.load(); }

void RecordPrologue(void* target) {
    if (!target) return;
    PrologueRecord r{target, {}};
    memcpy(r.bytes, target, kPrologueSize);
    g_prologues.push_back(r);
}

// Another hook on top of ours (the prologue changed since we hooked): leave ours in rather than break theirs.
bool SafeRemoveHook(void* target) {
    if (!target) return true;
    for (const auto& r : g_prologues)
        if (r.target == target && memcmp(target, r.bytes, kPrologueSize) != 0) return false;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    return true;
}

bool CanUnload() { return g_canUnload; }

}  // namespace emotes::hooks
