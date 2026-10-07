#include "Movement.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Subtick.hpp"
#include "gui/renderer/Renderer.hpp" // Circular dependency

#include <cmath>
#include <numbers>

#pragma comment(lib, "winmm.lib")

namespace {
    constexpr uint32_t FL_ONGROUND = (1 << 0);

    // Button states for client.dll buttons
    constexpr int BUTTON_PRESSED = 65537;
    constexpr int BUTTON_RELEASED = 256;

    // Counter strafing starts above this speed. It stops when the speed reaches zero, predicted from the last
    // simulated movement: the velocity we read is up to a tick old & the counter strafe keeps pushing in the meantime,
    // so stopping at a fixed speed went past zero into the other direction
    // Low, so a short tap (rapid trigger keyboards) is stopped too & not left to slide on friction
    constexpr float QUICK_STOP_START_SPEED = 15.f;
    constexpr float QUICK_STOP_MIN_SPEED = 3.f;      // Friction handles the rest at once
    constexpr float QUICK_STOP_LEAD = 0.003f;        // Seconds before zero: our key reaches the game a bit later

    // Counter strafe slowdown before it was measured: sv_accelerate 5.5 * 250 wish speed, plus friction 5.2 * speed.
    // Rather too fast than too slow, letting go early leaves a little slide friction stops, late overshoots
    constexpr float QUICK_STOP_ACCELERATE = 1375.f;
    constexpr float QUICK_STOP_FRICTION = 5.2f;
    constexpr float QUICK_STOP_DECEL_MIN = 400.f;
    constexpr float QUICK_STOP_DECEL_MAX = 4000.f;

    constexpr int MOVE_KEYS[] = { 'W', 'S', 'A', 'D' }; // Same order as Movement::Direction

    bool IsKeyDown(int key) {
        return GetAsyncKeyState(key) & 0x8000;
    }

    // Game input: SDL3 keyboard events, the queue the game polls its input from (IInputSystem turns them into buttons)
    constexpr uint32_t SDL_EVENT_KEY_DOWN = 0x300;
    constexpr uint32_t SDL_EVENT_KEY_UP = 0x301;
    constexpr uint32_t SDL_SCANCODES[] = { 26, 22, 4, 7 };          // W S A D
    constexpr uint32_t SDL_KEYCODES[] = { 'w', 's', 'a', 'd' };

    struct SdlKeyboardEvent {
        uint32_t type;
        uint32_t reserved;
        uint64_t timestamp;     // 0: SDL_PushEvent stamps it
        uint32_t window_id;     // Filled in the game, the window with the keyboard focus
        uint32_t which;
        uint32_t scancode;
        uint32_t key;
        uint16_t mod;
        uint16_t raw;
        bool down;
        bool repeat;
    };
    static_assert(offsetof(SdlKeyboardEvent, window_id) == 0x10 && offsetof(SdlKeyboardEvent, down) == 0x24);

    // Data of our thread in the game, right after its code
    struct InputData {
        uint64_t push_event;        // SDL_PushEvent
        uint64_t keyboard_focus;    // SDL_GetKeyboardFocus
        uint64_t window_id;         // SDL_GetWindowID
        uint64_t wait;              // WaitForSingleObject
        uint32_t head;              // Events written by us
        uint32_t tail;              // Events pushed by the thread
        uint32_t stop;
        uint32_t padding;
        uint64_t wake;              // Event handle, in the game
        uint64_t close_handle;      // CloseHandle
    };
    static_assert(offsetof(InputData, head) == 0x20 && offsetof(InputData, wake) == 0x30 && offsetof(InputData, close_handle) == 0x38);

    constexpr size_t INPUT_DATA = 0x100;            // From the code
    constexpr size_t INPUT_EVENTS = 0x100;          // From the data
    constexpr size_t INPUT_EVENT_SIZE = 128;        // sizeof(SDL_Event)
    constexpr uint32_t INPUT_QUEUE = 32;            // Events, the code masks the index with 31
    constexpr size_t INPUT_SIZE = INPUT_DATA + INPUT_EVENTS + INPUT_EVENT_SIZE * INPUT_QUEUE;

