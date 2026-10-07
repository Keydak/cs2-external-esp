#pragma once
#include <condition_variable>

// What the public Steam profile tells (its XML), whatever the privacy of the profile lets through
struct SteamProfile {
    bool loaded = false;        // Looked up, the fields below may still be empty
    std::string persona;        // Steam name
    std::string real_name;
    std::string location;
    std::string member_since;
    std::string state;          // "Online", "Offline", "In-Game ..."
    std::string privacy;        // "public", "private", "friendsonly"
    std::string trade_ban;      // "None" when there is none
    std::string avatar_full;    // URL of the big picture
    bool vac_banned = false;
    bool limited = false;

    // Counter-Strike 2, from the games of the last two weeks: Steam lists only those without a login
    bool cs2_listed = false;
    std::string cs2_hours;          // All time
    std::string cs2_recent_hours;   // The last two weeks
};

// Steam profile pictures by SteamID64. The picture is looked up once from the public profile (no API key needed),
// then downloaded & kept on disk by ImageCache
class Avatars {
public:
    ~Avatars()                         = default;
    Avatars(const Avatars&)            = delete;
    Avatars(Avatars&&)                 = delete;
    Avatars& operator=(const Avatars&) = delete;
    Avatars& operator=(Avatars&&)      = delete;

    // 0 while it is still loading, for bots & when the profile has none
    static ImTextureID Get(uint64_t steam_id);

    // The rest of the profile, looked up with the picture. Not loaded yet while it is
    static SteamProfile GetProfile(uint64_t steam_id);
private:
    Avatars() {};

    static Avatars& GetInstance()
    {
        static Avatars i{};
        return i;
    }

    ImTextureID GetImpl(uint64_t steam_id);
    void Worker();

    struct Entry {
        bool looked_up = false;
        std::string url;
        SteamProfile profile;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::map<uint64_t, Entry> entries;
    std::vector<uint64_t> queue;
    bool started = false;
};
