#pragma once
#include <optional>

// Player models of GameBanana (Counter-Strike 2 > Skins) put in the game with one click: the archive downloaded,
// unpacked by the tar of Windows (zip, rar, 7z), and only its compiled resources (.v*_c) copied to csgo/ at the paths
// they have in it. What went where is kept, so a model is removed as cleanly. CustomModels then lists & checks them
// like any model of the folder.
//
// Before that each mod is checked the same way without downloading it: of a zip only its file list & its models are
// read (ranges of the file). A rar or 7z can't be read in parts: one uploaded before the animations of CS2 changed is
// left out (every zip of that time that was checked had the old ones), a newer one is checked once downloaded, before
// anything goes in the game. Results are kept by file, a file of GameBanana never changes
class ModBrowser {
public:
    struct File {
        int id = 0;
        std::string name;
        uint64_t size = 0;
        std::string description;
        int64_t added = 0;
    };

    struct Mod {
        int id = 0;
        std::string name;
        std::string author;
        std::string image;      // Thumbnail, 16:9
        std::string url;        // Its page
        int likes = 0;
        int views = 0;
        int64_t added = 0;
        int64_t modified = 0;

        // The check of its files: usable when one of them has a model to wear
        enum class Verdict { PENDING, USABLE, UNUSABLE, UNCHECKED } verdict = Verdict::PENDING;
        std::string note;
        bool own_hands = true;  // Usable, with first person hands of its own (else the arms of the agent are shown)
    };

    enum class ListState { IDLE, LOADING, READY, FAILED };

    // The check of one file before downloading it
    enum class FileState { UNCHECKED, USABLE, UNUSABLE };
    struct FileCheck {
        FileState state = FileState::UNCHECKED;
        std::string note;
        bool own_hands = true;
    };

    struct Job {
        enum class State { NONE, FETCHING, CHOOSE, DOWNLOADING, UNPACKING, DONE, FAILED } state = State::NONE;
        float progress = 0.f;           // Of the download
        std::string message;
        std::vector<File> files;        // To pick from, more than one in the mod
    };

    ~ModBrowser()                            = default;
    ModBrowser(const ModBrowser&)            = delete;
    ModBrowser(ModBrowser&&)                 = delete;
    ModBrowser& operator=(const ModBrowser&) = delete;
    ModBrowser& operator=(ModBrowser&&)      = delete;

    // The list & its check in the background from the start, low priority: done by the time the page opens
    static void Start();
    static void Shutdown();

    // The list, the kept one at once while the one of GameBanana loads (loaded the first time it is asked for)
    static std::vector<Mod> List();
    static ListState GetListState();
    static void Reload();

    // Downloads & puts the mod in the game. A mod with several files asks first (Job::State::CHOOSE), then Install
    // again with the file. One at a time
    static void Install(const Mod& mod, int file_id = 0);
    static Job GetJob(int mod_id);
    static void ClearJob(int mod_id);
    static bool IsBusy();
    static void Cancel();

    // Mods checked so far, of all, how many can be used
    static void CheckProgress(int& checked, int& total, int& usable);

    static bool IsInstalled(int mod_id);
    // Removes what Install put in the game, the models it had come back for the menu
    static bool Remove(int mod_id, std::vector<std::string>& models, std::string& error);
    // The models (resources) a mod put in the game
    static std::vector<std::string> ModelsOf(int mod_id);
private:
    ModBrowser() {};

    static ModBrowser& GetInstance()
    {
        static ModBrowser i{};
        return i;
    }

    struct Installed {
        std::string name;
        int file_id = 0;
        std::vector<std::string> files;     // Relative to csgo/
        std::vector<std::string> models;    // "characters/models/x/y.vmdl"
    };

    void LoadThread();
    void InstallThread(Mod mod, int file_id);
    bool Unpack(const std::filesystem::path& archive, const std::filesystem::path& folder, std::string& error,
        const std::function<bool()>& stop);

    struct ModFiles {
        int64_t modified = 0;
        std::vector<File> files;
    };

    bool GetFiles(int mod_id, int64_t modified, std::vector<File>& files, std::string& error);
    void CheckThread();
    std::optional<FileCheck> CheckFile(const File& file);    // nullopt: not reached now, tried again another time
    std::optional<FileCheck> CheckZip(const File& file);
    FileCheck CheckFolder(const std::filesystem::path& folder);
    void LoadChecks();
    void SaveChecks();
    void LoadInstalled();
    void SaveInstalled();
    void LoadList();
    void SaveList();
    void SetJob(int mod_id, const Job& job);

private:
    std::mutex mutex;
    std::vector<Mod> mods;
    ListState list_state = ListState::IDLE;

    std::map<int, Job> jobs;                // By mod
    std::atomic<bool> busy = false;
    std::atomic<bool> cancel = false;
    std::atomic<bool> stopping = false;

    std::map<int, Installed> installed;     // By mod
    bool installed_loaded = false;

    std::map<int, ModFiles> mod_files;      // By mod, with mutex
    std::map<int, FileCheck> file_checks;   // By file
    struct ModVerdict {
        Mod::Verdict verdict = Mod::Verdict::PENDING;
        std::string note;
        bool own_hands = true;
    };
    std::map<int, ModVerdict> verdicts;     // By mod
    std::vector<int> check_queue;           // Mods in the order of the list
    size_t check_next = 0;
    bool checks_loaded = false;
    bool list_loaded = false;
    bool checks_dirty = false;
    int check_workers = 0;
};
