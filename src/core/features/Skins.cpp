#include "Skins.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/engine/GameThread.hpp"
#include "updater/http/HttpHelper.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace {
    const std::string skins_url = "https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/skins.json";
    const std::string agents_url = "https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/agents.json";
    const std::string music_kits_url = "https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/music_kits.json";
    const std::string cache_file = "skins_cache.json";
    constexpr int CACHE_VERSION = 6; // Bumped when the cached fields change

    constexpr uint32_t INVALID_ITEM_ID = 0xFFFFFFFF; // Not an inventory item, the game skips the inventory lookup
    constexpr int QUALITY_UNUSUAL = 3; // The star quality of knives & gloves

    // Skins are built in the background, a rebuild while one is still going cancels it
    constexpr auto RETRY_DELAY = 2s;
    constexpr int MAX_ATTEMPTS = 5; // The weapon model might still be loading, then the game skips the skin
    constexpr size_t MAX_REBUILDS = 32; // Per call into the game, keeps the code within its page
    constexpr size_t CODE_SIZE = 0x2000;

    // CEconItemAttribute
    struct EconItemAttribute {
        uintptr_t vtable;
        uintptr_t owner;
        uint8_t pad_10[0x20];
        uint16_t definition_index;      // 0x30
        uint8_t pad_32[2];
        float value;                    // 0x34
        float initial_value;            // 0x38
        int32_t refundable_currency;    // 0x3C
        bool set_bonus;                 // 0x40
        uint8_t pad_41[7];
    };
    static_assert(sizeof(EconItemAttribute) == 0x48);

    // Attribute definitions read by the skin code of the game
    enum Attribute : uint16_t {
        PAINT_KIT = 6,
        PATTERN_SEED = 7,
        WEAR = 8,
    };
    constexpr int ATTRIBUTE_COUNT = 3;

    // CAttributeList::m_Attributes, a CUtlVector
    struct AttributeVector {
        int32_t size;
        int32_t pad;
        uintptr_t memory;
        int32_t allocated;
        uint32_t flags;
    };
    static_assert(sizeof(AttributeVector) == 0x18);

    // Marks the memory as owned by someone else, so the game never frees our attributes
    constexpr uint32_t EXTERNAL_BUFFER = 0x40000000;

    // The api mixes numbers & strings for some fields
    int JsonInt(const json& parent, const char* key) {
        auto it = parent.find(key);
        if (it == parent.end())
            return 0;

        if (it->is_number_integer())
            return it->get<int>();

        if (it->is_string()) {
            try { return std::stoi(it->get<std::string>()); }
            catch (...) { return 0; }
        }

        return 0;
    }

    float JsonFloat(const json& parent, const char* key, float def) {
        auto it = parent.find(key);
        return (it != parent.end() && it->is_number()) ? it->get<float>() : def;
    }

    std::string JsonString(const json& parent, const char* key) {
        auto it = parent.find(key);
        return (it != parent.end() && it->is_string()) ? it->get<std::string>() : "";
    }

    int CategoryOrder(const std::string& category) {
        static const char* order[] = { "Pistols", "Rifles", "SMGs", "Heavy", "Gloves", "Knives" };

        for (int i = 0; i < IM_ARRAYSIZE(order); i++)
            if (category == order[i])
                return i;

        return IM_ARRAYSIZE(order);
    }

    bool SameSkin(const cfg::skins::item_t& a, const cfg::skins::item_t& b) {
        return a.paint_kit == b.paint_kit && a.seed == b.seed && a.wear == b.wear;
    }

    // Not an item of the inventory any more but ours: the game takes the skin from our attributes
    void ClaimItem(uintptr_t item, uint32_t account_id) {
        auto p = Engine::GetProcess();
        if (p->read<uint32_t>(item + offsets::econ::m_iItemIDHigh) != INVALID_ITEM_ID)
            p->write<uint32_t>(item + offsets::econ::m_iItemIDHigh, INVALID_ITEM_ID);
        if (p->read<uint32_t>(item + offsets::econ::m_iAccountID) != account_id)
            p->write<uint32_t>(item + offsets::econ::m_iAccountID, account_id);
    }

    uintptr_t AttributeVectorOf(uintptr_t item) {
        return item + offsets::econ::m_AttributeList + offsets::econ::m_Attributes;
    }

    constexpr int KNIFE_DEFAULT_CT = 42;
    constexpr int KNIFE_DEFAULT_T = 59;

    // Folder & file name of the knife models, "weapons/models/knife/<name>/weapon_<name>.vmdl"
    const char* KnifeModelName(int index) {
        switch (index) {
        case KNIFE_DEFAULT_CT: return "knife_default_ct";
        case KNIFE_DEFAULT_T:  return "knife_default_t";
        case 500: return "knife_bayonet";
        case 503: return "knife_css";
        case 505: return "knife_flip";
        case 506: return "knife_gut";
        case 507: return "knife_karambit";
        case 508: return "knife_m9";
        case 509: return "knife_tactical";
        case 512: return "knife_falchion";
        case 514: return "knife_bowie";
        case 515: return "knife_butterfly";
        case 516: return "knife_push";
        case 517: return "knife_cord";
        case 518: return "knife_canis";
        case 519: return "knife_ursus";
        case 520: return "knife_navaja";
        case 521: return "knife_outdoor";
        case 522: return "knife_stiletto";
        case 523: return "knife_talon";
        case 525: return "knife_skeleton";
        case 526: return "knife_kukri";
        default: return nullptr;
        }
    }

    std::string KnifeModel(int index) {
        auto name = KnifeModelName(index);
        return name ? std::format("weapons/models/knife/{0}/weapon_{0}.vmdl", name) : "";
    }

    // CUtlStringToken, MurmurHash2 of the lower case string. Weapons use their definition index as the subclass name
    uint32_t StringToken(const std::string& text) {
        constexpr uint32_t seed = 0x31415926;
        constexpr uint32_t m = 0x5BD1E995;

        auto length = static_cast<uint32_t>(text.size());
        uint32_t h = seed ^ length;
        size_t i = 0;

        auto byte = [&](size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(text[at])))); };

        for (; length >= 4; i += 4, length -= 4) {
            uint32_t k = byte(i) | byte(i + 1) << 8 | byte(i + 2) << 16 | byte(i + 3) << 24;
            k *= m;
            k ^= k >> 24;
            k *= m;
            h *= m;
            h ^= k;
        }

        switch (length) {
        case 3: h ^= byte(i + 2) << 16; [[fallthrough]];
        case 2: h ^= byte(i + 1) << 8;  [[fallthrough]];
        case 1: h ^= byte(i); h *= m;
        }

        h ^= h >> 13;
        h *= m;
        h ^= h >> 15;
        return h;
    }
}

const SkinInfo* ItemInfo::FindSkin(int paint_kit) const {
    for (const auto& skin : skins)
        if (skin.paint_kit == paint_kit)
            return &skin;

    return nullptr;
}

bool Skins::Init() {
    return GetInstance().InitImpl();
}

bool Skins::IsAvailable() {
    return Engine::IsInsecure() && offsets::skins::updateWeaponSkin;
}

bool Skins::IsLoaded() {
    return GetInstance().loaded;
}

bool Skins::HasFailed() {
    return GetInstance().failed;
}

const std::vector<ItemInfo>& Skins::GetItems() {
    return GetInstance().items;
}

const ItemInfo* Skins::FindItem(int definition_index) {
    if (!IsLoaded())
        return nullptr;

    for (const auto& item : GetItems())
        if (item.definition_index == definition_index)
            return &item;

    return nullptr;
}

const std::vector<AgentInfo>& Skins::GetAgents() {
    return GetInstance().agents;
}

const AgentInfo* Skins::FindAgent(int definition_index) {
    if (!IsLoaded())
        return nullptr;

    for (const auto& agent : GetAgents())
        if (agent.definition_index == definition_index)
            return &agent;

    return nullptr;
}

