#pragma once
#include <condition_variable>

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
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::map<uint64_t, Entry> entries;
    std::vector<uint64_t> queue;
    bool started = false;
};
