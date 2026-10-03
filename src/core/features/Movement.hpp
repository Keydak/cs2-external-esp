#pragma once
#include "core/memory/Memory.hpp"

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
    static bool IsSliding();
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

    void Bhop();
    void SetJump(bool pressed);

    enum class Direction { Forward, Back, Left, Right };

    void QuickStop();
    void UpdateCounterStrafe(Direction direction, float speed_against);
    void SetCounterStrafe(Direction direction, bool pressed);
    void ReleaseCounterStrafe();

    void SlideWalk();
    void SetSlideKey(size_t key, bool down);
    void SendSlideKey(size_t key, bool down);

private:
    bool counter_strafing[4]{}; // Keys we are holding down to stop
    bool jump_pressed = false;  // Bunny hop, our state of the jump button

    // Keyboard hook, W S A D
    std::atomic<bool> hooked = false;
    std::atomic<bool> held[4]{};    // Held on the keyboard
    bool nulled[4]{};               // Held, but let go for the opposite key pressed after it

    // Slide walk, how each key differs from the keyboard
    enum class SlideState { None, Pressed, Released };
    std::atomic<SlideState> slide_state[4]{};
    std::chrono::steady_clock::time_point slide_start{};
    std::atomic<bool> sliding = false;
    bool slide_toggled = false;
    bool slide_key_was_down = false;
    bool slide_flipped[2]{};    // Speed pattern, per axis
    size_t slide_main[2]{};     // Direction slid towards, per axis

    // Keyboard hook, order of the presses
    std::atomic<uint64_t> pressed_at[4]{};
    std::atomic<uint64_t> press_count = 0;
};
