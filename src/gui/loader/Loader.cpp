#include "Loader.hpp"

#include "core/engine/Engine.hpp"
#include "core/features/Skins.hpp"
#include "gui/frontend/images/ImageCache.hpp"
#include "updater/Updater.hpp"
#include "assets/fonts/Inter.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <imgui/backends/imgui_impl_dx11.h>
#include <imgui/backends/imgui_impl_win32.h>

#include <cmath>
#include <numbers>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
#ifdef _DEBUG
    constexpr const char* TITLE = "Cs2 External";
    constexpr const char* SUBTITLE = "Counter-Strike 2 [DEV]";
#else
    constexpr const char* TITLE = "Cs2 External";
    constexpr const char* SUBTITLE = "Counter-Strike 2";
#endif

    // Size of the window before the scale of the screen, the title bar moves it
    constexpr float WIDTH = 400.f, HEIGHT = 620.f, TITLE_HEIGHT = 72.f, CLOSE_WIDTH = 84.f;

    constexpr ImU32 BACKGROUND = IM_COL32(8, 8, 10, 255);      // Amoled, like the menu
    constexpr ImU32 PANEL = IM_COL32(19, 19, 22, 255);
    constexpr ImU32 PANEL_HOVER = IM_COL32(26, 26, 30, 255);
    constexpr ImU32 BORDER = IM_COL32(34, 34, 40, 255);
    constexpr ImU32 TRACK = IM_COL32(32, 32, 38, 255);
    constexpr ImU32 TEXT = IM_COL32(237, 237, 240, 255);
    constexpr ImU32 DIM = IM_COL32(154, 154, 165, 255);
    constexpr ImU32 FAINT = IM_COL32(99, 99, 109, 255);
    constexpr ImU32 ACCENT = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 GOOD = IM_COL32(80, 200, 120, 255);
    constexpr ImU32 WARN = IM_COL32(240, 180, 60, 255);
    constexpr ImU32 BAD = IM_COL32(235, 80, 80, 255);

    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    bool imgui_win32 = false, imgui_dx11 = false;

    float scale = 1.f;
    bool closed = false;
    ImFont* font_regular = nullptr;
    ImFont* font_bold = nullptr;

    float S(float value) { return value * scale; }

    // The folder of the program's files (configs, cache, sounds, maps): next to the .exe unless another was picked. The
    // pick is kept in a file next to the .exe, the program works in the folder from Start on
    constexpr auto FOLDER_FILE = "folder.txt";
    std::filesystem::path folder;
    std::string folder_error;

    std::filesystem::path ExeFolder() {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    void LoadFolder() {
        folder = ExeFolder();
        std::ifstream file(ExeFolder() / FOLDER_FILE, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        if (!text.empty())
            folder = std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // Next to the .exe again: the file goes
    void SaveFolder() {
        std::error_code error;
        auto file = ExeFolder() / FOLDER_FILE;
        if (std::filesystem::equivalent(folder, ExeFolder(), error)) {
            std::filesystem::remove(file, error);
            return;
        }
        auto text = folder.u8string();
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size()));
    }

    // The folder picker of Windows, starting in the folder
    void PickFolder() {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(L"The folder of the program's files");

            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }

            IShellItem* picked = nullptr;
            if (SUCCEEDED(dialog->Show(hwnd)) && SUCCEEDED(dialog->GetResult(&picked))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    folder = path;
                    folder_error.clear();
                    SaveFolder();
                    CoTaskMemFree(path);
                }
                picked->Release();
            }
            dialog->Release();
        }
        if (SUCCEEDED(com))
            CoUninitialize();
    }

    // Made when missing, written to once to see it can be: the program works in it from then on
    bool UseFolder() {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        auto test = folder / ".write_test";
        {
            std::ofstream out(test, std::ios::binary | std::ios::trunc);
            out << "ok";
            if (!out.good()) {
                folder_error = "This folder cannot be written to, pick another";
                return false;
            }
        }
        std::filesystem::remove(test, error);
        if (!SetCurrentDirectoryW(folder.c_str())) {
            folder_error = "This folder cannot be used, pick another";
            return false;
        }
        auto text = folder.u8string();
        LOGF(INFO, "Working in {}", std::string(text.begin(), text.end()));
        return true;
    }

    // The version against GitHub, checked when the window opens & on Retry
    enum class Version { Checking, Latest, Newer, Unsafe, Offline };
    std::atomic<Version> version = Version::Checking;

    void CheckVersion() {
        version = Version::Checking;
        std::thread([] {
            bool reached = Updater::Init();
            auto status = Updater::GetStatus();
            if (!reached)
                version = Version::Offline;
            else if (status.unsafe)
                version = Version::Unsafe;
            else if (status.version_current > Updater::GetVersion())
                version = Version::Newer;
            else
                version = Version::Latest;
        }).detach();
    }

    // After Start: the game found & read, then the pictures of the items (the list of the items comes with them)
    enum class Step { Pending, Running, Done, Warning, Failed };
    std::atomic<bool> started = false;
    std::atomic<Step> game = Step::Pending;
    std::atomic<bool> finished = false;

    void Work() {
        // The items need no game: they load while it is found
        Skins::FetchList();
        ImageCache::StartPrefetch();

        game = Step::Running;
        if (!Engine::Attach()) {
            game = Step::Failed;
            return;
        }
        game = Engine::IsInsecure() ? Step::Done : Step::Warning;

        while (!ImageCache::IsPrefetched())
            std::this_thread::sleep_for(50ms);
        finished = true;
    }

    LRESULT CALLBACK Procedure(HWND window, UINT msg, WPARAM w, LPARAM l) {
        if (ImGui_ImplWin32_WndProcHandler(window, msg, w, l))
            return true;

        switch (msg) {
        case WM_NCHITTEST: {
            // The title bar moves the window, its buttons stay buttons
            POINT point{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(window, &point);
            RECT rect{};
            GetClientRect(window, &rect);
            if (point.y >= 0 && point.y < S(TITLE_HEIGHT) && point.x < rect.right - S(CLOSE_WIDTH))
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_CLOSE:
            closed = true;
            return 0;
        }
        return DefWindowProcA(window, msg, w, l);
    }

    void Destroy() {
        if (imgui_dx11)
            ImGui_ImplDX11_Shutdown();
        if (imgui_win32)
            ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        imgui_dx11 = imgui_win32 = false;

        if (target) target->Release();
        if (swap_chain) swap_chain->Release();
        if (context) context->Release();
        if (device) device->Release();
        target = nullptr; swap_chain = nullptr; context = nullptr; device = nullptr;

        if (hwnd) {
            DestroyWindow(hwnd);
            UnregisterClassA("cs2ext_loader", GetModuleHandleA(nullptr));
            hwnd = nullptr;
        }
    }

    bool Create() {
        ImGui_ImplWin32_EnableDpiAwareness();

        // In the middle of the screen the mouse is on, in its scale
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        scale = std::max(1.f, ImGui_ImplWin32_GetDpiScaleForMonitor(monitor));
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoA(monitor, &info);
        int width = static_cast<int>(S(WIDTH)), height = static_cast<int>(S(HEIGHT));
        int x = (info.rcWork.left + info.rcWork.right - width) / 2;
        int y = (info.rcWork.top + info.rcWork.bottom - height) / 2;

        WNDCLASSEXA wc{ sizeof(wc) };
        wc.lpfnWndProc = Procedure;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "cs2ext_loader";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        RegisterClassExA(&wc);

        hwnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, TITLE, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
            x, y, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd)
            return false;

        // Round corners & a dark frame on Windows 11, nothing on older ones
        DWORD corners = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners));
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &swap_chain, &device, nullptr, &context);
        if (result == DXGI_ERROR_UNSUPPORTED)
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                D3D11_SDK_VERSION, &sd, &swap_chain, &device, nullptr, &context);
        if (FAILED(result))
            return false;

        ID3D11Texture2D* back = nullptr;
        swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
        if (!back)
            return false;
        device->CreateRenderTargetView(back, nullptr, &target);
        back->Release();
        if (!target)
            return false;

        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;

        // The fonts of the menu, kept by us
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        font_regular = io.Fonts->AddFontFromMemoryTTF(inter_medium_font, static_cast<int>(inter_medium_font_len), S(15.f), &config);
        font_bold = io.Fonts->AddFontFromMemoryTTF(inter_bold_font, static_cast<int>(inter_bold_font_len), S(15.f), &config);
        if (!font_regular || !font_bold)
            return false;

        imgui_win32 = ImGui_ImplWin32_Init(hwnd);
        imgui_dx11 = imgui_win32 && ImGui_ImplDX11_Init(device, context);
        if (!imgui_dx11)
            return false;

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetForegroundWindow(hwnd);
        return true;
    }

    // Text with its top left there
    void Text(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text) {
        d->AddText(font, S(size), ImVec2(floorf(at.x), floorf(at.y)), color, text);
    }

    float TextWidth(ImFont* font, float size, const char* text) {
        return font->CalcTextSizeA(S(size), FLT_MAX, 0.f, text).x;
    }

    // A button drawn by us: primary is filled in the accent
    bool Button(ImDrawList* d, const char* id, ImVec2 min, ImVec2 size, const char* label, bool enabled, bool primary) {
        ImGui::SetCursorScreenPos(min);
        bool pressed = ImGui::InvisibleButton(id, size) && enabled;
        bool hovered = enabled && ImGui::IsItemHovered();
        bool held = enabled && ImGui::IsItemActive();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        auto max = min + size;
        ImU32 fill = primary ? (enabled ? (held ? IM_COL32(205, 205, 210, 255) : hovered ? IM_COL32(230, 230, 235, 255) : ACCENT) : TRACK)
            : (hovered ? PANEL_HOVER : PANEL);
        d->AddRectFilled(min, max, fill, S(8.f));
        if (!primary)
            d->AddRect(min, max, BORDER, S(8.f));

        ImU32 color = primary ? (enabled ? IM_COL32(8, 8, 10, 255) : FAINT) : (enabled ? TEXT : FAINT);
        float width = TextWidth(font_bold, 14.f, label);
        Text(d, font_bold, 14.f, ImVec2(min.x + (size.x - width) * 0.5f, min.y + (size.y - S(14.f)) * 0.5f - S(1.f)), color, label);
        return pressed;
    }

    // Where a step is: a ring while waiting, turning while it runs, a check, a ! or a cross once it is done
    void StepIcon(ImDrawList* d, ImVec2 center, Step step, float time) {
        const float radius = S(9.f);
        const float pi = std::numbers::pi_v<float>;
        switch (step) {
        case Step::Pending:
            d->AddCircle(center, radius, FAINT, 24, S(1.5f));
            break;
        case Step::Running:
            d->AddCircle(center, radius, TRACK, 24, S(2.f));
            d->PathArcTo(center, radius, time * 5.f, time * 5.f + pi * 0.6f, 16);
            d->PathStroke(ACCENT, 0, S(2.f));
            break;
        case Step::Done:
            d->AddCircleFilled(center, radius, GOOD, 24);
            d->AddLine(center + ImVec2(-S(4.f), 0.f), center + ImVec2(-S(1.f), S(3.f)), BACKGROUND, S(2.f));
            d->AddLine(center + ImVec2(-S(1.f), S(3.f)), center + ImVec2(S(4.5f), -S(3.f)), BACKGROUND, S(2.f));
            break;
        case Step::Warning:
            d->AddCircleFilled(center, radius, WARN, 24);
            d->AddLine(center + ImVec2(0.f, -S(4.5f)), center + ImVec2(0.f, S(1.f)), BACKGROUND, S(2.f));
            d->AddCircleFilled(center + ImVec2(0.f, S(4.f)), S(1.2f), BACKGROUND);
            break;
        case Step::Failed:
            d->AddCircleFilled(center, radius, BAD, 24);
            d->AddLine(center + ImVec2(-S(3.5f), -S(3.5f)), center + ImVec2(S(3.5f), S(3.5f)), BACKGROUND, S(2.f));
            d->AddLine(center + ImVec2(S(3.5f), -S(3.5f)), center + ImVec2(-S(3.5f), S(3.5f)), BACKGROUND, S(2.f));
            break;
        }
    }

    void Draw() {
        auto& io = ImGui::GetIO();
        float time = static_cast<float>(ImGui::GetTime());

        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##loader", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);

        auto d = ImGui::GetWindowDrawList();
        const float w = io.DisplaySize.x, h = io.DisplaySize.y;
        const float margin = S(24.f);
        d->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(w, h), BACKGROUND);
        d->AddRect(ImVec2(0.f, 0.f), ImVec2(w, h), BORDER);

        // The name, minimize & close at the right
        Text(d, font_bold, 19.f, ImVec2(margin, S(20.f)), TEXT, TITLE);
        Text(d, font_regular, 12.f, ImVec2(margin, S(44.f)), FAINT, SUBTITLE);

        const float button = S(30.f);
        auto close_min = ImVec2(w - margin - button + S(6.f), S(20.f));
        ImGui::SetCursorScreenPos(close_min);
        if (ImGui::InvisibleButton("##close", ImVec2(button, button)))
            closed = true;
        if (ImGui::IsItemHovered())
            d->AddRectFilled(close_min, close_min + ImVec2(button, button), PANEL_HOVER, S(6.f));
        auto close_center = close_min + ImVec2(button, button) * 0.5f;
        d->AddLine(close_center - ImVec2(S(5.f), S(5.f)), close_center + ImVec2(S(5.f), S(5.f)), DIM, S(1.5f));
        d->AddLine(close_center + ImVec2(S(5.f), -S(5.f)), close_center + ImVec2(-S(5.f), S(5.f)), DIM, S(1.5f));

        auto minimize_min = close_min - ImVec2(button + S(4.f), 0.f);
        ImGui::SetCursorScreenPos(minimize_min);
        if (ImGui::InvisibleButton("##minimize", ImVec2(button, button)))
            ShowWindow(hwnd, SW_MINIMIZE);
        if (ImGui::IsItemHovered())
            d->AddRectFilled(minimize_min, minimize_min + ImVec2(button, button), PANEL_HOVER, S(6.f));
        auto minimize_center = minimize_min + ImVec2(button, button) * 0.5f;
        d->AddLine(minimize_center - ImVec2(S(5.f), 0.f), minimize_center + ImVec2(S(5.f), 0.f), DIM, S(1.5f));

        d->AddLine(ImVec2(0.f, S(TITLE_HEIGHT)), ImVec2(w, S(TITLE_HEIGHT)), BORDER);

        // The version: what this is & what GitHub says, a button when something can be done about it
        Version state = version;
        auto card_min = ImVec2(margin, S(88.f)), card_max = ImVec2(w - margin, S(152.f));
        d->AddRectFilled(card_min, card_max, PANEL, S(10.f));
        d->AddRect(card_min, card_max, BORDER, S(10.f));

        // Written by the check while it runs
        auto status = state != Version::Checking ? Updater::GetStatus() : Status{};
        auto title = std::format("Version {}", Updater::GetVersion());
        Text(d, font_bold, 14.f, card_min + ImVec2(S(16.f), S(14.f)), TEXT, title.c_str());

        std::string line;
        ImU32 line_color = DIM;
        switch (state) {
        case Version::Checking: line = "Checking for updates..."; break;
        case Version::Latest: line = "Up to date"; line_color = GOOD; break;
        case Version::Newer: line = std::format("Version {} is out, update to use it", status.version_current); line_color = WARN; break;
        case Version::Unsafe: line = "This version is marked unsafe"; line_color = BAD; break;
        case Version::Offline: line = "Could not reach GitHub to check"; line_color = BAD; break;
        }
        Text(d, font_regular, 12.f, card_min + ImVec2(S(16.f), S(36.f)), line_color, line.c_str());

        if (state == Version::Newer || state == Version::Offline) {
            const char* label = state == Version::Newer ? "Download" : "Retry";
            auto size = ImVec2(TextWidth(font_bold, 14.f, label) + S(28.f), S(32.f));
            auto at = ImVec2(card_max.x - S(16.f) - size.x, (card_min.y + card_max.y - size.y) * 0.5f);
            if (Button(d, "##version_action", at, size, label, true, false)) {
                if (state == Version::Newer)
                    ShellExecuteA(nullptr, "open", (Updater::GetProjectUrl() + "/releases").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                else
                    CheckVersion();
            }
        }
        else if (state == Version::Checking) {
            auto center = ImVec2(card_max.x - S(30.f), (card_min.y + card_max.y) * 0.5f);
            StepIcon(d, center, Step::Running, time);
        }

        // The folder: where it is, Change while not started
        {
            auto folder_min = ImVec2(margin, S(164.f)), folder_max = ImVec2(w - margin, S(216.f));
            d->AddRectFilled(folder_min, folder_max, PANEL, S(10.f));
            d->AddRect(folder_min, folder_max, folder_error.empty() ? BORDER : BAD, S(10.f));
            Text(d, font_bold, 11.f, folder_min + ImVec2(S(16.f), S(10.f)), FAINT, "FOLDER");

            const char* label = "Change";
            auto size = ImVec2(TextWidth(font_bold, 14.f, label) + S(28.f), S(32.f));
            auto at = ImVec2(folder_max.x - S(10.f) - size.x, (folder_min.y + folder_max.y - size.y) * 0.5f);
            if (Button(d, "##folder", at, size, label, !started, false))
                PickFolder();

            // The end of a long path shows, its start cut
            auto text = folder_error.empty() ? std::string(reinterpret_cast<const char*>(folder.u8string().c_str())) : folder_error;
            float room = at.x - S(12.f) - (folder_min.x + S(16.f));
            if (folder_error.empty() && TextWidth(font_regular, 12.f, text.c_str()) > room) {
                while (text.size() > 4 && TextWidth(font_regular, 12.f, ("..." + text).c_str()) > room)
                    text.erase(0, 1);
                text = "..." + text;
            }
            auto text_min = folder_min + ImVec2(S(16.f), S(27.f));
            ImGui::SetCursorScreenPos(text_min);
            ImGui::InvisibleButton("##folder_path", ImVec2(std::max(1.f, room), S(16.f)));
            if (ImGui::IsItemHovered() && folder_error.empty())
                ImGui::SetTooltip("%s", reinterpret_cast<const char*>(folder.u8string().c_str()));
            Text(d, font_regular, 12.f, text_min, folder_error.empty() ? DIM : BAD, text.c_str());
        }

        // The checks of Start
        Text(d, font_bold, 11.f, ImVec2(margin, S(236.f)), FAINT, "CHECKS");

        struct Row {
            const char* name;
            Step step;
            std::string detail;
        };

        Step game_step = game;
        std::string game_detail = "Waiting for Start";
        if (game_step == Step::Running)
            game_detail = Engine::GetProgress();
        else if (game_step == Step::Done)
            game_detail = "Found, launched with -insecure";
        else if (game_step == Step::Warning)
            game_detail = "Found, without -insecure: only the reading features";
        else if (game_step == Step::Failed)
            game_detail = Engine::GetProgress();

        Step list_step = !started ? Step::Pending
            : Skins::IsLoaded() ? Step::Done
            : Skins::HasFailed() ? Step::Warning : Step::Running;
        std::string list_detail = list_step == Step::Pending ? "Waiting for Start"
            : list_step == Step::Done ? std::format("{} weapons, {} agents, {} music kits", Skins::GetItems().size(), Skins::GetAgents().size(), Skins::GetMusicKits().size())
            : list_step == Step::Warning ? "Could not be loaded, the skin changer works by ids" : "Loading the skins, agents & music kits";

        float pictures = ImageCache::GetPrefetchPercent();
        Step pictures_step = !started ? Step::Pending : ImageCache::IsPrefetched() ? Step::Done : Step::Running;
        std::string pictures_detail = ImageCache::GetPrefetchProgress();
        if (pictures_step == Step::Pending)
            pictures_detail = "Waiting for Start";
        else if (pictures_step == Step::Done)
            pictures_detail = "On the disk, shown right away";
        else if (pictures_detail.rfind("Loading ", 0) == 0)
            pictures_detail = "Downloading " + pictures_detail.substr(8);

        const Row rows[] = {
            { "Game", game_step, game_detail },
            { "Item list", list_step, list_detail },
            { "Item pictures", pictures_step, pictures_detail },
        };

        float y = S(260.f);
        for (const auto& row : rows) {
            StepIcon(d, ImVec2(margin + S(9.f), y + S(16.f)), row.step, time);
            Text(d, font_bold, 14.f, ImVec2(margin + S(30.f), y + S(1.f)), row.step == Step::Pending ? DIM : TEXT, row.name);
            ImU32 detail_color = row.step == Step::Failed ? BAD : row.step == Step::Warning ? WARN : FAINT;
            d->PushClipRect(ImVec2(margin + S(30.f), y), ImVec2(w - margin, y + S(44.f)), true);
            Text(d, font_regular, 12.f, ImVec2(margin + S(30.f), y + S(20.f)), detail_color, row.detail.c_str());
            d->PopClipRect();
            y += S(50.f);
        }

        // Started without the game: open it, it is used as soon as it is there
        bool waiting = game_step == Step::Running && Engine::IsWaitingForGame();
        static bool flashed = false;
        if (waiting && !flashed) {
            flashed = true;
            FLASHWINFO flash{ sizeof(flash), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
            FlashWindowEx(&flash);
        }
        if (waiting) {
            auto notice_min = ImVec2(margin, h - S(200.f)), notice_max = ImVec2(w - margin, h - S(144.f));
            float pulse = 0.5f + 0.5f * sinf(time * 3.f);
            d->AddRectFilled(notice_min, notice_max, IM_COL32(240, 180, 60, 22), S(10.f));
            d->AddRect(notice_min, notice_max, IM_COL32(240, 180, 60, static_cast<int>(110 + 90 * pulse)), S(10.f), 0, S(1.5f));
            Text(d, font_bold, 14.f, notice_min + ImVec2(S(16.f), S(11.f)), WARN, "Launch CS2");
            Text(d, font_regular, 12.f, notice_min + ImVec2(S(16.f), S(31.f)), DIM, "It is used as soon as it opens, keep this open");
        }

        // How far, then Start
        bool failed = game_step == Step::Failed;
        if (started && !failed) {
            float game_part = game_step == Step::Done || game_step == Step::Warning ? 1.f : 0.f;
            float list_part = list_step == Step::Done || list_step == Step::Warning ? 1.f : 0.f;
            float percent = finished ? 1.f : game_part * 0.3f + list_part * 0.1f + pictures * 0.6f;
            static float shown = 0.f;
            shown += (percent - shown) * std::min(1.f, io.DeltaTime * 6.f);

            auto bar_min = ImVec2(margin, h - S(100.f)), bar_max = ImVec2(w - margin, h - S(96.f));
            d->AddRectFilled(bar_min, bar_max, TRACK, S(2.f));
            d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * shown, bar_max.y), ACCENT, S(2.f));
            auto text = std::format("{}%", static_cast<int>(shown * 100.f + 0.5f));
            Text(d, font_regular, 12.f, ImVec2(bar_max.x - TextWidth(font_regular, 12.f, text.c_str()), bar_min.y - S(20.f)), DIM, text.c_str());
            Text(d, font_regular, 12.f, ImVec2(margin, bar_min.y - S(20.f)), DIM, finished ? "Ready" : "Checking...");
        }

        const char* label = "Start";
        bool can_start = state == Version::Latest && !started;
        if (failed)
            label = "Close";
        else if (finished)
            label = "Ready";
        else if (started)
            label = "Checking...";
        else if (state == Version::Checking)
            label = "Checking the version...";
        else if (state != Version::Latest)
            label = state == Version::Offline ? "Needs the version check" : "Update required";

        auto start_min = ImVec2(margin, h - margin - S(46.f));
        if (Button(d, "##start", start_min, ImVec2(w - margin * 2.f, S(46.f)), label, can_start || failed, true)) {
            if (failed)
                closed = true;
            else if (UseFolder()) {
                started = true;
                std::thread(Work).detach();
            }
        }

        ImGui::End();
    }
}

bool Loader::Run() {
    if (!Create()) {
        Destroy();
        MessageBoxA(nullptr, "The window of the loader could not be made (DirectX 11).", TITLE, MB_ICONERROR);
        return false;
    }

    LoadFolder();
    CheckVersion();

    std::chrono::steady_clock::time_point finished_at{};
    while (!closed) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (closed)
            break;

        // Done: shown for a moment, then the overlay takes over
        if (finished) {
            auto now = std::chrono::steady_clock::now();
            if (finished_at == std::chrono::steady_clock::time_point{})
                finished_at = now;
            else if (now - finished_at > 700ms)
                break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Draw();
        ImGui::Render();

        const float clear[4] = { 17.f / 255.f, 17.f / 255.f, 20.f / 255.f, 1.f };
        context->OMSetRenderTargets(1, &target, nullptr);
        context->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Minimized or hidden: nothing is shown, no need to spin
        if (swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED)
            std::this_thread::sleep_for(16ms);
    }

    bool done = finished && !closed;
    Destroy();
    return done;
}