const std::vector<MusicKitInfo>& Skins::GetMusicKits() {
    return GetInstance().music_kits;
}

const MusicKitInfo* Skins::FindMusicKit(int definition_index) {
    if (!IsLoaded())
        return nullptr;

    for (const auto& kit : GetMusicKits())
        if (kit.definition_index == definition_index)
            return &kit;

    return nullptr;
}

bool Skins::IsGlove(int index) {
    return index == 4725 || (index >= 5027 && index <= 5035);
}

bool Skins::IsKnife(int index) {
    return KnifeModelName(index) != nullptr;
}

bool Skins::InitImpl() {
    // The list is useful for the menu even without -insecure
    std::thread(&Skins::Load, this).detach();

    if (!IsAvailable())
        return false;

    std::thread(&Skins::Thread, this).detach();

    LOGF(INFO, "Successfully initialized skins...");
    return true;
}

void Skins::Load() {
    json response;
    auto http_status = HttpHelper::Get(skins_url, response);

    if (http_status != 200 || !response.is_array()) {
        if (LoadCache()) {
            LOGF(WARNING, "Failed to fetch skin list (status {}), using the cached one", http_status);
            return;
        }

        LOGF(WARNING, "Failed to fetch skin list (status {}), paint kits can still be set by id", http_status);
        this->failed = true;
        return;
    }

    std::map<int, ItemInfo> by_index;

    for (const auto& entry : response) {
        auto weapon = entry.find("weapon");
        if (weapon == entry.end() || !weapon->is_object())
            continue;

        int index = JsonInt(*weapon, "weapon_id");
        int paint_kit = JsonInt(entry, "paint_index");
        if (!index)
            continue;

        std::string category;
        if (auto c = entry.find("category"); c != entry.end() && c->is_object())
            category = JsonString(*c, "name");

        // Only the knives we have a model for
        if (category == "Knives" && !IsKnife(index))
            continue;

        // The plain knife, without a skin
        if (!paint_kit) {
            if (category == "Knives") {
                auto& item = by_index[index];
                item.definition_index = index;
                item.name = JsonString(*weapon, "name");
                item.category = category;
                item.terrorist = item.counter_terrorist = true;
                item.image = JsonString(entry, "image");
            }

            continue;
        }

        // "AK-47 | Redline" -> "Redline"
        auto full_name = JsonString(entry, "name");
        auto separator = full_name.find(" | ");
        if (separator == std::string::npos)
            continue;

        auto& item = by_index[index];
        if (!item.definition_index) {
            item.definition_index = index;
            item.name = JsonString(*weapon, "name");
            item.category = category;
        }

        // Every team gets a knife
        if (category == "Knives")
            item.terrorist = item.counter_terrorist = true;

        SkinInfo skin;
        skin.name = full_name.substr(separator + 3);
        skin.paint_kit = paint_kit;
        skin.min_float = JsonFloat(entry, "min_float", 0.f);
        skin.max_float = JsonFloat(entry, "max_float", 1.f);
        skin.image = JsonString(entry, "image");

        if (auto legacy = entry.find("legacy_model"); legacy != entry.end() && legacy->is_boolean())
            skin.legacy_model = legacy->get<bool>();

        // "#eb4b4b"
        if (auto rarity = entry.find("rarity"); rarity != entry.end() && rarity->is_object()) {
            auto color = JsonString(*rarity, "color");
            if (color.size() == 7 && color[0] == '#') {
                try { skin.rarity_color = static_cast<uint32_t>(std::stoul(color.substr(1), nullptr, 16)); }
                catch (...) {}
            }
        }

        // The weapon belongs to the teams its skins are for
        if (auto team = entry.find("team"); team != entry.end() && team->is_object()) {
            auto id = JsonString(*team, "id");
            item.terrorist |= id != "counter-terrorists";
            item.counter_terrorist |= id != "terrorists";
        }

        item.skins.push_back(skin);
    }

    std::vector<ItemInfo> result;
    result.reserve(by_index.size());

    for (auto& [index, item] : by_index) {
        std::sort(item.skins.begin(), item.skins.end(), [](const SkinInfo& a, const SkinInfo& b) { return a.name < b.name; });
        result.push_back(std::move(item));
    }

    std::sort(result.begin(), result.end(), [](const ItemInfo& a, const ItemInfo& b) {
        auto order_a = CategoryOrder(a.category), order_b = CategoryOrder(b.category);
        return order_a != order_b ? order_a < order_b : a.name < b.name;
    });

    this->items = std::move(result);
    LoadAgents();
    LoadMusicKits();
    this->loaded = true;

    SaveCache();
    LOGF(INFO, "Loaded {} skinnable items", this->items.size());
}

void Skins::LoadAgents() {
    json response;
    if (HttpHelper::Get(agents_url, response) != 200 || !response.is_array()) {
        LOGF(WARNING, "Failed to fetch the agent list");
        return;
    }

    std::vector<AgentInfo> result;

    for (const auto& entry : response) {
        AgentInfo agent;
        agent.definition_index = JsonInt(entry, "def_index");
        agent.model = JsonString(entry, "model_player");
        agent.image = JsonString(entry, "image");

        if (!agent.definition_index || agent.model.empty())
            continue;

        // "Bloody Darryl The Strapped | The Professionals"
        auto full_name = JsonString(entry, "name");
        auto separator = full_name.find(" | ");
        agent.name = full_name.substr(0, separator);
        agent.group = separator != std::string::npos ? full_name.substr(separator + 3) : "";

        if (auto team = entry.find("team"); team != entry.end() && team->is_object())
            agent.terrorist = JsonString(*team, "id") == "terrorists";

        if (auto rarity = entry.find("rarity"); rarity != entry.end() && rarity->is_object()) {
            auto color = JsonString(*rarity, "color");
            if (color.size() == 7 && color[0] == '#') {
                try { agent.rarity_color = static_cast<uint32_t>(std::stoul(color.substr(1), nullptr, 16)); }
                catch (...) {}
            }
        }

        result.push_back(std::move(agent));
    }

    std::sort(result.begin(), result.end(), [](const AgentInfo& a, const AgentInfo& b) {
        return a.group != b.group ? a.group < b.group : a.name < b.name;
    });

    this->agents = std::move(result);
}

void Skins::LoadMusicKits() {
    json response;
    if (HttpHelper::Get(music_kits_url, response) != 200 || !response.is_array()) {
        LOGF(WARNING, "Failed to fetch the music kit list");
        return;
    }

    std::vector<MusicKitInfo> result;
    std::set<int> seen;

    for (const auto& entry : response) {
        MusicKitInfo kit;
        kit.definition_index = JsonInt(entry, "def_index");
        kit.image = JsonString(entry, "image");

        if (auto original = entry.find("original"); original != entry.end() && original->is_object())
            kit.code_name = JsonString(*original, "name");

        // Every kit is listed again as StatTrak, same music
        auto name = JsonString(entry, "name");
        if (!kit.definition_index || name.starts_with("StatTrak") || !seen.insert(kit.definition_index).second)
            continue;

        // "Music Kit | Daniel Sadowski, Crimson Assault" -> "Daniel Sadowski, Crimson Assault"
        constexpr std::string_view prefix = "Music Kit | ";
        kit.name = name.starts_with(prefix) ? name.substr(prefix.size()) : name;

        if (auto rarity = entry.find("rarity"); rarity != entry.end() && rarity->is_object()) {
            auto color = JsonString(*rarity, "color");
            if (color.size() == 7 && color[0] == '#') {
                try { kit.rarity_color = static_cast<uint32_t>(std::stoul(color.substr(1), nullptr, 16)); }
                catch (...) {}
            }
        }

        result.push_back(std::move(kit));
    }

    std::sort(result.begin(), result.end(), [](const MusicKitInfo& a, const MusicKitInfo& b) { return a.name < b.name; });
    this->music_kits = std::move(result);
}