    // rcx = InputData. Pushes queued events until stop is set, sleeping on the wake event while there are none:
    //  loop:  if (stop) goto exit
    //         if (tail == head) { WaitForSingleObject(wake, 50); goto loop }
    //         event = events[tail & 31]
    //         if (window = SDL_GetKeyboardFocus()) event->window_id = SDL_GetWindowID(window)
    //         SDL_PushEvent(event); tail++; goto loop
    //  exit:  CloseHandle(wake); return 0
    constexpr uint8_t INPUT_CODE[] = {
        0x53, 0x56, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCB, 0x83, 0x7B, 0x28, 0x00, 0x75, 0x3F, 0x8B,
        0x43, 0x24, 0x3B, 0x43, 0x20, 0x74, 0x29, 0x83, 0xE0, 0x1F, 0xC1, 0xE0, 0x07, 0x48, 0x8D, 0xB4,
        0x03, 0x00, 0x01, 0x00, 0x00, 0xFF, 0x53, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x09, 0x48, 0x89, 0xC1,
        0xFF, 0x53, 0x10, 0x89, 0x46, 0x10, 0x48, 0x89, 0xF1, 0xFF, 0x13, 0xFF, 0x43, 0x24, 0xEB, 0xC9,
        0x48, 0x8B, 0x4B, 0x30, 0xBA, 0x32, 0x00, 0x00, 0x00, 0xFF, 0x53, 0x18, 0xEB, 0xBB, 0x48, 0x8B,
        0x4B, 0x30, 0xFF, 0x53, 0x38, 0x48, 0x83, 0xC4, 0x28, 0x5E, 0x5B, 0x31, 0xC0, 0xC3,
    };
    static_assert(sizeof(INPUT_CODE) <= INPUT_DATA);

