#pragma once
#include "core/memory/Memory.hpp"
#include "core/engine/types/Vec3.hpp"

class Movement {
public:
    ~Movement()                          = default;
    Movement(const Movement&)            = delete;
    Movement(Movement&&)                 = delete;
    Movement& operator=(const Movement&) = delete;
    Movement& operator=(Movement&&)      = delete;

    static bool Init();
    static bool IsAvailable();

    // The game has focus & takes our keys as moves: not typing in the chat or console, no menu open.
    // The game hides the cursor only while playing
    static bool IsPlaying();

    // A key that turns something on or off may be read: the game in front & playing. Mouse buttons also when the
    // cursor shows, the game shows it after a while of nothing pressed until the next click
    static bool IsKeyUsable(int key);

    // The game in front & our menu closed, whatever the cursor does
    static bool HasFocus();

    // Keys go straight into the input of the game (the SDL event queue IInputSystem reads), not through Windows
    static bool UsesGameInput();

    // Lets go of the keys we hold & stops our thread in the game
    static void Shutdown();
private:
    Movement() {};

    static Movement& GetInstance()
    {
        static Movement i{};
        return i;
    }

    bool InitImpl();
    void Thread();
    void InputThread();

    static LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam);
    bool OnMoveKey(size_t key, bool down); // Returns true to block the key event
    bool IsHeld(size_t key);

    bool IsSpaceHeld();

    // A move key (W S A D) pressed or released for the game: game input when it is set up, a Windows key event otherwise
    void Press(size_t key, bool down);
    bool SetupGameInput();
    bool PushGameKey(size_t key, bool down);
    bool PushGameEvent(uint32_t scancode, uint32_t keycode, bool down);

    void Bhop();
    void SetJump(bool pressed);

    enum class Direction { Forward, Back, Left, Right };

    void QuickStop();
    void UpdateCounterStrafe(Direction direction, float speed_against, float decel, float seconds_since_tick);
    void SetCounterStrafe(Direction direction, bool pressed);
    void ReleaseCounterStrafe();

private:
    bool counter_strafing[4]{}; // Keys we are holding down to stop

    // Quick stop: the last movement the game simulated & how fast a counter strafe slows us down,
    // to let go right when the speed reaches zero instead of overshooting into the other direction
    Vec3_t tick_velocity{};
    std::chrono::steady_clock::time_point tick_time{};
    float counter_decel = 0.f;  // Units per second per second, measured while counter strafing

    // Game input: a thread of ours in the game pushes the queued key events into SDL
    std::atomic<bool> game_input = false;
    uintptr_t input_block = 0;      // Code, its data & the event queue, in the game
    HANDLE input_wake = nullptr;    // Wakes that thread up for a new event
    uint32_t input_head = 0;
    std::mutex input_mutex;         // Keys come from the keyboard hook & the movement thread
    std::atomic<bool> stopping = false;
    bool jump_pressed = false;  // Bunny hop, our state of the jump button

    // Keyboard hook, W S A D
    std::atomic<bool> hooked = false;
    std::atomic<bool> held[4]{};    // Held on the keyboard
    bool nulled[4]{};               // Held, but let go for the opposite key pressed after it

    // Keyboard hook, order of the presses
    std::atomic<uint64_t> pressed_at[4]{};
    std::atomic<uint64_t> press_count = 0;
};
