#pragma once

class Renderer {
public:
    // Where the time of a frame goes, milliseconds averaged over a few frames
    struct FrameTimes {
        float esp = 0.f;
        float overlays = 0.f;
        float menu = 0.f;
        float draw = 0.f;       // Handing the frame to the GPU
        float present = 0.f;    // Waiting for the GPU or V-Sync
        float window = 0.f;     // Keeping the overlay on the game
    };

    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    static bool Init();
    static void Destroy();
    static void Thread();

    // Outdated for the running CS2 (Engine::IsOutdated): a card over the game says so until Insert or Right Shift, or
    // until the game closes, then the window goes. Thread() keeps its window for it when it stopped for that, the
    // features put back what they changed in the meantime. False without a window to show it in
    static bool Notice();

    static bool IsOpen();
    // The menu opened or closed like its key does, from the render thread
    static void SetOpen(bool open);
    static bool IsFocused();
    // The game was closed, the render thread ended for that
    static bool IsGameClosed();
    static const FrameTimes& GetFrameTimes();
private:
    Renderer() {};

    static Renderer& GetInstance()
    {
        static Renderer i{};
        return i;
    }

    bool InitImpl();
    void ThreadImpl();
    void DestroyImpl();
    bool NoticeImpl();
    void RenderNotice();
    void Teardown();

    void Render();
    bool HandleState();
    void SetOpenImpl(bool open);
    bool HandleWindowOrder();
private:
    bool isRunning = true;
    bool isOpen = false;

    bool isFocused = false;
    bool gameClosed = false;
    bool windowUp = false;      // Window, device & ImGui made & not torn down yet
    FrameTimes times;
};