bool Skins::LoadCache() {
    std::ifstream f(cache_file);
    if (!f.good())
        return false;

    try {
        auto data = json::parse(f);
        if (data.value("version", 0) != CACHE_VERSION)
            return false;

        std::vector<ItemInfo> result;

        for (const auto& entry : data["items"]) {
            ItemInfo item;
            item.definition_index = entry.value("index", 0);
            item.name = entry.value("name", "");
            item.category = entry.value("category", "");
            item.terrorist = entry.value("t", true);
            item.counter_terrorist = entry.value("ct", true);
            item.image = entry.value("image", "");

            for (const auto& s : entry["skins"]) {
                SkinInfo skin;
                skin.name = s.value("name", "");
                skin.paint_kit = s.value("paint_kit", 0);
                skin.min_float = s.value("min", 0.f);
                skin.max_float = s.value("max", 1.f);
                skin.legacy_model = s.value("legacy", false);
                skin.image = s.value("image", "");
                skin.rarity_color = s.value("rarity", 0u);
                item.skins.push_back(skin);
            }

            result.push_back(std::move(item));
        }

        if (result.empty())
            return false;

        std::vector<AgentInfo> agents;
        for (const auto& entry : data["agents"]) {
            AgentInfo agent;
            agent.definition_index = entry.value("index", 0);
            agent.name = entry.value("name", "");
            agent.group = entry.value("group", "");
            agent.terrorist = entry.value("t", false);
            agent.model = entry.value("model", "");
            agent.image = entry.value("image", "");
            agent.rarity_color = entry.value("rarity", 0u);
            agents.push_back(std::move(agent));
        }

        std::vector<MusicKitInfo> music_kits;
        for (const auto& entry : data["music_kits"]) {
            MusicKitInfo kit;
            kit.definition_index = entry.value("index", 0);
            kit.name = entry.value("name", "");
            kit.code_name = entry.value("code", "");
            kit.image = entry.value("image", "");
            kit.rarity_color = entry.value("rarity", 0u);
            music_kits.push_back(std::move(kit));
        }

        this->items = std::move(result);
        this->agents = std::move(agents);
        this->music_kits = std::move(music_kits);
        this->loaded = true;
        return true;
    }
    catch (...) {
        return false;
    }
}

void Skins::SaveCache() {
    json data;
    data["version"] = CACHE_VERSION;
    data["items"] = json::array();

    for (const auto& item : this->items) {
        json entry;
        entry["index"] = item.definition_index;
        entry["name"] = item.name;
        entry["category"] = item.category;
        entry["t"] = item.terrorist;
        entry["ct"] = item.counter_terrorist;
        entry["image"] = item.image;
        entry["skins"] = json::array();

        for (const auto& skin : item.skins)
            entry["skins"].push_back({
                { "name", skin.name },
                { "paint_kit", skin.paint_kit },
                { "min", skin.min_float },
                { "max", skin.max_float },
                { "legacy", skin.legacy_model },
                { "image", skin.image },
                { "rarity", skin.rarity_color },
            });

        data["items"].push_back(entry);
    }

    data["agents"] = json::array();
    for (const auto& agent : this->agents)
        data["agents"].push_back({
            { "index", agent.definition_index },
            { "name", agent.name },
            { "group", agent.group },
            { "t", agent.terrorist },
            { "model", agent.model },
            { "image", agent.image },
            { "rarity", agent.rarity_color },
        });

    data["music_kits"] = json::array();
    for (const auto& kit : this->music_kits)
        data["music_kits"].push_back({
            { "index", kit.definition_index },
            { "name", kit.name },
            { "code", kit.code_name },
            { "image", kit.image },
            { "rarity", kit.rarity_color },
        });

    std::ofstream f(cache_file);
    f << data.dump();
}

void Skins::Shutdown() {
    auto& skins = GetInstance();

    // Stop the thread first, it would patch again
    skins.stopping = true;
    std::this_thread::sleep_for(150ms);

    skins.SetGlovePreload(false);
    skins.PatchGloveRemoval(false);
    skins.ApplyMusicKit(0);
}

void Skins::Thread() {
    while (!this->stopping) {
        Apply();
        std::this_thread::sleep_for(100ms);
    }
}

