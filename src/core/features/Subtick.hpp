#pragma once
#include "core/memory/Memory.hpp"

// Subtick air strafe. CreateMove builds the subtick move steps, the button state & move_crc of the command of a tick
// from the input events of that tick in CCSGOInput, all before the command is predicted & sent. We add our events
// there at the start of every tick in the air, when the player pressed nothing: the game makes them steps itself
class Subtick {
public:
    ~Subtick()                         = default;
    Subtick(const Subtick&)            = delete;
    Subtick(Subtick&&)                 = delete;
    Subtick& operator=(const Subtick&) = delete;
    Subtick& operator=(Subtick&&)      = delete;

    static bool Init();
    static bool IsAvailable();
    static void Shutdown();
private:
    Subtick() {};

    static Subtick& GetInstance()
    {
        static Subtick i{};
        return i;
    }

    bool InitImpl();
    void Thread();

public:
    // An input event of a tick in CCSGOInput, its view is the absolute one
    struct Event {
        float when;             // Part of the tick, 0 to 1
        uint32_t padding;
        uint64_t button;
        uint32_t pressed;       // bool, or the forward analog delta when button is 0
        float analog_left;
        float pitch;
        float yaw;
    };
    static_assert(sizeof(Event) == 0x20);

private:
    // The air of the server we play on, community servers change it
    void ReadConVars();
    // The sequence of the command of this tick, -1 without one
    int GetSequence();
    // Events for this tick, 0 for none
    int Plan(uintptr_t slot, Event* out);

private:
    bool available = false;
    uintptr_t convar_accelerate = 0, convar_wishspeed = 0;
    float air_accelerate = 12.f, air_max_wishspeed = 30.f;
    std::atomic<bool> stopping = false;
};