    // Windows key events, when the game input could not be set up
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

bool Movement::UsesGameInput() {
    return GetInstance().game_input;
}

void Movement::Shutdown() {
    auto& movement = GetInstance();
    if (!movement.input_block && !movement.hooked)
        return;

    // The movement thread would press the keys again
    movement.stopping = true;
    std::this_thread::sleep_for(20ms);

    // Keys we hold back to what the keyboard says
    movement.ReleaseCounterStrafe();
    for (size_t key = 0; key < std::size(MOVE_KEYS); key++) {
        if (movement.nulled[key] && movement.held[key]) {
            movement.nulled[key] = false;
            SendKey(MOVE_KEYS[key], true);
        }
    }

    if (!movement.input_block)
        return;

    // Our thread pushes the last events, then ends on its own. Its memory is freed once it is surely out of it
    std::this_thread::sleep_for(20ms);
    movement.game_input = false;

    auto p = Engine::GetProcess();
    p->write<uint32_t>(movement.input_block + INPUT_DATA + offsetof(InputData, stop), 1);
    SetEvent(movement.input_wake);
    std::this_thread::sleep_for(150ms);

    p->free_remote(movement.input_block);
    movement.input_block = 0;
    CloseHandle(movement.input_wake);
    movement.input_wake = nullptr;
}

bool Movement::IsPlaying() {
    auto p = Engine::GetProcess();
    if (!p || GetForegroundWindow() != p->hwnd_)
        return false;

    CURSORINFO cursor{ sizeof(cursor) };
    return GetCursorInfo(&cursor) && !(cursor.flags & CURSOR_SHOWING);
}

bool Movement::IsKeyUsable(int key) {
    // Not the cursor: the game shows it after a while of nothing pressed, until the next click or key
    (void)key;
    return HasFocus();
}

bool Movement::HasFocus() {
    auto p = Engine::GetProcess();
    return p && GetForegroundWindow() == p->hwnd_ && !Renderer::IsOpen();
}

bool Movement::InitImpl() {
    if (!Engine::IsInsecure())
        return false;

    // sleep_for rounds up to the system timer resolution (~15.6ms by default)
    timeBeginPeriod(1);
    this->move_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (SetupGameInput())
        LOGF(INFO, "Movement keys go straight into the game input");
    else
        LOGF(WARNING, "Could not set up the game input, movement keys are sent as Windows key events");

    std::thread(&Movement::Thread, this).detach();
    std::thread(&Movement::InputThread, this).detach();

    LOGF(INFO, "Successfully initialized movement...");
    return true;
}

bool Movement::SetupGameInput() {
    auto p = Engine::GetProcess();

    InputData data{};
    data.push_event = p->FindExport("SDL3.dll", "SDL_PushEvent");
    data.keyboard_focus = p->FindExport("SDL3.dll", "SDL_GetKeyboardFocus");
    data.window_id = p->FindExport("SDL3.dll", "SDL_GetWindowID");

    // System modules are at the same address in every process
    auto kernel = GetModuleHandleA("kernel32.dll");
    data.wait = reinterpret_cast<uint64_t>(GetProcAddress(kernel, "WaitForSingleObject"));
    data.close_handle = reinterpret_cast<uint64_t>(GetProcAddress(kernel, "CloseHandle"));

    if (!data.push_event || !data.keyboard_focus || !data.window_id || !data.wait || !data.close_handle)
        return false;

    // Auto reset, set after every event we queue. The game gets its own handle of it
    this->input_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE remote_wake = nullptr;
    if (!this->input_wake || !DuplicateHandle(GetCurrentProcess(), this->input_wake, p->handle_, &remote_wake, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        if (this->input_wake)
            CloseHandle(this->input_wake);
        this->input_wake = nullptr;
        return false;
    }
    data.wake = reinterpret_cast<uint64_t>(remote_wake);

    this->input_block = p->allocate_remote(INPUT_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->input_block)
        return false;

    p->write_bytes(this->input_block, std::vector<uint8_t>(std::begin(INPUT_CODE), std::end(INPUT_CODE)));
    p->write<InputData>(this->input_block + INPUT_DATA, data);

    if (!p->start_remote(this->input_block, this->input_block + INPUT_DATA)) {
        p->free_remote(this->input_block);
        this->input_block = 0;
        return false;
    }

    this->game_input = true;
    return true;
}

bool Movement::PushGameKey(size_t key, bool down) {
    return PushGameEvent(SDL_SCANCODES[key], SDL_KEYCODES[key], down);
}

bool Movement::PushGameEvent(uint32_t scancode, uint32_t keycode, bool down) {
    auto p = Engine::GetProcess();
    auto data = this->input_block + INPUT_DATA;

    std::lock_guard<std::mutex> lock(this->input_mutex);

    // Queue full: our thread in the game is stuck, wait a little for it
    for (int i = 0; p->read<uint32_t>(data + offsetof(InputData, tail)) + INPUT_QUEUE <= this->input_head; i++) {
        if (i >= 20)
            return false;
        std::this_thread::sleep_for(1ms);
    }

    SdlKeyboardEvent event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.scancode = scancode;
    event.key = keycode;
    event.down = down;

    // The event first, then the index that hands it over
    auto slot = data + INPUT_EVENTS + INPUT_EVENT_SIZE * (this->input_head % INPUT_QUEUE);
    p->write_bytes(slot, std::vector<uint8_t>(INPUT_EVENT_SIZE, 0));
    p->write<SdlKeyboardEvent>(slot, event);
    p->write<uint32_t>(data + offsetof(InputData, head), ++this->input_head);

    SetEvent(this->input_wake);
    return true;
}

void Movement::Press(size_t key, bool down) {
    if (this->game_input && PushGameKey(key, down))
        return;

    SendKey(MOVE_KEYS[key], down);
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
            bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;

            // The free cam takes the keys it flies with, the game never sees them
            bool camera = Freecam::OnKey(info->vkCode, down);

            for (size_t i = 0; i < std::size(MOVE_KEYS); i++) {
                if (info->vkCode != static_cast<DWORD>(MOVE_KEYS[i]))
                    continue;

                if (camera) {
                    GetInstance().held[i] = down;
                    return 1;
                }

                if (GetInstance().OnMoveKey(i, down))
                    return 1; // Blocked

                break;
            }

            if (camera)
                return 1;
        }
    }