void Skins::Apply() {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (!p)
        return;

    // Also while dead, the MVP anthem plays at the end of the round
    {
        int music_kit;
        bool enabled;
        {
            std::lock_guard<std::mutex> lock(cfg::skins::mutex);
            music_kit = cfg::skins::music_kit;
            enabled = cfg::skins::enabled;
        }
        ApplyMusicKit(enabled ? music_kit : 0);
    }

    auto pawn = Engine::GetLocalPawn();
    bool alive = pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) > 0;

    // Also while dead, or the spawn after it is not seen
    TrackSpawn(pawn, alive);

    if (!alive)
        return;

    if (this->skins_started == std::chrono::steady_clock::time_point{})
        this->skins_started = std::chrono::steady_clock::now();

    // Work on a copy, so the menu is not blocked while we write
    bool enabled;
    bool hide_third_person;
    int glove;
    int agent;
    int knife;
    std::map<int, cfg::skins::item_t> wanted;
    {
        std::lock_guard<std::mutex> lock(cfg::skins::mutex);
        enabled = cfg::skins::enabled;
        hide_third_person = cfg::skins::glove_hide_third_person;

        // The loadout of the team we play in
        auto team = p->read<uint8_t>(pawn + offsets::pawn::m_iTeamNum);
        const auto& loadout = cfg::skins::loadouts[team == 3 ? cfg::skins::COUNTER_TERRORIST : cfg::skins::TERRORIST];

        glove = loadout.glove;
        agent = loadout.agent;
        knife = loadout.knife;
        wanted = loadout.items;
    }

    // Turned off, everything goes back to default
    if (!enabled) {
        if (this->weapons.empty() && !this->gloves.block && this->agent_applied.empty() && this->knives.empty())
            return;

        glove = 0;
        agent = 0;
        knife = 0;
        wanted.clear();
    }

    auto controller = p->read<uintptr_t>(client.base + offsets::localPlayerController);
    auto account_id = static_cast<uint32_t>(p->read<uint64_t>(controller + offsets::controller::m_steamID));

    auto now = std::chrono::steady_clock::now();
    std::vector<uintptr_t> live, rebuild;
    std::vector<MeshMask> masks;

    auto weapon_services = p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices);
    auto count = weapon_services ? p->read<int>(weapon_services + offsets::econ::m_hMyWeapons) : 0;
    auto handles = weapon_services ? p->read<uintptr_t>(weapon_services + offsets::econ::m_hMyWeapons + 0x8) : 0;

    for (int i = 0; handles && i < std::min(count, 64); i++) {
        auto weapon = Engine::GetEntityFromHandle(p->read<uint32_t>(handles + i * sizeof(uint32_t)));
        if (!weapon)
            continue;

        live.push_back(weapon);

        auto item = weapon + offsets::pawn::m_AttributeManager + offsets::pawn::m_Item;
        int index = p->read<uint16_t>(item + offsets::pawn::m_iItemDefinitionIndex);

        // Changes the index to the new knife, its skin is looked up below
        bool model_changed = false;
        bool knife_changed = ApplyKnife(pawn, weapon, item, index, knife, model_changed);

        auto want = wanted.find(index);
        bool has_skin = want != wanted.end() && want->second.paint_kit > 0;

        auto it = this->weapons.find(weapon);

        // Same address but another entity, or the game replaced the attributes
        if (it != this->weapons.end() && (it->second.item != item || !IsAttached(it->second))) {
            p->free_remote(it->second.block);
            it = this->weapons.erase(it);
            it = this->weapons.end();
        }

        // The knife changed again (the server sent the one of the game after our change): a skin made anew, from new
        // attributes. The old ones only ever gave the knife of the second change no material
        std::optional<uint32_t> original_id_high;
        if (knife_changed && it != this->weapons.end()) {
            original_id_high = it->second.original_id_high;
            Detach(it->second);
            p->free_remote(it->second.block);
            this->weapons.erase(it);
            it = this->weapons.end();
        }

        if (has_skin) {
            if (it == this->weapons.end()) {
                Applied applied{};
                applied.item = item;
                applied.skin = want->second;

                if (!Attach(applied))
                    continue;

                applied.original_id_high = original_id_high.value_or(p->read<uint32_t>(item + offsets::econ::m_iItemIDHigh));
                ClaimItem(item, account_id);

                it = this->weapons.emplace(weapon, applied).first;
                rebuild.push_back(weapon);

                LOGF(VERBOSE, "Attached paint kit {} to item {}", applied.skin.paint_kit, index);
            }
            else if (knife_changed || !SameSkin(it->second.skin, want->second)) {
                it->second.skin = want->second;
                it->second.built = false;
                it->second.attempts = 0;

                // The game puts its own knife back with an update from the server right after a spawn, the item id
                // with it: the item of the inventory again, without our skin, until it is ours once more
                ClaimItem(item, account_id);
                WriteAttributes(it->second);
                rebuild.push_back(weapon);
            }
            else if (!it->second.built && it->second.attempts < MAX_ATTEMPTS && now >= it->second.next_try) {
                // Might have finished since the last try
                if (CountMaterials(weapon) > 0)
                    it->second.built = true;
                else {
                    ClaimItem(item, account_id);
                    rebuild.push_back(weapon);
                }
            }

            // Skins made for the old model need the old mesh
            bool legacy = false;
            if (auto info = FindItem(index))
                if (auto skin = info->FindSkin(want->second.paint_kit))
                    legacy = skin->legacy_model;

            // A model set just now is still loading, the game crashes switching its mesh. The skin made again once the
            // model is there sets it then
            bool rebuilding = std::find(rebuild.begin(), rebuild.end(), weapon) != rebuild.end();
            if (!model_changed)
                CollectMeshMasks(pawn, weapon, legacy ? 2 : 1, rebuilding, masks);
        }
        else if (it != this->weapons.end()) {
            // Skin removed, back to the default look
            Detach(it->second);
            p->write<uint32_t>(item + offsets::econ::m_iItemIDHigh, it->second.original_id_high);
            p->free_remote(it->second.block);
            this->weapons.erase(it);

            if (!model_changed)
                CollectMeshMasks(pawn, weapon, 1, true, masks);
            rebuild.push_back(weapon);
        }
    }

    // Knives of weapons that are gone
    for (auto it = this->knives.begin(); it != this->knives.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end())
            it = this->knives.erase(it);
        else
            ++it;
    }
    for (auto it = this->knife_seen.begin(); it != this->knife_seen.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end())
            it = this->knife_seen.erase(it);
        else
            ++it;
    }

    // Dropped weapons keep their skin, forget them once the game let go of our attributes
    for (auto it = this->weapons.begin(); it != this->weapons.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end() && !IsAttached(it->second)) {
            p->free_remote(it->second.block);
            it = this->weapons.erase(it);
        }
        else {
            ++it;
        }
    }

    if (!rebuild.empty() || !masks.empty()) {
        Rebuild(rebuild, masks);

        for (const auto& m : masks)
            LOGF(VERBOSE, "Set mesh group mask of node 0x{:X} to {}", m.node, m.mask);

        for (auto weapon : rebuild) {
            auto it = this->weapons.find(weapon);
            if (it == this->weapons.end())
                continue;

            it->second.built = CountMaterials(weapon) > 0;
            it->second.attempts++;
            it->second.next_try = now + RETRY_DELAY;

            LOGF(VERBOSE, "Rebuilt skin of 0x{:X}, materials {}, attempt {}", weapon, CountMaterials(weapon), it->second.attempts);
        }
    }

    ApplyGloves(pawn, account_id, glove, wanted);
    HideThirdPersonGloves(pawn, this->gloves.block && hide_third_person);
    auto info = agent ? FindAgent(agent) : nullptr;
    ApplyAgent(pawn, info ? info->model : "");

}

void Skins::ApplyGloves(uintptr_t pawn, uint32_t account_id, int glove, const std::map<int, cfg::skins::item_t>& wanted) {
    auto p = Engine::GetProcess();
    auto item = pawn + offsets::econ::m_EconGloves;

    // New pawn after a reconnect, the old one is gone
    if (this->gloves.block && this->gloves_pawn != pawn) {
        this->gloves = {};
        this->gloves_pawn = 0;
    }

    cfg::skins::item_t skin{};
    if (auto want = wanted.find(glove); want != wanted.end())
        skin = want->second;

    if (!glove || !skin.paint_kit) {
        if (glove && !this->gloves.block) {
            static int reported = 0;
            if (reported != glove) {
                reported = glove;
                LOGF(VERBOSE, "Gloves {} have no skin selected, pick one in the item list", glove);
            }
        }

        if (!this->gloves.block)
            return;

        // Back to the default gloves
        Detach(this->gloves);
        p->free_remote(this->gloves.block);

        p->write<uint16_t>(item + offsets::pawn::m_iItemDefinitionIndex, static_cast<uint16_t>(this->gloves.original_index));
        p->write<uint32_t>(item + offsets::econ::m_iItemIDHigh, this->gloves.original_id_high);
        ReapplyGloves(pawn);
        ShowDefaultGloves(pawn, true);
        SetGlovePreload(false);
        PatchGloveRemoval(false);
        this->gloves_retries = 0;

        this->gloves = {};
        this->gloves_pawn = 0;

        LOGF(VERBOSE, "Removed custom gloves");
        return;
    }

    // The game resets the gloves on every spawn
    int index = p->read<uint16_t>(item + offsets::pawn::m_iItemDefinitionIndex);
    bool attached = this->gloves.block && IsAttached(this->gloves);

    if (attached && index == glove && SameSkin(this->gloves.skin, skin)) {
        // Try again until the glove model is loaded & the gloves exist
        auto now = std::chrono::steady_clock::now();
        if (this->gloves_retries > 0 && now >= this->gloves_next_try) {
            if (GetGloveEntity(pawn)) {
                this->gloves_retries = 0;
                SetGlovePreload(false);

                // Some player models come with gloves, ours go over them
                ShowDefaultGloves(pawn, false);
                LOGF(VERBOSE, "Gloves {} are on", glove);
            }
            else if (--this->gloves_retries == 0) {
                SetGlovePreload(false);
                LOGF(WARNING, "Gloves {} did not show up, the glove model might not be loadable", glove);
            }
            else {
                ReapplyGloves(pawn);
                this->gloves_next_try = now + 3s;
            }
        }
        return;
    }

    if (!attached) {
        if (this->gloves.block)
            p->free_remote(this->gloves.block);

        Applied applied{};
        applied.item = item;
        applied.skin = skin;
        applied.original_index = index;
        applied.original_id_high = p->read<uint32_t>(item + offsets::econ::m_iItemIDHigh);

        if (!Attach(applied)) {
            static uintptr_t reported = 0;
            if (reported != item) {
                reported = item;
                LOGF(VERBOSE, "Could not attach the glove attributes to 0x{:X}", item);
            }
            return;
        }

        this->gloves = applied;
        this->gloves_pawn = pawn;
    }
    else {
        this->gloves.skin = skin;
        WriteAttributes(this->gloves);
    }

    p->write<bool>(item + offsets::econ::m_bInitialized, false);
    p->write<uint16_t>(item + offsets::pawn::m_iItemDefinitionIndex, static_cast<uint16_t>(glove));
    p->write<uint32_t>(item + offsets::econ::m_iItemIDHigh, INVALID_ITEM_ID);
    p->write<uint32_t>(item + offsets::econ::m_iItemIDHigh + 0x4, INVALID_ITEM_ID); // Low
    p->write<int>(item + offsets::econ::m_iEntityQuality, QUALITY_UNUSUAL);
    p->write<uint32_t>(item + offsets::econ::m_iAccountID, account_id);
    p->write<bool>(item + offsets::econ::m_bInitialized, true);

    PatchGloveRemoval(true);
    SetGlovePreload(true);
    ReapplyGloves(pawn);
    this->gloves_retries = 3;
    this->gloves_next_try = std::chrono::steady_clock::now() + 3s;

    LOGF(VERBOSE, "Applied gloves {} with paint kit {}", glove, skin.paint_kit);
}

