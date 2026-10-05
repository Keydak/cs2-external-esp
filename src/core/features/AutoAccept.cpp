#include "AutoAccept.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/GameThread.hpp"
#include "core/offsets/Offsets.hpp"

#include <cstring>

namespace {
    // ".accept-match .FlatButton" of panorama/styles/popups/popup_accept_match.vcss: a solid button of this color,
    // drawn as (54, 183, 82). The border & glow of the popup come close in a few pixels, kept out by the
    // tight tolerance & by taking the box only from rows & columns full of the color
    constexpr int BUTTON_R = 53, BUTTON_G = 182, BUTTON_B = 81;
    constexpr int BUTTON_TOLERANCE = 8;
    constexpr float DENSE_LINE = 0.3f;      // Rows & columns with this part of the fullest one belong to the button

    constexpr int STEP = 2;                 // Every second pixel of every second row
    constexpr int MIN_SAMPLES = 150;
    constexpr float MIN_FILL = 0.45f;       // Part of the button box with its color, the rest is the label
    constexpr float MIN_WIDTH = 0.06f;      // Of the window
    constexpr float MIN_HEIGHT = 0.03f;

    constexpr auto CHECK_INTERVAL = 500ms;
    constexpr auto CONFIRM_INTERVAL = 150ms; // Seen once, looked at again before clicking
    constexpr auto AFTER_CLICK = 3s;

    constexpr int32_t ACCEPTED = 2;         // State of the matchmaking once accepted
}

bool AutoAccept::Init() {
    auto& accept = GetInstance();
    if (accept.running.exchange(true))
        return true;

    std::thread(&AutoAccept::Thread, &accept).detach();
    return true;
}

void AutoAccept::Shutdown() {
    GetInstance().stopping = true;
}

void AutoAccept::Thread() {
    POINT last{};
    int seen = 0;
    bool told_failed = false;

    while (!this->stopping) {
        std::this_thread::sleep_for(seen ? CONFIRM_INTERVAL : CHECK_INTERVAL);

        if (!cfg::misc::auto_accept) {
            seen = 0;
            continue;
        }

        auto p = Engine::GetProcess();
        if (!p)
            continue;

        HWND window = p->hwnd_;
        if (!window || !IsWindow(window)) {
            p->UpdateHWND();
            continue;
        }

        // Minimized there is nothing to see. Whether we are in a match can't be told from the local controller,
        // it keeps the one of the last match in the menu
        if (IsIconic(window)) {
            seen = 0;
            continue;
        }

        POINT center{};
        if (!FindButton(window, center)) {
            seen = 0;
            continue;
        }

        // The popup scales in, the button has to stand still where it was seen
        bool same = seen && std::abs(center.x - last.x) <= 8 && std::abs(center.y - last.y) <= 8;
        last = center;
        if (!same) {
            seen = 1;
            continue;
        }

        // From memory, nothing to click
        if (AcceptInMemory()) {
            LOGF(INFO, "Match found, accepted it");
            told_failed = false;
            seen = 0;
            std::this_thread::sleep_for(AFTER_CLICK);
            continue;
        }

        // Else a click posted to the window: the cursor never moves & what the player does with the mouse can't get
        // in between. Brought to the front first, the game takes clicks only there
        if (GetForegroundWindow() != window) {
            DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            DWORD ours = GetCurrentThreadId();

            // Windows only lets the app in the foreground hand it over, borrowed by sharing its input
            AttachThreadInput(ours, foreground, TRUE);
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            BringWindowToTop(window);
            AttachThreadInput(ours, foreground, FALSE);
            continue;
        }

        PostClick(window, center);
        LOGF(INFO, "Match found, clicked ACCEPT");
        told_failed = false;
        seen = 0;
        std::this_thread::sleep_for(AFTER_CLICK);

        // Still there after the click: the game did not take it
        POINT again{};
        if (FindButton(window, again) && !told_failed) {
            LOGF(WARNING, "ACCEPT is still there after clicking it, accept it yourself");
            told_failed = true;
        }
    }
}

void AutoAccept::PostClick(HWND window, POINT center) {
    // Window messages, not input: the game reads them like a real click. Posted ones go before the input of the
    // mouse, the move right before each button puts the game at the button whatever the mouse did in between
    LPARAM at = MAKELPARAM(center.x, center.y);

    PostMessageA(window, WM_MOUSEMOVE, 0, at);
    std::this_thread::sleep_for(30ms);                 // Hovered first, like a real click it lights up

    PostMessageA(window, WM_MOUSEMOVE, 0, at);
    PostMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, at);
    std::this_thread::sleep_for(40ms);

    PostMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, at);
    PostMessageA(window, WM_LBUTTONUP, 0, at);
}

