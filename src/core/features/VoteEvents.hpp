#pragma once
#include "core/memory/Memory.hpp"

// Who voted what. The vote controller only has the counts, every ballot comes as a vote_cast event.
// A listener object of ours is added to the game event manager: its FireGameEvent (code in our page, run on the
// main thread of the game) copies the fields of each vote event into a ring, a thread of ours reads them from there
class VoteEvents {
public:
    ~VoteEvents()                            = default;
    VoteEvents(const VoteEvents&)            = delete;
    VoteEvents(VoteEvents&&)                 = delete;
    VoteEvents& operator=(const VoteEvents&) = delete;
    VoteEvents& operator=(VoteEvents&&)      = delete;

    struct Event {
        bool cast = false;      // vote_cast, else vote_changed
        int option = -1;        // vote_cast: 0 yes, 1 no
        int team = -1;
        int slot = -1;          // vote_cast: who
        int yes = -1, no = -1, potential = -1; // vote_changed
        std::string player;     // Name of who cast it
        uint64_t steam_id = 0;  // & their profile
    };

    static bool Init();
    static bool IsAvailable();

    // Events since the last call, oldest first
    static std::vector<Event> Take();

    // Takes our listener out of the game
    static void Shutdown();
private:
    VoteEvents() {};

    static VoteEvents& GetInstance()
    {
        static VoteEvents i{};
        return i;
    }

    void Thread();
    bool Build();
    bool Register(bool add);
    void Drain();
    void ReadPlayer(int slot, Event& event);

private:
    uintptr_t page = 0;         // Listener, its vtable, the keys, code & the ring
    bool registered = false;
    uint32_t read = 0;          // Events of the ring we have taken

    std::mutex mtx;
    std::vector<Event> pending;

    std::atomic<bool> stopping = false;
};
