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
        if (HttpHelper::GetRaw(std::format("https://steamcommunity.com/profiles/{}/?xml=1", steam_id), xml) == 200)
            url = FindAvatar(xml);

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            auto& entry = this->entries[steam_id];
            entry.looked_up = true;
            entry.url = url;
        }

        // Gentle with the profile pages
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}