    return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool Movement::OnMoveKey(size_t key, bool down) {
    size_t opposite = key ^ 1; // W & S, A & D
    bool repeat = down && this->held[key];

    this->held[key] = down;

    if (!down && this->move_wake)
        SetEvent(this->move_wake);

    // Order of presses, the last pressed direction of an axis wins
    if (down && !repeat)
        this->pressed_at[key] = ++this->press_count;

    auto p = Engine::GetProcess();
    bool active = cfg::misc::null_binds && p && IsPlaying();

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

bool Movement::IsSpaceHeld() {
    return IsKeyDown(VK_SPACE);
}

bool Movement::IsHeld(size_t key) {
    // Without the hook, keys we hold for a counter strafe can't be told apart from the player's
    if (!this->hooked)
        return IsKeyDown(MOVE_KEYS[key]) && !this->counter_strafing[key];

    return this->held[key];
}

void Movement::Thread() {
    while (!this->stopping) {
        // The camera away from our player: none of it, it would move the player standing there
        if (Freecam::GetMode() != Freecam::Mode::Off) {
            if (this->jump_pressed)
                SetJump(false);
            ReleaseCounterStrafe();
            std::this_thread::sleep_for(1ms);
            continue;
        }

        QuickStop();
        Bhop();

        // A move key let go wakes it right away: the counter strafe starts with the release, not up to a ms later
        if (this->move_wake)
            WaitForSingleObject(this->move_wake, 1);
        else
            std::this_thread::sleep_for(1ms);
    }
}

void Movement::Bhop() {
    auto p = Engine::GetProcess();

    // With -insecure the jump is a subtick press of Subtick, right at the landing. Holding the key from here would be
    // a press at the start of the tick, while still in the air
    bool should_run = cfg::misc::bhop
        && p && IsPlaying()
        && IsSpaceHeld()
        && !Subtick::HandlesBhop();

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
        && !IsSpaceHeld()
        && !player_moving;

    auto pawn = should_run ? Engine::GetLocalPawn() : 0;

    if (!pawn || p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0 ||
        !(p->read<uint32_t>(pawn + offsets::pawn::m_fFlags) & FL_ONGROUND)) {
        ReleaseCounterStrafe();
        this->counter_decel = 0.f;
        return;
    }

    // Velocity relative to where we are looking
    auto velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
    float yaw = p->read<Vec3_t>(pawn + offsets::grenade::m_angEyeAngles).y * std::numbers::pi_v<float> / 180.f;

    auto along = [&](const Vec3_t& v) {
        return std::pair{ v.x * cosf(yaw) + v.y * sinf(yaw), v.x * sinf(yaw) - v.y * cosf(yaw) };
    };

    auto countering = [&]() {
        return std::any_of(std::begin(this->counter_strafing), std::end(this->counter_strafing), [](bool b) { return b; });
    };

    auto now = std::chrono::steady_clock::now();

    // The game simulated another step: how fast the counter strafe slowed us down since the last one
    if (velocity != this->tick_velocity) {
        float seconds = std::chrono::duration<float>(now - this->tick_time).count();

        if (countering() && seconds > 0.002f && seconds < 0.05f) {
            auto [before_forward, before_right] = along(this->tick_velocity);
            auto [forward, right] = along(velocity);

            float measured = (std::hypot(before_forward, before_right) - std::hypot(forward, right)) / seconds;
            if (measured > 0.f) {
                measured = std::clamp(measured, QUICK_STOP_DECEL_MIN, QUICK_STOP_DECEL_MAX);
                this->counter_decel = this->counter_decel > 0.f ? (this->counter_decel + measured) * 0.5f : measured;
            }
        }

        this->tick_velocity = velocity;
        this->tick_time = now;
    }

    auto [forward, right] = along(velocity);
    float since_tick = std::chrono::duration<float>(now - this->tick_time).count();

    // Not measured yet: what the movement code of the game does at this speed
    float decel = this->counter_decel > 0.f
        ? this->counter_decel
        : std::clamp(QUICK_STOP_ACCELERATE + QUICK_STOP_FRICTION * std::hypot(forward, right), QUICK_STOP_DECEL_MIN, QUICK_STOP_DECEL_MAX);

    // Hold the opposite direction until the speed reaches zero
    UpdateCounterStrafe(Direction::Back, forward, decel, since_tick);
    UpdateCounterStrafe(Direction::Forward, -forward, decel, since_tick);
    UpdateCounterStrafe(Direction::Left, right, decel, since_tick);
    UpdateCounterStrafe(Direction::Right, -right, decel, since_tick);

    if (!countering())
        this->counter_decel = 0.f;
}

void Movement::UpdateCounterStrafe(Direction direction, float speed_against, float decel, float seconds_since_tick) {
    bool pressed = this->counter_strafing[static_cast<size_t>(direction)];

    if (!pressed) {
        if (speed_against > QUICK_STOP_START_SPEED)
            SetCounterStrafe(direction, true);
        return;
    }

    // Where the speed is now, the game kept slowing us down since the movement we read
    float speed_now = speed_against - decel * seconds_since_tick;

    if (speed_against < QUICK_STOP_MIN_SPEED || speed_now < decel * QUICK_STOP_LEAD)
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

    Press(index, pressed);
}

void Movement::ReleaseCounterStrafe() {
    for (auto direction : { Direction::Forward, Direction::Back, Direction::Left, Direction::Right })
        SetCounterStrafe(direction, false);
}
