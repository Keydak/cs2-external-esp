#pragma once
#include "core/memory/Memory.hpp"

// Accepts when a match is found, the button seen on a screenshot of the game window. With -insecure like the button
// does, from memory on the main thread of the game. Else a click on it posted to the window as messages: the cursor
// never moves either way
class AutoAccept {
public:
    ~AutoAccept()                            = default;
    AutoAccept(const AutoAccept&)            = delete;
    AutoAccept(AutoAccept&&)                 = delete;
    AutoAccept& operator=(const AutoAccept&) = delete;
    AutoAccept& operator=(AutoAccept&&)      = delete;

    static bool Init();
    static void Shutdown();
private:
    AutoAccept() {};

    static AutoAccept& GetInstance()
    {
        static AutoAccept i{};
        return i;
    }

    void Thread();

    // Center of the button in client coordinates of the window, when it is on screen
    bool FindButton(HWND window, POINT& center);
    bool Capture(HWND window, int width, int height, std::vector<uint32_t>& pixels);
    bool AcceptInMemory();
    // A click on the button sent to the window as messages, the cursor stays where it is
    void PostClick(HWND window, POINT center);

private:
    uintptr_t accept_code = 0;  // Calls the accept of the game, run by GameThread

private:
    std::atomic<bool> stopping = false;
    std::atomic<bool> running = false;
};