// Our kit goes everywhere the game reads it from, also while dead: the MVP anthem plays at the end of the round
void Skins::ApplyMusicKit(int music_kit) {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    // The main menu has no controller
    ApplyMenuMusic(music_kit);

    auto controller = p->read<uintptr_t>(Engine::GetClient().base + offsets::localPlayerController);

    // A new controller (map change) starts with what the game gave it
    if (controller != this->music_controller) {
        this->music_controller = controller;
        for (auto& target : this->music_targets)
            target = {};
        this->server_entities = 0;
    }

    if (!controller)
        return;

    // Read by the scoreboard
    ApplyMusicTarget(this->music_targets[0], controller + offsets::econ::m_iMusicKitID, true, music_kit);

    // Picks the music of the round start, bomb, round end... for us
    if (auto services = p->read<uintptr_t>(controller + offsets::econ::m_pInventoryServices))
        ApplyMusicTarget(this->music_targets[1], services + offsets::econ::m_unMusicID, false, music_kit);

    // The MVP anthem comes with the event of the server, only there when the game hosts it (offline)
    if (music_kit || this->music_targets[2].applied) {
        if (auto server_controller = GetServerController(controller))
            ApplyMusicTarget(this->music_targets[2], server_controller + offsets::econ::server_m_iMusicKitID, true, music_kit);
    }
}

// The main menu plays the kit of the inventory, unless a kit name is in its override (the store previews kits so)
void Skins::ApplyMenuMusic(int music_kit) {
    auto p = Engine::GetProcess();
    if (!offsets::econ::dwMenuMusic || !offsets::econ::m_pszMenuMusicOverride)
        return;

    constexpr size_t NAME_SIZE = 64;
    auto address = Engine::GetClient().base + offsets::econ::dwMenuMusic + offsets::econ::m_pszMenuMusicOverride;
    auto current = p->read<uintptr_t>(address);

    auto ours = [&](uintptr_t pointer) {
        return this->menu_music_names && (pointer == this->menu_music_names || pointer == this->menu_music_names + NAME_SIZE);
    };

    const MusicKitInfo* kit = music_kit ? FindMusicKit(music_kit) : nullptr;

    if (!kit || kit->code_name.empty() || kit->code_name.size() >= NAME_SIZE) {
        // Removed: the inventory one plays again
        if (ours(current))
            p->write<uintptr_t>(address, 0);
        this->menu_music_written.clear();
        return;
    }

    if (!this->menu_music_names && !(this->menu_music_names = p->allocate_remote(NAME_SIZE * 2)))
        return;

    // Another kit: written to the other name, the game restarts the music when the pointer changes
    if (kit->code_name != this->menu_music_written) {
        this->menu_music_slot ^= 1;
        auto name = this->menu_music_names + NAME_SIZE * this->menu_music_slot;
        p->write_bytes(name, std::vector<uint8_t>(kit->code_name.c_str(), kit->code_name.c_str() + kit->code_name.size() + 1));
        this->menu_music_written = kit->code_name;
    }

    // Left alone while the store previews a kit, ours again once it is done
    auto name = this->menu_music_names + NAME_SIZE * this->menu_music_slot;
    if (current != name && (!current || ours(current)))
        p->write<uintptr_t>(address, name);
}

void Skins::ApplyMusicTarget(MusicTarget& target, uintptr_t address, bool wide, int music_kit) {
    auto p = Engine::GetProcess();

    // Somewhere else now, what we wrote at the old place is gone with it
    if (target.address != address)
        target = { address, wide };

    auto read = [&]() { return wide ? p->read<int32_t>(address) : int(p->read<uint16_t>(address)); };
    auto write = [&](int value) {
        if (wide)
            p->write<int32_t>(address, value);
        else
            p->write<uint16_t>(address, uint16_t(value));
    };

    int current = read();

    if (music_kit) {
        if (!target.applied)
            target.original = current;

        if (current != music_kit)
            write(music_kit);

        target.applied = true;
        target.written = music_kit;
        return;
    }

    // Removed: back to the one of the game, unless the game changed it since
    if (target.applied) {
        if (current == target.written)
            write(target.original);
        target.applied = false;
    }
}

// Our controller in server.dll, by the same entity index
uintptr_t Skins::GetServerController(uintptr_t controller) {
    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();

    if ((!this->server_entities || !offsets::econ::server_m_iMusicKitID) && now >= this->server_next_search) {
        this->server_next_search = now + 5s;

        Dumper::ResolveServer();
        if (auto resources = p->FindInterface("engine2.dll", "GameResourceServiceServerV001"))
            this->server_entities = p->read<uintptr_t>(resources + 0x58);
    }

    if (!this->server_entities || !offsets::econ::server_m_iMusicKitID)
        return 0;

    auto identity = p->read<uintptr_t>(controller + 0x10);
    auto index = p->read<uint32_t>(identity + 0x10) & 0x7FFF;
    if (!identity || !index || index == 0x7FFF)
        return 0;

    auto chunk = p->read<uintptr_t>(this->server_entities + 0x10 + 8 * (index >> 9));
    auto server_controller = chunk ? p->read<uintptr_t>(chunk + 0x70 * (index & 0x1FF)) : 0;
    if (!server_controller)
        return 0;

    // The entity system is made again with every map, check it is still a controller
    char name[24]{};
    p->read_raw(p->read<uintptr_t>(p->read<uintptr_t>(server_controller + 0x10) + offsets::grenade::m_designerName), name, sizeof(name) - 1);
    if (std::string_view(name) != "cs_player_controller") {
        this->server_entities = 0;
        return 0;
    }

    return server_controller;
}

// Asks the game to rebuild the gloves from the item. Glove models nobody owns are only loaded
// while the player preview flag is set, so it is turned on until the game handled the request
void Skins::ReapplyGloves(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    p->write<bool>(pawn + offsets::econ::m_bNeedToReApplyGloves, true);

    // Handled on the next frame
    for (int i = 0; i < 100 && p->read<bool>(pawn + offsets::econ::m_bNeedToReApplyGloves); i++)
        std::this_thread::sleep_for(2ms);
}

// Agent models can say they come with their own gloves, the game removes any other gloves then
void Skins::PatchGloveRemoval(bool patched) {
    if (!offsets::skins::gloveRemoveJump || patched == this->glove_patched)
        return;

    auto p = Engine::GetProcess();
    auto address = Engine::GetClient().base + offsets::skins::gloveRemoveJump;

    constexpr uint8_t JE = 0x74, JMP = 0xEB;
    auto current = p->read<uint8_t>(address);

    // Already like that, e.g. left patched by a run that was closed by force
    if (current == (patched ? JMP : JE)) {
        this->glove_patched = patched;
        return;
    }

    if (current != (patched ? JE : JMP))
        return;

    uint8_t opcode = patched ? JMP : JE;
    if (p->patch_code(address, &opcode, sizeof(opcode))) {
        this->glove_patched = patched;
        LOGF(VERBOSE, "Glove removal check {}", patched ? "patched" : "restored");
    }
}

