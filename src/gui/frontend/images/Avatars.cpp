#include "Avatars.hpp"

#include "ImageCache.hpp"
#include "updater/http/HttpHelper.hpp"

namespace {
    constexpr uint64_t FIRST_STEAM_ID = 76561197960265728ull; // Bots have none

    // <avatarMedium><![CDATA[https://...]]></avatarMedium> of the profile XML
    std::string FindAvatar(const std::string& xml) {
        constexpr std::string_view open = "<avatarMedium><![CDATA[";

        auto start = xml.find(open);
        if (start == std::string::npos)
            return {};

        start += open.size();
        auto end = xml.find("]]>", start);
        if (end == std::string::npos || xml.compare(start, 8, "https://") != 0)
            return {};

        return xml.substr(start, end - start);
    }

    // The text of <name>...</name>, without its CDATA wrapping. Empty when it is not there
    std::string FindTag(const std::string& xml, const std::string& name) {
        auto open = "<" + name + ">";
        auto start = xml.find(open);
        if (start == std::string::npos)
            return {};

        start += open.size();
        auto end = xml.find("</" + name + ">", start);
        if (end == std::string::npos)
            return {};

        auto text = xml.substr(start, end - start);
        constexpr std::string_view cdata_open = "<![CDATA[", cdata_close = "]]>";
        if (text.starts_with(cdata_open) && text.ends_with(cdata_close))
            text = text.substr(cdata_open.size(), text.size() - cdata_open.size() - cdata_close.size());

        // Spaces & line breaks around it, the state message can end in a <br/>
        if (auto br = text.find("<br/>"); br != std::string::npos)
            text = text.substr(0, br) + " " + text.substr(br + 5);
        while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());
        return text;
    }

    SteamProfile ParseProfile(const std::string& xml) {
        SteamProfile profile;
        profile.loaded = true;
        profile.persona = FindTag(xml, "steamID");
        profile.real_name = FindTag(xml, "realname");
        profile.location = FindTag(xml, "location");
        profile.member_since = FindTag(xml, "memberSince");
        profile.state = FindTag(xml, "stateMessage");
        profile.privacy = FindTag(xml, "privacyState");
        profile.trade_ban = FindTag(xml, "tradeBanState");
        profile.avatar_full = FindTag(xml, "avatarFull");
        profile.vac_banned = FindTag(xml, "vacBanned") == "1";
        profile.limited = FindTag(xml, "isLimitedAccount") == "1";

        // <mostPlayedGame> blocks, Counter-Strike 2 is app 730
        size_t at = 0;
        while ((at = xml.find("<mostPlayedGame>", at)) != std::string::npos) {
            auto end = xml.find("</mostPlayedGame>", at);
            if (end == std::string::npos)
                break;

            auto game = xml.substr(at, end - at);
            at = end;

            auto link = FindTag(game, "gameLink");
            if (!link.ends_with("/app/730"))
                continue;

            profile.cs2_listed = true;
            profile.cs2_hours = FindTag(game, "hoursOnRecord");
            profile.cs2_recent_hours = FindTag(game, "hoursPlayed");
            break;
        }
        return profile;
    }
}

SteamProfile Avatars::GetProfile(uint64_t steam_id) {
    auto& i = GetInstance();
    i.GetImpl(steam_id);    // Queues the lookup when it was never asked for

    std::lock_guard<std::mutex> lock(i.mutex);
    auto it = i.entries.find(steam_id);
    return it != i.entries.end() ? it->second.profile : SteamProfile{};
}

ImTextureID Avatars::Get(uint64_t steam_id) {
    return GetInstance().GetImpl(steam_id);
}

ImTextureID Avatars::GetImpl(uint64_t steam_id) {
    if (steam_id < FIRST_STEAM_ID)
        return 0;

    std::string url;
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        if (!this->started) {
            this->started = true;
            std::thread(&Avatars::Worker, this).detach();
        }

        auto [it, inserted] = this->entries.try_emplace(steam_id);
        if (inserted) {
            this->queue.push_back(steam_id);
            this->wake.notify_one();
        }

        url = it->second.url;
    }

    return url.empty() ? 0 : ImageCache::Get(url);
}

void Avatars::Worker() {
    while (true) {
        uint64_t steam_id;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->wake.wait(lock, [this] { return !this->queue.empty(); });

            steam_id = this->queue.back();
            this->queue.pop_back();
        }

        std::string xml;
        std::string url;
        SteamProfile profile;
        profile.loaded = true;
        if (HttpHelper::GetRaw(std::format("https://steamcommunity.com/profiles/{}/?xml=1", steam_id), xml) == 200) {
            url = FindAvatar(xml);
            profile = ParseProfile(xml);
        }

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            auto& entry = this->entries[steam_id];
            entry.looked_up = true;
            entry.url = url;
            entry.profile = std::move(profile);
        }

        // Gentle with the profile pages
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}
