#pragma once
#include <condition_variable>

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
    // when given (JPEG has none). 0 when it fails
    static ImTextureID FromMemory(const void* data, size_t size, const void* alpha = nullptr, size_t alpha_size = 0);
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

private:
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, Entry> entries;
    std::vector<std::string> queue; // Newest first, what is on screen right now matters most
    bool started = false;
};
