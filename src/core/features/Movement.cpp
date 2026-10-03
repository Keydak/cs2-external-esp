#include "Movement.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"

#include <numbers>

#pragma comment(lib, "winmm.lib")

namespace {
    constexpr uint32_t FL_ONGROUND = (1 << 0);

    // Button states for client.dll buttons
    constexpr int BUTTON_PRESSED = 65537;
    constexpr int BUTTON_RELEASED = 256;

    // Counter strafing starts above the first speed and stops below the second, friction does the rest.
    // The gap keeps a small overshoot from starting a counter strafe the other way
    constexpr float QUICK_STOP_START_SPEED = 60.f;
    constexpr float QUICK_STOP_END_SPEED = 35.f;

    constexpr int MOVE_KEYS[] = { 'W', 'S', 'A', 'D' }; // Same order as Movement::Direction

    bool IsKeyDown(int key) {
        return GetAsyncKeyState(key) & 0x8000;
    }

    // Slide walk
    constexpr int SLIDE_MODE_HOLD = 0;
    constexpr int SLIDE_MODE_TOGGLE = 1;
    constexpr int SLIDE_MODE_ALWAYS = 2;

    constexpr int SLIDE_PATTERN_TIMER = 0;
    constexpr int SLIDE_PATTERN_SPEED = 1;

    constexpr float SLIDE_SPEED_BAND = 0.8f; // Speed pattern lets go of the opposite key below this part of the target

    // Real key events, the game takes movement from them & not from the button states in its memory
    void SendKey(int key, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyA(key, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);

        SendInput(1, &input, sizeof(INPUT));
    }
}

bool Movement::Init() {
    return GetInstance().InitImpl();
}

bool Movement::IsAvailable() {
    return Engine::IsInsecure();
}

bool Movement::IsPlaying() {
    auto p = Engine::GetProcess();
    if (!p || GetForegroundWindow() != p->hwnd_)
        return false;

    CURSORINFO cursor{ sizeof(cursor) };
    return GetCursorInfo(&cursor) && !(cursor.flags & CURSOR_SHOWING);
}

bool Movement::InitImpl() {
    if (!Engine::IsInsecure())
        return false;

    // sleep_for rounds up to the system timer resolution (~15.6ms by default)
    timeBeginPeriod(1);

    std::thread(&Movement::Thread, this).detach();
    std::thread(&Movement::InputThread, this).detach();

    LOGF(INFO, "Successfully initialized movement...");
    return true;
}

