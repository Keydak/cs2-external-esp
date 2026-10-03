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

    static bool IsOpen();
    static bool IsFocused();
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

    void Render();
    bool HandleState();
    bool HandleWindowOrder();
private:
    bool isRunning = true;
    bool isOpen = false;

    bool isFocused = false;
    FrameTimes times;
};