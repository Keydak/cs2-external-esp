#pragma once
#include "core/memory/Memory.hpp"

#include <chrono>
#include <string>

// Matchmaking (-insecure), no mouse at all: no click, no cursor, the window left where it is.
// Accepts when a match is found (the button seen on a screenshot of the game window): through the popup of the game,
// its own accept as if its button was clicked, the popup shows it like after a click. Else from memory like the button
// does: the server takes it, the popup keeps its button & timer until the match loads. The game not in front: its
// taskbar button flashes until it is brought up.
// Cancel queue: an X in the top right corner of that popup, accepted or not, made in it by a script of Panorama. A button
// of the game like its cancel search, clicked by the player
class AutoAccept {
public:
    ~AutoAccept()                            = default;
    AutoAccept(const AutoAccept&)            = delete;
    AutoAccept(AutoAccept&&)                 = delete;
    AutoAccept& operator=(const AutoAccept&) = delete;
    AutoAccept& operator=(AutoAccept&&)      = delete;

    static bool Init();
    static void Shutdown();

    // -insecure & the accept of the game found
    static bool IsAvailable();

    // -insecure, the matchmaking status of the game found & scripts of Panorama
    static bool IsQueueAvailable();
private:
    AutoAccept() {};

    static AutoAccept& GetInstance()
    {
        static AutoAccept i{};
        return i;
    }

    void Thread();
    // While a match is found: the X in its popup, once in each. Every half second
    void UpdateCancel();
    // The status string of the menu, read on the main thread of the game. Empty out of the queue
    bool ReadQueueStatus(std::string& status);
    // The matchmaking took our accept
    bool Accepted();

    // Center of the button in client coordinates of the window, when it is on screen
    bool FindButton(HWND window, POINT& center);
    bool Capture(HWND window, int width, int height, std::vector<uint32_t>& pixels);
    bool AcceptInPopup();
    bool AcceptInMemory();

private:
    uintptr_t accept_code = 0;  // Calls the accept of the game, run by GameThread
    uintptr_t status_code = 0;  // Calls the status of the game, with its context & string, run by GameThread

    std::chrono::steady_clock::time_point next_cancel{};
    uintptr_t cancel_popup = 0; // The panel of the popup the X was put in

private:
    std::atomic<bool> stopping = false;
    std::atomic<bool> running = false;
};