void Movement::InputThread() {
    // Low level hook, it tells keys pressed on the keyboard apart from the ones we send
    if (!SetWindowsHookExA(WH_KEYBOARD_LL, &Movement::KeyboardHook, GetModuleHandleA(nullptr), 0)) {
        LOGF(WARNING, "Failed to install the keyboard hook ({}), null binds are disabled", GetLastError());
        return;
    }

    this->hooked = true;

    // The hook is called through this thread's message loop
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

LRESULT CALLBACK Movement::KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        auto info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);

        if (!(info->flags & LLKHF_INJECTED)) {
            for (size_t i = 0; i < std::size(MOVE_KEYS); i++) {
                if (info->vkCode != static_cast<DWORD>(MOVE_KEYS[i]))
                    continue;

                bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
                if (GetInstance().OnMoveKey(i, down))
                    return 1; // Blocked

                break;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool Movement::OnMoveKey(size_t key, bool down) {
    size_t opposite = key ^ 1; // W & S, A & D
    bool repeat = down && this->held[key];

    this->held[key] = down;

    // Order of presses, the last pressed direction of an axis wins
    if (down && !repeat)
        this->pressed_at[key] = ++this->press_count;

    // Slide walk is holding this key released, key repeat would press it again
    if (repeat && this->slide_state[key] == SlideState::Released)
        return true;

    auto p = Engine::GetProcess();
    // Slide walk resolves opposite keys itself, the same way
    bool active = cfg::misc::null_binds && !this->sliding && p && IsPlaying();

    if (!active) {
        this->nulled[key] = this->nulled[opposite] = false;
        return false;
    }

    if (down) {
        // Key repeat would press the key the opposite one took over again
        if (repeat)
            return this->nulled[key];

        // Newest key wins, the opposite one is let go while it stays held
        if (this->held[opposite] && !this->nulled[opposite]) {
            this->nulled[opposite] = true;
            SendKey(MOVE_KEYS[opposite], false);
        }

        return false;
    }

    // Released while taken over, the game already saw it go up
    if (this->nulled[key]) {
        this->nulled[key] = false;
        return true;
    }

    // Back to the key still held
    if (this->held[opposite] && this->nulled[opposite]) {
        this->nulled[opposite] = false;
        SendKey(MOVE_KEYS[opposite], true);
    }

    return false;
}

bool Movement::IsHeld(size_t key) {
    // Without the hook, keys we hold for a counter strafe can't be told apart from the player's
    if (!this->hooked)
        return IsKeyDown(MOVE_KEYS[key]) && !this->counter_strafing[key];

    return this->held[key];
}

void Movement::Thread() {
    while (true) {
        QuickStop();
        SlideWalk();
        Bhop();
        std::this_thread::sleep_for(1ms);
    }
}

void Movement::Bhop() {
    auto p = Engine::GetProcess();

    bool should_run = cfg::misc::bhop
        && p && IsPlaying()
        && (GetAsyncKeyState(VK_SPACE) & 0x8000);

    auto pawn = should_run ? Engine::GetLocalPawn() : 0;

    if (!pawn || p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0) {
        // Let go of a jump we were holding, SPACE was released in the meantime
        if (this->jump_pressed && p)
            SetJump(false);
        return;
    }

    // Jump held while on the ground, let go in the air, so every landing is a new press
    auto flags = p->read<uint32_t>(pawn + offsets::pawn::m_fFlags);
    SetJump(flags & FL_ONGROUND);
}

void Movement::SetJump(bool pressed) {
    this->jump_pressed = pressed;

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    p->write<int>(client.base + offsets::buttons::jump, pressed ? BUTTON_PRESSED : BUTTON_RELEASED);
}

void Movement::QuickStop() {
    auto p = Engine::GetProcess();

    bool player_moving = false;
    for (size_t i = 0; i < std::size(MOVE_KEYS); i++)
        player_moving |= IsHeld(i);

    bool should_run = cfg::misc::quick_stop
        && p && IsPlaying()
        && !IsKeyDown(VK_SPACE)
        && !player_moving
        && !this->sliding;

    auto pawn = should_run ? Engine::GetLocalPawn() : 0;

    if (!pawn || p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0 ||
        !(p->read<uint32_t>(pawn + offsets::pawn::m_fFlags) & FL_ONGROUND)) {
        ReleaseCounterStrafe();
        return;
    }

    // Velocity relative to where we are looking
    auto velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
    float yaw = p->read<Vec3_t>(pawn + offsets::grenade::m_angEyeAngles).y * std::numbers::pi_v<float> / 180.f;

    float forward = velocity.x * cosf(yaw) + velocity.y * sinf(yaw);
    float right = velocity.x * sinf(yaw) - velocity.y * cosf(yaw);

    // Hold the opposite direction until we are almost stopped
    UpdateCounterStrafe(Direction::Back, forward);
    UpdateCounterStrafe(Direction::Forward, -forward);
    UpdateCounterStrafe(Direction::Left, right);
    UpdateCounterStrafe(Direction::Right, -right);
}

void Movement::UpdateCounterStrafe(Direction direction, float speed_against) {
    bool pressed = this->counter_strafing[static_cast<size_t>(direction)];

    if (!pressed && speed_against > QUICK_STOP_START_SPEED)
        SetCounterStrafe(direction, true);
    else if (pressed && speed_against < QUICK_STOP_END_SPEED)
        SetCounterStrafe(direction, false);
}

void Movement::SetCounterStrafe(Direction direction, bool pressed) {
    auto index = static_cast<size_t>(direction);
    if (this->counter_strafing[index] == pressed)
        return;

    this->counter_strafing[index] = pressed;

    // The player pressed it in the meantime and keeps holding it, letting go would cancel their press
    if (!pressed && this->hooked && this->held[index])
        return;

    SendKey(MOVE_KEYS[index], pressed);
}

void Movement::ReleaseCounterStrafe() {
    for (auto direction : { Direction::Forward, Direction::Back, Direction::Left, Direction::Right })
        SetCounterStrafe(direction, false);
}

void Movement::SlideWalk() {
    auto p = Engine::GetProcess();
    bool focused = p && IsPlaying();

    // Toggle mode flips on every press of the key
    bool key_down = focused && IsKeyDown(cfg::misc::slide_walk_key);
    if (key_down && !this->slide_key_was_down)
        this->slide_toggled = !this->slide_toggled;
    this->slide_key_was_down = key_down;

    bool wanted = false;
    switch (cfg::misc::slide_walk_mode) {
    case SLIDE_MODE_HOLD:   wanted = key_down;              break;
    case SLIDE_MODE_TOGGLE: wanted = this->slide_toggled;   break;
    case SLIDE_MODE_ALWAYS: wanted = true;                  break;
    }

    uintptr_t pawn = 0;
    if (cfg::misc::slide_walk && this->hooked && focused && wanted) {
        pawn = Engine::GetLocalPawn();
        if (pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0)
            pawn = 0;
    }

    this->sliding = pawn != 0;

    if (!pawn) {
        this->slide_start = {};
        this->slide_flipped[0] = this->slide_flipped[1] = false;

        for (size_t key = 0; key < std::size(MOVE_KEYS); key++)
            SetSlideKey(key, this->held[key]);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (this->slide_start == std::chrono::steady_clock::time_point{})
        this->slide_start = now;

    // Timer pattern: where we are in the current flip, first the opposite direction, then the held one
    float rate = std::clamp(cfg::misc::slide_walk_rate, 1.f, 64.f);
    float phase = fmodf(std::chrono::duration<float>(now - this->slide_start).count() * rate, 1.f);
    bool timer_flipped = phase < std::clamp(cfg::misc::slide_walk_ratio, 0.f, 1.f);

    // Speed pattern: velocity in each move key direction (W S A D)
    float speed_along[4]{};
    if (cfg::misc::slide_walk_pattern == SLIDE_PATTERN_SPEED) {
        auto velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
        float yaw = p->read<Vec3_t>(pawn + offsets::grenade::m_angEyeAngles).y * std::numbers::pi_v<float> / 180.f;

        float forward = velocity.x * cosf(yaw) + velocity.y * sinf(yaw);
        float right = velocity.x * sinf(yaw) - velocity.y * cosf(yaw);

        speed_along[0] = forward;
        speed_along[1] = -forward;
        speed_along[2] = -right;
        speed_along[3] = right;
    }

    // Each axis on its own, only when a single direction of it is held
    bool any_held = std::any_of(std::begin(this->held), std::end(this->held), [](const auto& held) { return held.load(); });

    for (size_t axis = 0; axis < 2; axis++) {
        size_t first = axis * 2, second = first + 1;
        bool first_held = this->held[first], second_held = this->held[second];

        // Auto walk slides forward while no direction is held
        if (cfg::misc::slide_walk_auto && !any_held && axis == 0)
            first_held = true;

        if (!first_held && !second_held) {
            this->slide_flipped[axis] = false;
            SetSlideKey(first, false);
            SetSlideKey(second, false);
            continue;
        }

        // Both held: the last one pressed wins, like null binds, instead of cancelling each other out
        size_t main = first_held ? first : second;
        if (first_held && second_held)
            main = this->pressed_at[first] > this->pressed_at[second] ? first : second;

        // New direction, the speed pattern starts over
        if (main != this->slide_main[axis]) {
            this->slide_main[axis] = main;
            this->slide_flipped[axis] = false;
        }

        bool flipped = timer_flipped;

        // Pushes back once above the target speed, until it drops a bit below
        if (cfg::misc::slide_walk_pattern == SLIDE_PATTERN_SPEED) {
            float target = cfg::misc::slide_walk_speed;
            auto& state = this->slide_flipped[axis];

            if (!state && speed_along[main] > target)
                state = true;
            else if (state && speed_along[main] < target * SLIDE_SPEED_BAND)
                state = false;

            flipped = state;
        }

        SetSlideKey(main, !flipped);
        SetSlideKey(main ^ 1, flipped);
    }
}

bool Movement::IsSliding() {
    return GetInstance().sliding;
}

void Movement::SetSlideKey(size_t key, bool down) {
    auto& state = this->slide_state[key];
    bool physical = this->held[key];

    // Matches the keyboard, give the key back if we changed it
    if (down == physical) {
        if (state != SlideState::None) {
            state = SlideState::None;
            SendSlideKey(key, down);
        }
        return;
    }

    auto wanted = down ? SlideState::Pressed : SlideState::Released;

    if (state == wanted)
        return;

    state = wanted;
    SendSlideKey(key, down);
}

void Movement::SendSlideKey(size_t key, bool down) {
    SendKey(MOVE_KEYS[key], down);
}
