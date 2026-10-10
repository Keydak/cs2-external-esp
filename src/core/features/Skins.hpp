#pragma once
#include "core/memory/Memory.hpp"

struct SkinInfo {
    std::string name;   // e.g. "Fade"
    int paint_kit = 0;
    float min_float = 0.f;
    float max_float = 1.f;
    bool legacy_model = false; // Made for the old weapon model
    std::string image;          // Url of the preview
    uint32_t rarity_color = 0;  // 0xRRGGBB, 0 when unknown
};

struct ItemInfo {
    int definition_index = 0;
    std::string name;       // e.g. "AK-47"
    std::string category;   // e.g. "Rifles", "Gloves"
    bool terrorist = false;         // Teams that can buy it
    bool counter_terrorist = false;
    std::string image;      // Without a skin, knives only
    std::vector<SkinInfo> skins;

    const SkinInfo* FindSkin(int paint_kit) const;
};

struct AgentInfo {
    int definition_index = 0;
    std::string name;       // e.g. "Bloody Darryl The Strapped"
    std::string group;      // e.g. "The Professionals"
    bool terrorist = false; // Team, counter-terrorist otherwise
    std::string model;      // e.g. "agents/models/tm_professional/tm_professional_varf5.vmdl"
    std::string image;
    uint32_t rarity_color = 0;
};

struct MusicKitInfo {
    int definition_index = 0;
    std::string name;       // e.g. "Daniel Sadowski, Crimson Assault"
    std::string code_name;  // e.g. "valve_cs2_01", its sound events are named after it
    std::string image;
    uint32_t rarity_color = 0;
};

class Skins {
public:
    ~Skins()                       = default;
    Skins(const Skins&)            = delete;
    Skins(Skins&&)                 = delete;
    Skins& operator=(const Skins&) = delete;
    Skins& operator=(Skins&&)      = delete;

    static bool Init();
    // Starts loading the list of the items (internet or the copy on the disk) once, the game is not needed for it
    static void FetchList();

    // Needs -insecure and the skin code of the game
    static bool IsAvailable();

    // Skin list is fetched in the background, items are only valid once loaded
    static bool IsLoaded();
    static bool HasFailed();
    static const std::vector<ItemInfo>& GetItems();
    static const ItemInfo* FindItem(int definition_index);

    static bool IsGlove(int definition_index);
    static bool IsKnife(int definition_index); // Any knife, also the default ones

    static const std::vector<AgentInfo>& GetAgents();
    static const AgentInfo* FindAgent(int definition_index);

    static const std::vector<MusicKitInfo>& GetMusicKits();
    static const MusicKitInfo* FindMusicKit(int definition_index);

    // Undoes what is left in the game code
    static void Shutdown();
private:
    Skins() {};

    static Skins& GetInstance()
    {
        static Skins i{};
        return i;
    }

    // Our attributes attached to an item of the game
    struct Applied {
        uintptr_t item = 0;
        uintptr_t block = 0;            // Attribute array, allocated in the game
        cfg::skins::item_t skin{};
        uint32_t original_id_high = 0;
        int original_index = 0;         // Gloves only
        bool built = false;             // Skin material exists
        int attempts = 0;
        std::chrono::steady_clock::time_point next_try{};
    };

    bool InitImpl();
    void Load();
    bool LoadCache();
    void SaveCache();
    void Thread();

    void Apply();
    void ApplyGloves(uintptr_t pawn, uint32_t account_id, int glove, const std::map<int, cfg::skins::item_t>& items);
    void TrackSpawn(uintptr_t pawn, bool alive);
    void ApplyAgent(uintptr_t pawn, const std::string& model); // Empty puts the model of the game back
    bool ApplyKnife(uintptr_t pawn, uintptr_t weapon, uintptr_t item, int& index, int knife, bool& model_changed);
    uintptr_t GetHudModel(uintptr_t pawn, uintptr_t weapon);
    bool SetModel(uintptr_t entity, const std::string& model);
    void LoadAgents();
    void LoadMusicKits();
    void ApplyMusicKit(int music_kit);
    std::string GetModelName(uintptr_t entity);
    void ReapplyGloves(uintptr_t pawn);
    void SetGlovePreload(bool enabled);
    void PatchGloveRemoval(bool patched);
    void ShowDefaultGloves(uintptr_t pawn, bool show);
    void HideThirdPersonGloves(uintptr_t pawn, bool hide);
    void ShowCustomModelHands(uintptr_t pawn, bool on, uint64_t mask, bool hide_gloves);
    uint64_t AgentArms(uintptr_t pawn, const std::string& worn, uint64_t empty_mask);
    bool CallInGame(uintptr_t function, uintptr_t first, uintptr_t second);
    uintptr_t GetGloveEntity(uintptr_t pawn);
    struct MeshMask {
        uintptr_t node;
        uint64_t mask;
    };