std::string Skins::GetModelName(uintptr_t entity) {
    auto p = Engine::GetProcess();

    auto node = p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode);
    auto name = node ? p->read<uintptr_t>(node + offsets::bone::m_modelState + offsets::econ::m_ModelName) : 0;
    if (!name)
        return "";

    char buffer[260]{};
    p->read_raw(name, buffer, sizeof(buffer) - 1);
    return buffer;
}

// The game sets the player model on every spawn, so this sets ours right after
constexpr auto AGENT_SPAWN_WINDOW = 3s;

void Skins::TrackSpawn(uintptr_t pawn, bool alive) {
    // A spawn: another pawn alive, or the same one alive again. Not the pawn we first see, that one might be mid life
    if (alive && this->agent_pawn && (pawn != this->agent_pawn || !this->agent_alive)) {
        this->agent_spawned = std::chrono::steady_clock::now();
        this->agent_waiting = false;
        this->agent_next_try = {};
    }

    if (pawn)
        this->agent_pawn = pawn;
    this->agent_alive = alive;
}

void Skins::ApplyAgent(uintptr_t pawn, const std::string& model) {
    if (!offsets::skins::setModel)
        return;

    auto now = std::chrono::steady_clock::now();

    auto current = GetModelName(pawn);
    if (current.empty())
        return;

    std::string wanted;

    if (!model.empty()) {
        bool on = current == model || (this->agent_applied == model && current == this->model_applied);
        if (on)
            return;

        // Anything else is the model of the game. Not ours from before, going from one agent to another keeps it
        if (this->agent_applied.empty() || current != this->model_applied)
            this->model_original = current;
        wanted = model;
    }
    else {
        // Back to the model of the game, if we changed it
        if (this->agent_applied.empty())
            return;

        if (current != this->model_applied || this->model_original.empty()) {
            this->agent_applied.clear();
            return;
        }

        wanted = this->model_original;
    }

    // The model might take a moment to load, so not every tick
    if (now < this->agent_next_try)
        return;

    // Only right after a spawn, like the game. A new model while alive leaves the old skeleton with an owner that is
    // gone, the game crashes on it a frame later
    if (now - this->agent_spawned > AGENT_SPAWN_WINDOW) {
        if (!this->agent_waiting)
            LOGF(INFO, "The agent changes on the next spawn");
        this->agent_waiting = true;
        return;
    }

    this->agent_next_try = now + 1s;

    if (!SetModel(pawn, wanted))
        return;

    if (!model.empty()) {
        this->agent_applied = model;
        this->model_applied = GetModelName(pawn);
        LOGF(VERBOSE, "Agent {} set, model {} -> {}", model, this->model_original, this->model_applied);
    }
    else {
        this->agent_applied.clear();
        LOGF(VERBOSE, "Agent removed, model back to {}", wanted);
    }
}

bool Skins::SetModel(uintptr_t entity, const std::string& model) {
    auto p = Engine::GetProcess();

    if (!offsets::skins::setModel || model.empty())
        return false;

    if (!this->strings)
        this->strings = p->allocate_remote(0x1000);

    if (!this->strings || model.size() >= 0x1000)
        return false;

    p->write_bytes(this->strings, std::vector<uint8_t>(model.c_str(), model.c_str() + model.size() + 1));
    return CallInGame(Engine::GetClient().base + offsets::skins::setModel, entity, this->strings);
}

constexpr auto KNIFE_REBUILD_DELAY = 500ms;
constexpr auto KNIFE_CHANGE_WINDOW = 3s;

// Turns the default knife into another one: item, weapon data & both models. True when it changed now
bool Skins::ApplyKnife(uintptr_t pawn, uintptr_t weapon, uintptr_t item, int& index, int knife, bool& model_changed) {
    if (!offsets::skins::subclassChanged || !offsets::skins::setModel || !IsKnife(index))
        return false;

    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();
    auto seen = this->knife_seen.try_emplace(weapon, now).first->second;
    auto it = this->knives.find(weapon);

    // A knife from the game, new or the game put its own back
    if (it == this->knives.end() || index != it->second.applied_index) {
        if (!knife || knife == index) {
            if (it != this->knives.end())
                this->knives.erase(it);

            return false;
        }

        it = this->knives.insert_or_assign(weapon, Knife{ index, index }).first;
    }

    auto& state = it->second;
    int target = knife ? knife : state.original_index;
    auto model = KnifeModel(target);

    if (target != index) {
        // The weapon data might take a moment, not every tick
        if (now < state.next_try)
            return false;

        // Only a knife that just came, like the game does: right after a spawn or a new knife. A knife changed later
        // on leaves a skeleton whose owner is gone, the game crashes on it with its next update of the weapon. The
        // knives there when we started might be mid life too
        bool new_knife = seen > this->skins_started + 1s && now - seen <= KNIFE_CHANGE_WINDOW;
        bool spawned = now - this->agent_spawned <= KNIFE_CHANGE_WINDOW;

        if (!new_knife && !spawned) {
            if (!this->knife_waiting)
                LOGF(INFO, "The knife changes on the next spawn");
            this->knife_waiting = true;
            return false;
        }
        this->knife_waiting = false;

        // Also with the knife in hand: the change only happens right after a spawn, waiting for it to be put away would
        // miss that. The first person model of the old knife the game might keep is replaced below
        auto weapon_services = p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices);
        bool in_hand = weapon_services && Engine::GetEntityFromHandle(p->read<uint32_t>(weapon_services + offsets::pawn::m_hActiveWeapon)) == weapon;

        state.next_try = now + 1s;

        p->write<uint16_t>(item + offsets::pawn::m_iItemDefinitionIndex, static_cast<uint16_t>(target));
        p->write<uint32_t>(weapon + offsets::econ::m_nSubclassID, StringToken(std::to_string(target)));

        // Loads the weapon data of the new knife, the animations come from it
        if (!CallInGame(Engine::GetClient().base + offsets::skins::subclassChanged, weapon, 0))
            return false;

        SetModel(weapon, model);
        model_changed = true;

        LOGF(VERBOSE, "Knife {} -> {}, model {}{}", index, target, GetModelName(weapon), in_hand ? ", in hand" : "");

        index = target;
        state.applied_index = target;
        state.fixes = 0;
        state.rebuild_at = now + KNIFE_REBUILD_DELAY;

        // Back to the knife of the game, nothing left to keep
        if (!knife) {
            this->knives.erase(it);
            return true;
        }

        return true;
    }

    // The game recreates the first person model, it might still show the old knife
    constexpr int MAX_FIXES = 3;
    auto hud = GetHudModel(pawn, weapon);

    if (hud && state.fixes < MAX_FIXES && now >= state.next_try) {
        auto hud_model = GetModelName(hud);
        if (!hud_model.empty() && hud_model != model) {
            state.next_try = now + 1s;
            state.fixes++;

            SetModel(hud, model);
            model_changed = true;
            LOGF(VERBOSE, "Knife first person model {} -> {}", hud_model, GetModelName(hud));

            // A new model has the materials of the default knife
            state.rebuild_at = now + KNIFE_REBUILD_DELAY;
        }
    }

    // The skin made right after a change can be on the model that was still loading, or on the first person model
    // we replaced since: once more on the model that is there now
    if (state.rebuild_at != std::chrono::steady_clock::time_point{} && now < state.rebuild_at)
        model_changed = true; // Still loading, its mesh waits too

    if (state.rebuild_at != std::chrono::steady_clock::time_point{} && now >= state.rebuild_at) {
        state.rebuild_at = {};
        LOGF(VERBOSE, "Knife {} skin made again", index);
        return true;
    }

    return false;
}

