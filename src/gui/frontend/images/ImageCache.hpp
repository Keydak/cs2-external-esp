#pragma once
#include <atomic>
#include <condition_variable>
#include <string>
#include <vector>

// Images from the internet as textures for the menu, downloaded in the background & kept on disk
class ImageCache {
public:
    ~ImageCache()                            = default;
    ImageCache(const ImageCache&)            = delete;
    ImageCache(ImageCache&&)                 = delete;
    ImageCache& operator=(const ImageCache&) = delete;
    ImageCache& operator=(ImageCache&&)      = delete;

    // 0 while it is still loading or when it failed, asks for it on the first call
    static ImTextureID Get(const std::string& url);

    // PNG or JPEG bytes made into a texture right away, the gray of another image as its transparency
    // when given (JPEG has none). silhouette: all white, only the shape, to be tinted. 0 when it fails
    static ImTextureID FromMemory(const void* data, size_t size, const void* alpha = nullptr, size_t alpha_size = 0, bool silhouette = false);

    // The smaller picture Steam serves of an item
    static std::string Small(const std::string& url);

    // While the program starts: the pictures of every skin, agent & music kit not on the disk yet are downloaded (once
    // the skin list is there), no textures made. The skin changer then shows them without waiting for the internet
    static void StartPrefetch();
    // Done, or gave up: the program is ready as far as the pictures go
    static bool IsPrefetched();
    // How far, 0 - 1, & what it does
    static float GetPrefetchPercent();
    static std::string GetPrefetchProgress();
private:
    ImageCache() {};

    static ImageCache& GetInstance()
    {
        static ImageCache i{};
        return i;
    }

    enum class State {
        Queued,
        Loaded,
        Failed,
    };

    struct Entry {
        State state = State::Queued;
        ImTextureID texture = 0;
    };

    ImTextureID GetImpl(const std::string& url);
    void Worker();
    ImTextureID Load(const std::string& url);

    void Prefetch();
    void SetPrefetchProgress(const std::string& text);

private:
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, Entry> entries;
    std::vector<std::string> queue; // Newest first, what is on screen right now matters most
    bool started = false;

    std::atomic<bool> prefetch_started = false;
    std::atomic<bool> prefetched = false;
    std::atomic<int> prefetch_done = 0;
    std::atomic<int> prefetch_total = 0;
    std::mutex prefetch_mutex;
    std::string prefetch_progress;
};