bool AutoAccept::Capture(HWND window, int width, int height, std::vector<uint32_t>& pixels) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // Top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    bool captured = false;
    if (memory && bitmap && bits) {
        auto previous = SelectObject(memory, bitmap);

        // Asks the window to draw itself, also when other windows are over it (our overlay included)
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
        captured = PrintWindow(window, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

        auto data = static_cast<const uint32_t*>(bits);
        bool blank = true;
        for (size_t i = 0; i < size_t(width) * height && blank; i += 997)
            blank = (data[i] & 0xFFFFFF) == 0;

        // Some games come out black that way: what is on the screen then, right when the game is in front
        if ((!captured || blank) && GetForegroundWindow() == window) {
            POINT origin{};
            ClientToScreen(window, &origin);
            captured = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);
        }

        if (captured)
            pixels.assign(data, data + size_t(width) * height);

        SelectObject(memory, previous);
    }

    if (bitmap)
        DeleteObject(bitmap);
    if (memory)
        DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    return captured;
}

bool AutoAccept::FindButton(HWND window, POINT& center) {
    RECT client{};
    if (!GetClientRect(window, &client))
        return false;

    int width = client.right, height = client.bottom;
    if (width < 320 || height < 240)
        return false;

    std::vector<uint32_t> pixels;
    if (!Capture(window, width, height, pixels))
        return false;

    auto is_button = [&](int x, int y) {
        uint32_t color = pixels[size_t(y) * width + x];
        int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
        return std::abs(r - BUTTON_R) <= BUTTON_TOLERANCE && std::abs(g - BUTTON_G) <= BUTTON_TOLERANCE && std::abs(b - BUTTON_B) <= BUTTON_TOLERANCE;
    };

    // The popup is in the middle, the button at its bottom. How much of each row & column has the color
    int left = width / 4, right = width * 3 / 4, top = height / 4, bottom = height * 9 / 10;
    std::vector<int> rows(height), columns(width);
    int samples = 0;

    for (int y = top; y < bottom; y += STEP) {
        for (int x = left; x < right; x += STEP) {
            if (!is_button(x, y))
                continue;

            samples++;
            rows[y]++;
            columns[x]++;
        }
    }

    if (samples < MIN_SAMPLES)
        return false;

    // The box of the button: the rows & columns full of the color, stray pixels of the popup border left out
    int fullest_row = *std::max_element(rows.begin(), rows.end());
    int fullest_column = *std::max_element(columns.begin(), columns.end());

    int min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int y = top; y < bottom; y += STEP) {
        if (rows[y] >= fullest_row * DENSE_LINE) {
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    for (int x = left; x < right; x += STEP) {
        if (columns[x] >= fullest_column * DENSE_LINE) {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
        }
    }

    // A wide box mostly filled with the color, the rest is the label
    int box_width = max_x - min_x + 1, box_height = max_y - min_y + 1;
    if (box_width < width * MIN_WIDTH || box_height < height * MIN_HEIGHT || box_width < box_height)
        return false;

    int inside = 0;
    for (int y = min_y; y <= max_y; y += STEP)
        for (int x = min_x; x <= max_x; x += STEP)
            inside += is_button(x, y);

    float fill = float(inside) * STEP * STEP / (float(box_width) * box_height);
    if (fill < MIN_FILL)
        return false;

    center = { (min_x + max_x) / 2, (min_y + max_y) / 2 };
    return true;
}

bool AutoAccept::AcceptInMemory() {
    if (!Engine::IsInsecure() || !offsets::lobby::accept || !offsets::lobby::matchmaking || !GameThread::Ensure())
        return false;

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    // No matchmaking, not in the queue: the green seen is something else
    auto matchmaking = p->read<uintptr_t>(client.base + offsets::lobby::matchmaking);
    if (!matchmaking)
        return false;
    if (p->read<int32_t>(matchmaking + offsets::lobby::m_nState) >= ACCEPTED)
        return true;

    if (!this->accept_code) {
        // if (matchmaking) Accept(matchmaking, true), like LobbyAPI.SetLocalPlayerReady("accept")
        std::vector<uint8_t> code = {
            0x48, 0x83, 0xEC, 0x28,                     // sub rsp, 0x28
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,         // mov rax, &matchmaking
            0x48, 0x8B, 0x08,                           // mov rcx, [rax]
            0x48, 0x85, 0xC9,                           // test rcx, rcx
            0x74, 0x0E,                                 // jz end
            0xB2, 0x01,                                 // mov dl, 1
            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,         // mov rax, accept
            0xFF, 0xD0,                                 // call rax
            0x48, 0x83, 0xC4, 0x28,                     // end: add rsp, 0x28
            0xC3,                                       // ret
        };
        uint64_t pointer = client.base + offsets::lobby::matchmaking, function = client.base + offsets::lobby::accept;
        std::memcpy(&code[6], &pointer, sizeof(pointer));
        std::memcpy(&code[26], &function, sizeof(function));

        this->accept_code = p->allocate_remote(code.size(), PAGE_EXECUTE_READWRITE);
        if (!this->accept_code)
            return false;
        p->write_bytes(this->accept_code, code);
        FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->accept_code), code.size());
    }

    if (!GameThread::Call(this->accept_code))
        return false;

    // Taken: the state moved on
    matchmaking = p->read<uintptr_t>(client.base + offsets::lobby::matchmaking);
    bool accepted = matchmaking && p->read<int32_t>(matchmaking + offsets::lobby::m_nState) >= ACCEPTED;
    if (!accepted)
        LOGF(WARNING, "Accepting from memory did not take");
    return accepted;
}
