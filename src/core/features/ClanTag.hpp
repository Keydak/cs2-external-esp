#pragma once
#include "core/memory/Memory.hpp"
#include <chrono>
#include <optional>
#include <string>

// Clan tag & name, as the game shows them to us. m_szClan of our controller points to our text, m_iszPlayerName
// holds our name, then the game cleans them itself on its main thread (the same functions as for what the server
// sends), which also tells the scoreboard. The tag goes in the clan slot, before or after the name, or replaces it.
//
// On the server too: the name (the tag in it) goes to the server like "setinfo name" does, the function of that
// command called on the main thread with our arguments, nothing typed in the console. setinfo only takes user info
// variables & "name" is not one: it is marked as one for that call only

class ClanTag {
public:
    ~ClanTag()                         = default;
    ClanTag(const ClanTag&)            = delete;
    ClanTag(ClanTag&&)                 = delete;
    ClanTag& operator=(const ClanTag&) = delete;
    ClanTag& operator=(ClanTag&&)      = delete;

    static bool Init();
    static bool IsAvailable();
    static bool IsNameAvailable();
    // The name & tag can be sent to the server
    static bool IsServerAvailable();

    // Puts the tag & name of the game back
    static void Shutdown();

    // Names of the animations, in the order of cfg::misc::clantag_mode
    static const std::vector<const char*>& GetModes();
private:
    ClanTag() {};

    static ClanTag& GetInstance()
    {
        static ClanTag i{};
        return i;
    }

    void Thread();

    // Steps of one round of the animation of a text
    size_t Cycle(size_t length, int mode);
    // The text at step "at" of its round, round picks the random letters
    std::string Frame(const std::string& text, int mode, size_t at, uint64_t round);
    // The tag at this step, texts separated by | take turns
    std::string Animate(const std::string& config, int mode, uint64_t step);
    std::string ReadName(uintptr_t controller);

    // m_szClan = clan & the name = name when given, then the game cleans them
    bool Apply(uintptr_t controller, std::optional<uintptr_t> clan, const std::string* name);
    void Restore();

    // The name to the server, through setinfo
    bool SendName(const std::string& name);

private:
    uintptr_t block = 0;        // Code & the text, in the game

    uintptr_t controller = 0;   // The one we changed, what it had
    uintptr_t original_clan = 0;
    std::string original_name;

    bool clan_active = false;   // m_szClan is ours
    bool name_active = false;   // The name is ours
    std::string clan_written;

    uintptr_t server_block = 0;     // setinfo: code, its arguments, the text
    uintptr_t name_convar = 0;      // ConVarData of "name"
    bool server_active = false;     // The server has a name of ours
    std::string server_sent;
    std::chrono::steady_clock::time_point server_last{};

    std::atomic<bool> stopping = false;
};
