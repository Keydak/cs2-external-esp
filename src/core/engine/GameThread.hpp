#pragma once
#include "core/memory/Memory.hpp"

// Runs code of ours on the main thread of the game, between its own work.
// Calling into the game from a thread of our own races with rendering & physics, which crashes it.
//
// The vtable of the CCSGOInput object is copied into our memory with every entry going through a stub.
// The stub runs a queued function when it is called on the main thread, then continues to the real function.
class GameThread {
public:
    ~GameThread()                            = default;
    GameThread(const GameThread&)            = delete;
    GameThread(GameThread&&)                 = delete;
    GameThread& operator=(const GameThread&) = delete;
    GameThread& operator=(GameThread&&)      = delete;

    static bool Init();
    static bool IsAvailable();

    // Installs the hook when it is not yet (the input object might not have existed at startup), then IsAvailable()
    static bool Ensure();

    // Calls function() (no arguments, ends with ret) on the main thread and waits for it.
    // False when it did not run in time, then it is cancelled: it either already runs or never will.
    // Its code must still not be touched, the game might be inside it
    static bool Call(uintptr_t function, DWORD timeout_ms = 2000);

    // The real function of a CCSGOInput vtable entry, 0 without the hook
    static uintptr_t GetOriginal(size_t index);
    // Our copy of the vtable calls target for that entry instead (which calls the original itself), 0 puts our stub back
    static bool Redirect(size_t index, uintptr_t target);

    // Puts the real vtable back
    static void Shutdown();
private:
    GameThread() {};

    static GameThread& GetInstance()
    {
        static GameThread i{};
        return i;
    }

    bool InitImpl();
    void RetryInstall();
    uintptr_t FindLeftoverVtable(uintptr_t copy, uintptr_t client, size_t image_size);
    bool CallImpl(uintptr_t function, DWORD timeout_ms);
    bool WaitIdle(DWORD timeout_ms);
    uint32_t FindMainThread();

private:
    std::mutex mtx;

    uintptr_t page = 0;         // Job, stubs & the copied vtable
    uintptr_t object = 0;       // CCSGOInput
    uintptr_t vtable = 0;       // The real one
    uintptr_t copy = 0;         // Ours, first entry
    std::vector<uintptr_t> entries; // The real functions
    bool installed = false;
    std::chrono::steady_clock::time_point next_install{};
};