// Only the third person model, the first person gloves are made from it so it has to stay
void Skins::HideThirdPersonGloves(uintptr_t pawn, bool hide) {
    constexpr uint32_t EF_NODRAW = 0x20;

    auto p = Engine::GetProcess();
    auto entity = hide ? GetGloveEntity(pawn) : this->glove_hidden;

    // Shown again when turned off, a new glove entity is visible already
    if (!hide) {
        if (entity && GetGloveEntity(pawn) == entity) {
            auto effects = p->read<uint32_t>(entity + offsets::econ::m_fEffects);
            p->write<uint32_t>(entity + offsets::econ::m_fEffects, effects & ~EF_NODRAW);
        }

        this->glove_hidden = 0;
        return;
    }

    if (!entity)
        return;

    auto effects = p->read<uint32_t>(entity + offsets::econ::m_fEffects);
    if (!(effects & EF_NODRAW))
        p->write<uint32_t>(entity + offsets::econ::m_fEffects, effects | EF_NODRAW);

    this->glove_hidden = entity;
}

void Skins::ShowDefaultGloves(uintptr_t pawn, bool show) {
    if (offsets::skins::showDefaultGloves)
        CallInGame(Engine::GetClient().base + offsets::skins::showDefaultGloves, pawn, show);
}

// function(first, second) on a thread of the game, first being an entity. Only while that entity is still there when
// the game runs it: the call waits for the game, which might have deleted the entity meanwhile (a first person model
// it recreates, a weapon dropped). A model set on a deleted one leaves a skeleton the game crashes on later
bool Skins::CallInGame(uintptr_t function, uintptr_t first, uintptr_t second) {
    auto p = Engine::GetProcess();

    // CEntityInstance::m_pEntity, its CEntityIdentity is the slot of the entity list: the entity first, its handle at 0x10
    constexpr std::ptrdiff_t ENTITY_IDENTITY = 0x10;
    constexpr std::ptrdiff_t IDENTITY_HANDLE = 0x10;
    constexpr std::ptrdiff_t SKIPPED = CODE_SIZE - 8;  // Set by the code when the entity was gone

    auto identity = first ? p->read<uintptr_t>(first + ENTITY_IDENTITY) : 0;
    if (!identity || p->read<uintptr_t>(identity) != first)
        return false;
    auto handle = p->read<uint32_t>(identity + IDENTITY_HANDLE);

    if (!this->code)
        this->code = p->allocate_remote(CODE_SIZE, PAGE_EXECUTE_READWRITE);

    if (!this->code)
        return false;

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    emit({ 0x48, 0x83, 0xEC, 0x28 });           // sub rsp, 0x28

    // The slot still holds this entity with the same handle, or the call is skipped
    emit({ 0x48, 0xB8 }); emit64(identity);     // mov rax, identity
    emit({ 0x48, 0xB9 }); emit64(first);        // mov rcx, first
    emit({ 0x48, 0x39, 0x08 });                 // cmp [rax], rcx
    emit({ 0x75, 0x00 });                       // jne skip
    size_t skip_entity = code.size() - 1;
    emit({ 0x81, 0x78, IDENTITY_HANDLE }); emit32(handle); // cmp dword ptr [rax + 0x10], handle
    emit({ 0x75, 0x00 });                       // jne skip
    size_t skip_handle = code.size() - 1;

    emit({ 0x48, 0xBA }); emit64(second);       // mov rdx, second
    emit({ 0x49, 0xBB }); emit64(function);     // mov r11, function
    emit({ 0x41, 0xFF, 0xD3 });                 // call r11
    emit({ 0xEB, 0x00 });                       // jmp done
    size_t to_done = code.size() - 1;

    size_t skip = code.size();
    emit({ 0x48, 0xB8 }); emit64(this->code + SKIPPED); // mov rax, &skipped
    emit({ 0xC6, 0x00, 0x01 });                 // mov byte ptr [rax], 1

    size_t done = code.size();
    emit({ 0x48, 0x83, 0xC4, 0x28 });           // add rsp, 0x28
    emit({ 0x33, 0xC0 });                       // xor eax, eax
    emit({ 0xC3 });                             // ret

    code[skip_entity] = static_cast<uint8_t>(skip - (skip_entity + 1));
    code[skip_handle] = static_cast<uint8_t>(skip - (skip_handle + 1));
    code[to_done] = static_cast<uint8_t>(done - (to_done + 1));

    p->write<uint8_t>(this->code + SKIPPED, 0);
    p->write_bytes(this->code, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->code), code.size());

    if (!GameThread::Call(this->code)) {
        LOGF(WARNING, "Call into the game did not finish in time");
        this->code = 0;
        return false;
    }

    if (p->read<uint8_t>(this->code + SKIPPED)) {
        LOGF(VERBOSE, "Call into the game skipped, entity 0x{:X} was deleted meanwhile", first);
        return false;
    }

    return true;
}

// Kept on while the glove model loads, the gloves are created once it is there
void Skins::SetGlovePreload(bool enabled) {
    if (!offsets::skins::precacheGloves || enabled == this->glove_preload)
        return;

    auto p = Engine::GetProcess();
    auto flag = Engine::GetClient().base + offsets::skins::precacheGloves;

    // Already on by the game, leave it alone
    if (enabled && p->read<bool>(flag))
        return;

    p->write<bool>(flag, enabled);
    this->glove_preload = enabled;

    LOGF(VERBOSE, "Glove preload {}", enabled ? "on" : "off");
}

uintptr_t Skins::GetGloveEntity(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    return Engine::GetEntityFromHandle(p->read<uint32_t>(pawn + offsets::skins::gloveHelper));
}

// Mesh first: node->mask = 0; SetMeshGroupMask(node, mask);
// Then like the loop of RegenerateWeaponSkins, but for the given weapons even when they had no skin before:
// clear(&weapon->composite_owner, true); update(weapon, false); weapon->viewmodel_skin_built = false; update_viewmodel(weapon);
void Skins::Rebuild(const std::vector<uintptr_t>& entities, const std::vector<MeshMask>& masks) {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (!this->code)
        this->code = p->allocate_remote(CODE_SIZE, PAGE_EXECUTE_READWRITE);

    if (!this->code)
        return;

    for (size_t start = 0; start < std::max<size_t>(entities.size(), 1); start += MAX_REBUILDS) {
        std::vector<uint8_t> code;

        auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
        auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
        auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

        emit({ 0x53 });                         // push rbx
        emit({ 0x48, 0x83, 0xEC, 0x20 });       // sub rsp, 0x20, keeps the stack aligned

        // The game skips the switch when the mask already matches, so it gets cleared first
        for (size_t i = 0; start == 0 && i < std::min(masks.size(), MAX_REBUILDS); i++) {
            auto address = masks[i].node + offsets::bone::m_modelState + offsets::econ::m_MeshGroupMask;

            emit({ 0x48, 0xB8 }); emit64(address);                                      // mov rax, &node->mask
            emit({ 0x48, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00 });                         // mov qword ptr [rax], 0
            emit({ 0x48, 0xB9 }); emit64(masks[i].node);                                // mov rcx, node
            emit({ 0x48, 0xBA }); emit64(masks[i].mask);                                // mov rdx, mask
            emit({ 0x49, 0xBB }); emit64(client.base + offsets::skins::setMeshGroupMask); // mov r11, SetMeshGroupMask
            emit({ 0x41, 0xFF, 0xD3 });                                                 // call r11
        }

        for (size_t i = start; i < std::min(entities.size(), start + MAX_REBUILDS); i++) {
            emit({ 0x48, 0xBB }); emit64(entities[i]);                                  // mov rbx, weapon
            emit({ 0x48, 0x8D, 0x8B }); emit32(static_cast<uint32_t>(offsets::skins::compositeOwner)); // lea rcx, [rbx + composite owner]
            emit({ 0xB2, 0x01 });                                                       // mov dl, 1
            emit({ 0x49, 0xBB }); emit64(client.base + offsets::skins::clearMaterials); // mov r11, clear
            emit({ 0x41, 0xFF, 0xD3 });                                                 // call r11
            emit({ 0x33, 0xD2 });                                                       // xor edx, edx
            emit({ 0x48, 0x8B, 0xCB });                                                 // mov rcx, rbx
            emit({ 0x49, 0xBB }); emit64(client.base + offsets::skins::updateWeaponSkin); // mov r11, update
            emit({ 0x41, 0xFF, 0xD3 });                                                 // call r11

            // First person model, its skin was built when the weapon was drawn
            if (offsets::skins::updateViewmodelSkin) {
                emit({ 0xC6, 0x83 }); emit32(static_cast<uint32_t>(offsets::skins::viewmodelSkinBuilt)); emit({ 0x00 }); // mov byte ptr [rbx + built], 0
                emit({ 0x48, 0x8B, 0xCB });                                                 // mov rcx, rbx
                emit({ 0x49, 0xBB }); emit64(client.base + offsets::skins::updateViewmodelSkin); // mov r11, update viewmodel
                emit({ 0x41, 0xFF, 0xD3 });                                                 // call r11
            }
        }

        emit({ 0x48, 0x83, 0xC4, 0x20 });       // add rsp, 0x20
        emit({ 0x5B });                         // pop rbx
        emit({ 0x33, 0xC0 });                   // xor eax, eax
        emit({ 0xC3 });                         // ret

        p->write_bytes(this->code, code);
        FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->code), code.size());

        if (!GameThread::Call(this->code)) {
            // Might still be running, never write over it
            LOGF(WARNING, "Skin rebuild did not finish in time");
            this->code = 0;
            return;
        }
    }
}