    void Rebuild(const std::vector<uintptr_t>& weapons, const std::vector<MeshMask>& masks);
    void CollectMeshMasks(uintptr_t pawn, uintptr_t weapon, uint64_t mask, bool force, std::vector<MeshMask>& out);
    void CollectArmsMask(uintptr_t node, uint64_t mask, std::vector<MeshMask>& out);

    bool Attach(Applied& applied);
    void Detach(Applied& applied);
    bool IsAttached(const Applied& applied);
    void WriteAttributes(const Applied& applied);
    int CountMaterials(uintptr_t entity);

private:
    std::vector<ItemInfo> items;
    std::vector<AgentInfo> agents;
    std::vector<MusicKitInfo> music_kits;

    std::atomic<bool> list_started = false;
    std::atomic<bool> loaded = false;
    std::atomic<bool> failed = false;

    std::map<uintptr_t, Applied> weapons; // By weapon entity
    Applied gloves{};
    uintptr_t gloves_pawn = 0;
    int gloves_retries = 0; // The model loads after the first try, the gloves are only created on a later one
    std::chrono::steady_clock::time_point gloves_next_try{};
    bool glove_preload = false; // We turned the preview flag on
    bool glove_patched = false;
    uintptr_t glove_hidden = 0; // Glove entity we hid from the third person view // Gloves are kept even when the player model says it has its own
    std::atomic<bool> stopping = false;

    uintptr_t code = 0; // Executable memory in the game for Rebuild()
    uintptr_t strings = 0; // Memory in the game for names passed to its functions

    // Music kit, kept in a few places: the controller (scoreboard), its inventory services (the music of the round)
    // & the controller of the server, which sends it with the MVP. What the game had is put back when ours is removed
    struct MusicTarget {
        uintptr_t address = 0;
        bool wide = true;       // int32, else uint16
        bool applied = false;
        int original = 0;
        int written = 0;
    };

    void ApplyMusicTarget(MusicTarget& target, uintptr_t address, bool wide, int music_kit);
    uintptr_t GetServerController(uintptr_t controller);

    void ApplyMenuMusic(int music_kit);

    MusicTarget music_targets[3];

    // Kit names for the music of the main menu, two so a new kit is a new pointer & the game restarts the music.
    // Never freed, the game keeps the pointer of the one playing
    uintptr_t menu_music_names = 0;
    int menu_music_slot = 0;
    std::string menu_music_written;
    uintptr_t music_controller = 0;
    uintptr_t server_entities = 0;  // Entity system of server.dll
    std::chrono::steady_clock::time_point server_next_search{};

    // Agent
    std::string model_original; // What the game gave us, put back when the agent is removed
    std::string model_applied;  // Name the game reports after our change
    std::string agent_applied;  // Model we asked for, empty when the game model is on
    std::chrono::steady_clock::time_point agent_next_try{};
    uintptr_t agent_pawn = 0;   // The pawn & when it spawned: the model only changes right after
    bool agent_alive = false;
    bool agent_waiting = false; // Logged once that a change waits for the next spawn
    std::chrono::steady_clock::time_point agent_spawned{};
    std::chrono::steady_clock::time_point selection_spawn{};   // The spawn the pick below was taken at
    std::string spawn_selection;    // Agent or custom model picked when we spawned, a pick after waits for the next spawn
    uintptr_t arms_swapped = 0;     // First person arms given the agent model (a custom model without hands)

    // Knife
    struct Knife {
        int original_index = 0; // Default knife of the team, put back when the knife is removed
        int applied_index = 0;
        int fixes = 0;          // Model fixes of the first person model in a row
        std::chrono::steady_clock::time_point next_try{};
        std::chrono::steady_clock::time_point rebuild_at{}; // The skin again once the new model is there, 0 for none
    };
    std::map<uintptr_t, Knife> knives; // By weapon entity
    std::map<uintptr_t, std::chrono::steady_clock::time_point> knife_seen; // When each knife was first seen
    std::chrono::steady_clock::time_point skins_started{}; // First pass with a living pawn, knives then might be mid life
    bool knife_waiting = false;     // Logged once that a change waits for the next spawn

    struct MaskFix {
        std::chrono::steady_clock::time_point last{};
        uint64_t mask = 0;
        int count = 0; // In a row, the game keeps resetting it when this grows
    };
    std::map<uintptr_t, MaskFix> mask_fixed; // By scene node

    // First person of a custom model: its own hands, the glove models of the game hidden
    uintptr_t hands_node = 0;               // Arms node whose mask we changed
    uint64_t hands_original_mask = 0;
    std::vector<uintptr_t> hands_hidden;    // Glove models of the arms
};
