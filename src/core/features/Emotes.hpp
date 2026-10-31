#pragma once
#include "core/memory/Memory.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Animations played on our own player, seen in third person by us only. Picked on the wheel (EmoteWheel).
//
// The game's own: the celebrations of the MVP banner, the poses & tricks of the main menu, the end of the round. The
// clips are loaded by the game through a resource manifest, the way its menu loads the player preview, once. Every
// final pose of a skeleton goes through CSkeletonInstance::ApplyAnimGraphPose: a jump over its start leads to our stub,
// which samples the clip into the pose of our player (CNmClip::GetPose at the time since the start) before the game
// sets the bones from it.
//
// The Fortnite dances & emotes of the Emote Pack (cache/emotes, downloaded from our assets on GitHub or extracted there
// by hand): its folder is added to the file system of the game &
// the dance model loaded like the clips. A client-only prop_dynamic of it plays the dance with a copy of our agent
// bonemerged onto it, spawned the way the portrait world of the game spawns its props; our player & the dance model
// are not drawn (MaterialChams). Its music is played by the game (Sounds).
//
// Moving, shooting, dying, a new round or the key again ends it
class Emotes {
public:
    ~Emotes()                        = default;
    Emotes(const Emotes&)            = delete;
    Emotes(Emotes&&)                 = delete;
    Emotes& operator=(const Emotes&) = delete;
    Emotes& operator=(Emotes&&)      = delete;

    static bool Init();

    // -insecure, the code of the game, third person & the mouse for the wheel (the free cam code)
    static bool IsAvailable();
    // Why not, for the menu. Null when available
    static const char* WhyUnavailable();
    // What the loading got to, empty once they can be played
    static std::string GetStatus();
    // The same for the Fortnite dances: where the pack goes, their loading. Empty once they can be played
    static std::string GetDanceStatus();

    enum class Kind : uint8_t { Game, Dance, Emote };
    struct Entry {
        int id;                 // What Play takes
        const char* name;
        Kind kind;
        int rarity;             // Fortnite's, 0 for the game's own
        const char* sequence;   // Its icon is icons/<sequence>.png of the pack, nullptr for the game's own
    };
    // The game's own animations, then the dances & emotes of the pack when they can be played
    static const std::vector<Entry>& GetEntries(bool dances);
    static bool CanDance();
    // The folder of the Emote Pack (the one holding csgo_addons), empty while there is none
    static std::filesystem::path GetPack();

    // The files of the pack the dances use, from our assets on GitHub into its folder (cache/emotes when there is
    // none). Any thread, the loader too: what is there already is kept, all of it there needs no network
    static void DownloadPack();
    static bool IsDownloadingPack();
    // Files of it the dances use that are not there (the model, icons, songs), -1 before it was looked for
    static int GetPackMissing();
    // How far the download got or why it failed, empty when there is nothing to say
    static std::string GetPackStatus();

    // Alive in a match with something loaded: the wheel may open
    static bool CanPlay();
    // From the wheel, any thread: started by the next update
    static void Play(int id);
    static void Stop();
    static bool IsPlaying();

    // The start of ApplyAnimGraphPose back, the dance removed, the manifests let go
    static void Shutdown();
private:
    Emotes() {};

    static Emotes& GetInstance()
    {
        static Emotes i{};
        return i;
    }

    bool InitImpl();
    void Thread();
    void Update();

    // The service manifests are made with, checked once. False when it can't be used
    bool EnsureService();
    // A manifest of the names at names (const char* each, in our page) made on the main thread, 0 when not
    uintptr_t MakeManifest(uintptr_t names, int count);

    // The game's own: the manifest made & loaded, then the clips of it on this map. True once they can be played
    bool UpdateClips();
    bool Load();
    bool FindClips();
    // Our page in reach of client.dll: the stub & the names of the clips
    bool BuildPage();
    // The jump over the start of ApplyAnimGraphPose, once
    bool Install();
    // Code of ours on the main thread of the game, with what it fills after it in its page. False when it did not run
    bool CallJob(const std::vector<uint8_t>& code, std::vector<uint8_t> data);