void Skins::CollectMeshMasks(uintptr_t pawn, uintptr_t weapon, uint64_t mask, bool force, std::vector<MeshMask>& out) {
    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();

    auto write_mask = [&](uintptr_t entity) {
        auto node = p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode);
        if (!node)
            return;

        auto address = node + offsets::bone::m_modelState + offsets::econ::m_MeshGroupMask;

        // Without the game function only the value changes, the model might not follow
        if (!offsets::skins::setMeshGroupMask) {
            if (p->read<uint64_t>(address) != mask)
                p->write<uint64_t>(address, mask);
            return;
        }

        if (!force && p->read<uint64_t>(address) == mask)
            return;

        // The game might keep resetting it, dont call into the game every tick or forever
        constexpr int MAX_FIXES = 3;
        auto& fix = this->mask_fixed[node];

        if (force || fix.mask != mask)
            fix.count = 0;
        else if (now - fix.last < 1s || fix.count >= MAX_FIXES)
            return;

        fix.last = now;
        fix.mask = mask;

        if (++fix.count == MAX_FIXES)
            LOGF(VERBOSE, "The game keeps resetting the mesh of node 0x{:X}, giving up", node);

        out.push_back({ node, mask });
    };

    write_mask(weapon);

    if (auto hud = GetHudModel(pawn, weapon))
        write_mask(hud);
}

// First person model, a child of the arms owned by the weapon
uintptr_t Skins::GetHudModel(uintptr_t pawn, uintptr_t weapon) {
    auto p = Engine::GetProcess();

    auto arms = Engine::GetEntityFromHandle(p->read<uint32_t>(pawn + offsets::econ::m_hHudModelArms));
    if (!arms)
        return 0;

    auto arms_node = p->read<uintptr_t>(arms + offsets::pawn::m_pGameSceneNode);
    auto child = arms_node ? p->read<uintptr_t>(arms_node + offsets::econ::m_pChild) : 0;

    for (int i = 0; child && i < 32; i++, child = p->read<uintptr_t>(child + offsets::econ::m_pNextSibling)) {
        auto owner = p->read<uintptr_t>(child + offsets::econ::m_pOwner);
        if (!owner)
            continue;

        if (Engine::GetEntityFromHandle(p->read<uint32_t>(owner + offsets::econ::m_hOwnerEntity)) == weapon)
            return owner;
    }

    return 0;
}

bool Skins::Attach(Applied& applied) {
    auto p = Engine::GetProcess();
    auto vector = AttributeVectorOf(applied.item);

    auto current = p->read<AttributeVector>(vector);

    // Ours from an earlier run, the memory is never freed so it is still there
    if (current.memory && (current.flags & EXTERNAL_BUFFER) && current.size == ATTRIBUTE_COUNT &&
        p->read<uint16_t>(current.memory + offsetof(EconItemAttribute, definition_index)) == PAINT_KIT) {
        applied.block = current.memory;
        WriteAttributes(applied);
        return true;
    }

    // Only into an empty list, the game owns the memory otherwise
    if (current.size || current.memory) {
        static std::set<uintptr_t> reported;
        if (reported.insert(applied.item).second)
            LOGF(VERBOSE, "Item 0x{:X} already has {} attributes, skipping", applied.item, current.size);

        return false;
    }

    applied.block = p->allocate_remote(sizeof(EconItemAttribute) * ATTRIBUTE_COUNT);
    if (!applied.block)
        return false;

    WriteAttributes(applied);

    // Memory first, the size makes the game read it
    AttributeVector attached{};
    attached.memory = applied.block;
    attached.allocated = ATTRIBUTE_COUNT;
    attached.flags = EXTERNAL_BUFFER;

    p->write<uintptr_t>(vector + offsetof(AttributeVector, memory), attached.memory);
    p->write<int32_t>(vector + offsetof(AttributeVector, allocated), attached.allocated);
    p->write<uint32_t>(vector + offsetof(AttributeVector, flags), attached.flags);
    p->write<int32_t>(vector + offsetof(AttributeVector, size), ATTRIBUTE_COUNT);

    return true;
}

void Skins::Detach(Applied& applied) {
    if (!IsAttached(applied))
        return;

    auto p = Engine::GetProcess();
    auto vector = AttributeVectorOf(applied.item);

    // Size first, so the game stops reading before the memory goes
    p->write<int32_t>(vector + offsetof(AttributeVector, size), 0);
    p->write<uintptr_t>(vector + offsetof(AttributeVector, memory), 0);
    p->write<int32_t>(vector + offsetof(AttributeVector, allocated), 0);
    p->write<uint32_t>(vector + offsetof(AttributeVector, flags), 0);
}

bool Skins::IsAttached(const Applied& applied) {
    if (!applied.block)
        return false;

    auto p = Engine::GetProcess();
    return p->read<uintptr_t>(AttributeVectorOf(applied.item) + offsetof(AttributeVector, memory)) == applied.block;
}

void Skins::WriteAttributes(const Applied& applied) {
    auto p = Engine::GetProcess();

    auto make = [](uint16_t definition_index, float value) {
        EconItemAttribute attribute{};
        attribute.definition_index = definition_index;
        attribute.value = value;
        attribute.initial_value = value;
        return attribute;
    };

    const EconItemAttribute attributes[ATTRIBUTE_COUNT] = {
        make(PAINT_KIT, static_cast<float>(applied.skin.paint_kit)),
        make(PATTERN_SEED, static_cast<float>(applied.skin.seed)),
        make(WEAR, applied.skin.wear),
    };

    for (int i = 0; i < ATTRIBUTE_COUNT; i++)
        p->write<EconItemAttribute>(applied.block + i * sizeof(EconItemAttribute), attributes[i]);
}

int Skins::CountMaterials(uintptr_t entity) {
    auto p = Engine::GetProcess();
    auto owner = entity + offsets::skins::compositeOwner;

    // Finished ones, their count sits right before the pointers, and the ones still being built
    return p->read<int>(owner + offsets::skins::compositeMaterials - 0x8) + p->read<int>(owner + offsets::skins::compositePending);
}
