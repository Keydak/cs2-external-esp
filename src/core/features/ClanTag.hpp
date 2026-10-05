#pragma once
#include "core/memory/Memory.hpp"
#include <optional>

// Clan tag & name, as the game shows them to us. m_szClan of our controller points to our text, m_iszPlayerName
// holds our name, then the game cleans them itself on its main thread (the same functions as for what the server
// sends), which also tells the scoreboard. The tag goes in the clan slot, before or after the name, or replaces it

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

private:
    uintptr_t block = 0;        // Code & the text, in the game

    uintptr_t controller = 0;   // The one we changed, what it had
    uintptr_t original_clan = 0;
    std::string original_name;

    bool clan_active = false;   // m_szClan is ours
    bool name_active = false;   // The name is ours
    std::string clan_written;

    std::atomic<bool> stopping = false;
};