    bool Start(int emote, uintptr_t pawn);
    void End(const char* why);
    void Fail(const std::string& why);
    void SetStatus(const std::string& text);

    // The Fortnite dances: the pack found, its folder added to the game & the model loaded. True once they can be played
    bool UpdateDances();
    // Their code was found & is what was expected
    bool DancesUsable();
    void FindPack();
    void DownloadThread();
    bool MountPack();
    bool LoadDanceModel();
    void DanceFail(const std::string& why);
    void SetDanceStatus(const std::string& text);

    // A dance: our player hidden first, the dance & the copy of our agent spawned once it is
    void BeginDance(int dance, uintptr_t pawn);
    bool SpawnDance();
    void StepDance();
    // The props removed (when they still are ours), our player drawn again, the music stopped
    void EndDance();
    // UTIL_Remove of each { entity, handle } on the main thread, only while it is still ours. False when it did not run
    bool RemoveProps(std::initializer_list<std::pair<uintptr_t, uint32_t>> props);
    // The props of a map that is gone, nothing to remove
    void ForgetDance();
    bool DanceAlive(uintptr_t entity, uint32_t handle);

    enum class LoadState : uint8_t { Idle, Loading, Ready, Failed };
    enum class DancePhase : uint8_t { None, Hiding, Dancing };

private:
    std::mutex update_mutex;
    std::atomic<bool> stopping = false;
    std::atomic<bool> broken = false;       // The game was not what was expected: off for the session

    std::atomic<int> request = -1;          // From the wheel
    std::atomic<bool> stop_request = false;
    std::atomic<int> playing = -1;
    std::atomic<bool> can_play = false;

    uintptr_t page = 0;
    uintptr_t job = 0;
    bool installed = false;

    uintptr_t service = 0;                  // IGameResourceService
    bool service_checked = false;
    uintptr_t manifest = 0;
    LoadState state = LoadState::Idle;
    std::chrono::steady_clock::time_point load_started{}, next_check{};
    std::string map;
    std::string found_map;                  // The clips were found on it
    std::vector<uintptr_t> clips;           // CNmClip of each emote, 0 when it can't be played
    bool clips_ready = false;

    // The Fortnite dances
    std::atomic<bool> can_dance = false;
    bool dances_broken = false;             // Not what was expected: off for the session
    std::mutex pack_mutex;
    std::filesystem::path pack;
    bool pack_told = false;                 // Where it goes, logged once
    std::chrono::steady_clock::time_point pack_checked{};
    std::atomic<bool> pack_changed = false; // Downloaded: looked for again right away
    std::atomic<bool> downloading = false;
    std::atomic<int> pack_missing = -1;
    bool mounted = false;
    uintptr_t dance_manifest = 0;
    uintptr_t mem_alloc = 0;                // &g_pMemAlloc of tier0, the key values of a spawn come from it
    LoadState dance_state = LoadState::Idle;
    std::chrono::steady_clock::time_point dance_load_started{}, dance_next_check{};

    // The one playing
    uintptr_t pawn = 0;
    Vec3_t anchor{};                        // Where our player stood at the start, where a dance stays
    std::chrono::steady_clock::time_point started{};
    float length = 0.f;
    uint8_t round = 0;

    // The dance playing
    DancePhase phase = DancePhase::None;
    int dance = -1;
    uintptr_t prop = 0, clone = 0;
    uint32_t prop_handle = 0, clone_handle = 0;
    bool clone_drawn = false;               // Its model was seen with scene objects
    bool intro = false;                     // The first sequence of one that loops another after
    std::filesystem::path song;             // Its music, empty for none
    std::chrono::steady_clock::time_point next_music{}, next_alive{};

    std::mutex status_mutex;
    std::string status, dance_status, pack_status;
};
