#pragma once
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

// A player model of the user's, in csgo/characters or csgo/agents of the game
struct CustomModel {
    enum class Status { OK, WARNING, BAD };

    std::string name;           // File name without its extension
    std::string resource;       // How the game names it: "characters/models/x/y.vmdl"
    Status status = Status::BAD;
    std::string note;           // Why it can't be used, or what to look out for
    int bones = 0;
    std::filesystem::path file;
    std::string place;          // Folder in csgo/ it was made for when it is somewhere else: "agents/models/x/"
    std::string made_for;       // Its resource once there
    bool fits_there = false;    // Checked as if it were there: usable once moved
    uint64_t hands_mask = 0;    // Mesh group of its first person hands, 0 when it has none
    uint64_t empty_mask = 0;    // A group with none of its meshes: first person without its body, the gloves of the game show
};

// Player models of the user (CS2 player models made by the community: the .vmdl_c with its materials & meshes) in
// csgo/characters/models of the game. Each file is checked before the game is asked for it: rigged to the skeleton of
// CS2 players with the animations of the game, every file it needs present. A model passing that is loaded by the
// game the way the files of a map are (the precache of its resource system), and only once the game has it in memory
// the agent code of Skins sets it on our pawn, right after a spawn like the game does. Only we see it, -insecure only.
//
// Without the precache the game only makes an empty handle for a file no manifest lists, which shows as ERROR
class CustomModels {
public:
    ~CustomModels()                              = default;
    CustomModels(const CustomModels&)            = delete;
    CustomModels(CustomModels&&)                 = delete;
    CustomModels& operator=(const CustomModels&) = delete;
    CustomModels& operator=(CustomModels&&)      = delete;

    static void Shutdown();

    // -insecure & the code of the game it needs is there
    static bool IsAvailable();

    // csgo/characters/models of the game (the running one, else the install Steam knows), empty when not found
    static std::filesystem::path Folder();

    // The check of a model file not on disk yet (in an archive): its bytes, where it would be, whether the archive has
    // a file it names (game files are looked up by the check)
    static CustomModel CheckData(const std::vector<uint8_t>& file, const std::string& resource,
        const std::function<bool(const std::string&)>& has_file);

    // The models of the folder & what the check found, looked at again every few seconds
    static std::vector<CustomModel> List();
    static void Rescan();

    // Moves the folder of a model put somewhere else to the folder it was made for, false with why when it can't
    static bool MoveToPlace(const CustomModel& model, std::string& error);

    // The model loaded by the game: true once it is in memory & may be set. The first call asks the game to load it,
    // again after a map change (the game drops what no map needs). Skins thread only
    static bool Prepare(const std::string& resource);

    // Asks the game to load any resource (a particle system of the kill effect...), like the models. False when the
    // game could not be asked (not alive yet to check the code against our model, not in a match). Any thread
    static bool PrecacheResource(const std::string& resource);

    // What Prepare got to for the model, for the menu
    static std::string LoadStatus(const std::string& resource);

    // First person of the model: the mesh group of its hands (0 when it has none of its own), else a group with none
    // of its meshes. False when the model is not known
    static bool FirstPerson(const std::string& resource, uint64_t& hands_mask, uint64_t& empty_mask);
private:
    CustomModels() {};

    static CustomModels& GetInstance()
    {
        static CustomModels i{};
        return i;
    }

    void ScanThread();
    void Scan();
    CustomModel Check(const std::filesystem::path& file, const std::string& resource);
    void Evaluate(CustomModel& model, const std::vector<uint8_t>& file, const std::function<bool(const std::string&)>& has_file);
    bool Exists(const std::string& reference);
    void IndexPackages();

    // Game side, Skins thread
    bool Resolve();
    bool BuildPage();
    bool Calibrate();
    bool CallPrecache(const std::string& resource);
    bool CallGetModel(const std::string& resource, uintptr_t& binding);
    bool IsLoaded(const std::string& resource, bool& wrong_model);

private:
    std::filesystem::path game_dir;     // game/ of the install

    // Scan thread
    struct Checked {
        uintmax_t size = 0;
        int64_t time = 0;
        CustomModel model;
    };
    std::unordered_map<std::string, Checked> checked;   // By resource
    std::unordered_set<uint64_t> packaged;              // Hashes of the files in the vpks of the game
    bool indexed = false;
    std::mutex package_mutex;                           // The index, checks run on other threads too

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<CustomModel> models;
    bool rescan = false;
    std::atomic<bool> started = false;
    std::atomic<bool> stopping = false;

    // What the game has loaded, Skins thread
    struct Load {
        enum class State { IDLE, LOADING, READY, FAILED } state = State::IDLE;
        std::string map;
        std::chrono::steady_clock::time_point started{}, next_check{};
    };
    std::unordered_map<std::string, Load> loads;
    std::mutex load_mutex;      // loads, read by the menu

    std::recursive_mutex call_mutex;  // Resolve & the calls share the page: Skins & KillEffect threads
    bool resolved = false;
    bool resolve_failed = false;
    uintptr_t page = 0;             // Our code & its data, in the game. A new one after a call that did not finish in time
    uintptr_t model_info = 0;       // GameModelInfo001, its GetModel is what SetModel uses
    size_t get_model_slot = 0;      // Byte offset in its vtable
    uintptr_t set_name = 0;         // CResourceName::Set of host.dll
    uintptr_t resource_system = 0;  // ResourceSystem013
    size_t precache_slot = 0;       // Byte offset in its vtable
    std::ptrdiff_t name_offset = -1; // Of the model name in the loaded model, -1 when not found
};
