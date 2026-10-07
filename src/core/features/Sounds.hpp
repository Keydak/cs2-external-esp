#pragma once
#include <condition_variable>
#include <deque>
#include <unordered_map>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// Hit & kill sounds, files of the sound folder next to the program. With -insecure the game plays them itself: our
// folder is added to its file system & its "playvol" is called with arguments of ours (not through the console), so
// they mix with the other sounds of the game. Without it, or for .wav & .mp3, Windows plays them through XAudio2
class Sounds {
public:
    ~Sounds()                        = default;
    Sounds(const Sounds&)            = delete;
    Sounds(Sounds&&)                 = delete;
    Sounds& operator=(const Sounds&) = delete;
    Sounds& operator=(Sounds&&)      = delete;

    static bool Init();
    static void Shutdown();

    static std::filesystem::path Folder();

    // File names in the sound folder, looked at again at most once a second
    static std::vector<std::string> List();

    // Volume 0 - 100, nothing for an empty file name. count times at once, on top of each other
    static void Play(const std::string& file, int volume, int count = 1);

    // Opened ahead, so the first time it plays has no delay
    static void Preload(const std::string& file);

    // The sounds of our GitHub repository that are not in the folder yet, on a thread of its own
    static void Download();
    static bool IsDownloading();
    static std::string GetStatus();
private:
    Sounds() {};

    static Sounds& GetInstance()
    {
        static Sounds i{};
        return i;
    }

    void Thread();
    void PlayNow(const std::filesystem::path& source, int volume, int count);
    // A file Windows can play: the source itself, or the sound unpacked from a .vsnd_c. Empty when there is none
    std::wstring Playable(const std::filesystem::path& source);

    // Through the sound system of the game, false when it cannot be used
    bool EnsureGameSound();
    // False when Windows should play it instead: out of a match, or the game did not take the call in time
    bool PlayInGame(const std::filesystem::path& source, int volume, int count);
    // The .vsnd_c copied where the game finds it, its resource name ("sounds/cs2ext/pop"). Empty when it can't be
    std::string GameName(const std::filesystem::path& source);
    void DownloadThread();

    struct Request {
        std::filesystem::path path;
        int volume;
        std::chrono::steady_clock::time_point at{};  // Played from then on
        int count = 1;
    };

    struct Listing {
        std::vector<std::string> files;
        std::chrono::steady_clock::time_point at{};
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;

    std::mutex list_mutex;
    Listing listing;

    std::mutex status_mutex;
    std::string status;

    std::unordered_map<std::string, std::wstring> playable; // Sound thread only
    // Sound thread only: the engine, each sound decoded with a few voices taking turns
    struct Loaded {
        std::vector<uint8_t> pcm;
        std::vector<uint8_t> format;    // WAVEFORMATEX, maybe extensible
        std::vector<IXAudio2SourceVoice*> voices;
        size_t next = 0;
    };
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    std::unordered_map<std::wstring, Loaded> loaded;
    float game_volume = 1.f;

    uintptr_t game_page = 0;        // Our code & the arguments of "playvol" in the game
    bool game_tried = false;
    bool game_ready = false;
    int game_failures = 0;          // In a row, the game path is given up after a few
    bool game_played = false;       // Logged once
    std::unordered_map<std::string, std::string> game_names; // Source -> resource name

    std::atomic<bool> stopping = false;
    std::atomic<bool> running = false;
    std::atomic<bool> downloading = false;
};
